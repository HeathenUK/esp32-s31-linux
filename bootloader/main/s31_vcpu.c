/*
 * s31_vcpu - stage 1 of docs/smp-plan.md: can a FreeRTOS task on hart0 run an
 * S-mode guest, have every M-mode interrupt preempt it transparently, take its
 * ecalls, and leave Wi-Fi, BT and USB exactly as they were?
 *
 * The mechanism is in s31_vcpu_asm.S. This file installs it (our mtvec and a
 * copy of IDF's mtvt with the ordinary-interrupt slots pointed at our entry),
 * opens a PMP window for S-mode, runs the guest from a priority-1 task and
 * reports counters. The guest here is a counting loop, not Linux: the exit
 * test is `make gate` passing while it runs.
 */
#include "s31_memory_layout.h"
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "riscv/csr.h"
#include "riscv/rv_utils.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include "soc/soc.h"

static const char *TAG = "s31_vcpu";

extern void _interrupt_handler(void);
extern uint32_t _mtvt_table[48];
extern void s31_vcpu_mtvec(void);
extern void s31_vcpu_irq_entry(void);
extern int s31_vcpu_enter(void);
extern void s31_vcpu_guest(void);
extern void s31_vcpu_guest2(void);
extern void s31_vcpu_guest3(void);
extern char s31_vcpu_g3_first[], s31_vcpu_g3_last[];
extern void s31_vcpu_mtimer_entry(void);

uint32_t s31_vcpu_area[64];
volatile uint32_t s31_vcpu_irq_count;
volatile uint32_t s31_vcpu_crumbs[16];	/* see s31_vcpu_asm.S; read with devmem */
/* Linux-mode counters. NOT s31_vcpu_crumbs: the assembly owns [1]-[7] of those.
 * [2] last redirected cause [3] redirected exceptions [4] medeleg readback
 * [5] last refused SBI EID [6] refusals [7] shadowed CSR accesses
 * [8] coproc ecalls [14] trapped wfi */
volatile uint32_t s31_vcpu_lx[16];
/* Survives a reset (RTC no-init): [0] last world-switch step (s31_vcpu_world.S
 * marks; 0x0f = finished), [1] last coproc ecall fid+1, [2] coproc ecalls,
 * [3] guest mstatus at the last world_in, [7] magic. Printed by main.c. */
RTC_NOINIT_ATTR volatile uint32_t s31_vcpu_trace[8];
/* World-switch buffers (s31_vcpu_world.S). 33 words: f0-f31, fcsr; the IDF
 * side has a 34th, FreeRTOS's mstatus.FS. The coprocessor block has the
 * layout OpenSBI and the kernel share (HWLoop 0x00, PIE 0x20, 0xf8 bytes). */
uint32_t s31_vcpu_guest_fp[34], s31_vcpu_idf_fp[34];
uint8_t s31_vcpu_guest_cp[256] __attribute__((aligned(16)));
uint8_t s31_vcpu_idf_cp[256] __attribute__((aligned(16)));
uint32_t s31_vcpu_guest_cpst[2], s31_vcpu_idf_cpst[2];	/* HWLP, PIE state CSRs */
volatile uint32_t s31_vcpu_probe[12];	/* stage 2b-0 reach probe results */
static volatile uint32_t guest_counters[4];	/* [0] loops, written by the guest */

/*
 * STAGE 2a: EMULATED INTERRUPT DELIVERY.
 *
 * On this silicon an M-mode interrupt taken while S-mode runs leaves the
 * supervisor interrupt level at a 0xff sentinel that blocks S-mode interrupts
 * until the next sret (our own OpenSBI masks every M interrupt on hart1 for
 * exactly this reason). FreeRTOS's M interrupts cross the guest ~160 times a
 * second here, so hardware S-interrupt delivery cannot be trusted on hart0.
 * Instead the guest's timer and IPI arrive in M-mode (hart0's own mtimecmp on
 * CLIC id 7; the CPU_INTR_FROM_CPU_1 doorbell through an ordinary IDF ISR),
 * set a bit in s31_vcpu_vpending, and s31_vcpu_inject() - called on every
 * return to the guest - performs the trap entry the hardware would have:
 * sepc/scause/sstatus, then the vector from stvt. The guest's sret at the end
 * of its handler also clears the sentinel.
 */
#define VIRQ_TIMER	1u
#define VIRQ_IPI	2u
#define VIRQ_MTIMER_RAW 4u	/* hart0's mtimecmp fired: guest deadline, or our retry */
#define INJECT_RETRY_TICKS 6400u	/* 20 us at 320 MHz */
#define GUEST_ID_TIMER	7u	/* what our kernel's CLIC driver expects */
#define GUEST_ID_IPI	47u	/* patches/0051: the kernel's IPI slot */
#define MST_SIE		(1u << 1)
#define MST_SPIE	(1u << 5)
#define MST_SPP		(1u << 8)
#define MST_MPP_MASK	(3u << 11)
#define MST_MPP_S	(1u << 11)
#define MST_MPRV	(1u << 17)
#define S31_MTIMECMP_LO	0x10004000u
#define S31_MTIMECMP_HI	0x10004004u
#define S31_MTIME_LO	0x1000bff8u
#define S31_MTIME_HI	0x1000bffcu
#define S31_CLIC_ID7	0x1080101cu	/* ip, ie, attr, ctl bytes */
#define S31_FROM_CPU_1	0x20586014u

volatile uint32_t s31_vcpu_vpending;
volatile uint32_t s31_vcpu_g2_counters[8];
static volatile uint32_t inj_timer, inj_ipi, inj_deferred, emul_rdtime, sbi_calls;

static uint32_t IRAM_ATTR guest_load32(uint32_t vaddr)
{
	/* Read through the GUEST's address translation (MPRV with MPP = S),
	 * as OpenSBI does for unprivileged access. */
	uint32_t old = RV_READ_CSR(mstatus), v;

	RV_WRITE_CSR(mstatus, (old & ~MST_MPP_MASK) | MST_MPP_S | MST_MPRV);
	v = *(volatile uint32_t *)(uintptr_t)vaddr;
	RV_WRITE_CSR(mstatus, old);
	return v;
}

static uint64_t IRAM_ATTR mtime_now(void)
{
	uint32_t hi, lo, t;

	do {
		hi = REG_READ(S31_MTIME_HI);
		lo = REG_READ(S31_MTIME_LO);
		t = REG_READ(S31_MTIME_HI);
	} while (hi != t);
	return ((uint64_t)hi << 32) | lo;
}

static volatile uint64_t guest_deadline = ~0ull;

static void IRAM_ATTR mtimer_arm(uint64_t when)
{
	volatile uint8_t *c = (volatile uint8_t *)S31_CLIC_ID7;

	REG_WRITE(S31_MTIMECMP_HI, 0xffffffffu);
	c[0] = 0;			/* clear the edge latch */
	c[2] = 0xc3;			/* M-mode, edge, hardware vectored */
	c[3] = 0x3f;			/* IDF's ordinary level: critical sections mask it */
	c[1] = 1;
	REG_WRITE(S31_MTIMECMP_LO, (uint32_t)when);
	REG_WRITE(S31_MTIMECMP_HI, (uint32_t)(when >> 32));
}

static void IRAM_ATTR guest_set_timer(uint64_t when)
{
	s31_vcpu_vpending &= ~VIRQ_TIMER;
	guest_deadline = when;
	mtimer_arm(when);
}

static void vcpu_inject(uint32_t *area, int force);
static volatile uint32_t inject_force_next;

void IRAM_ATTR s31_vcpu_inject(uint32_t *area)
{
	int force = inject_force_next;

	inject_force_next = 0;
	vcpu_inject(area, force);
}

static void IRAM_ATTR vcpu_inject(uint32_t *area, int force)
{
	uint32_t pend = s31_vcpu_vpending, mst = area[32], id, bit;

	/* hart0's one mtimecmp serves two masters: the guest's deadline and
	 * our own injection retry. Sort out which this was. */
	if (pend & VIRQ_MTIMER_RAW) {
		pend &= ~VIRQ_MTIMER_RAW;
		if (mtime_now() >= guest_deadline) {
			guest_deadline = ~0ull;
			pend |= VIRQ_TIMER;
		} else if (guest_deadline != ~0ull) {
			mtimer_arm(guest_deadline);
		}
		s31_vcpu_vpending = pend;
	}
	if (!pend)
		return;
	/* In U-mode supervisor interrupts are always deliverable. */
	if (!force && !(mst & MST_SIE) && (mst & MST_MPP_MASK) == MST_MPP_S) {
		/*
		 * Linux idles in wfi with interrupts OFF and expects the
		 * hardware to take the pending one the instant it turns them
		 * back on. We only get to look when an M-mode interrupt
		 * crosses, and those all land inside the wfi - so without this
		 * every crossing deferred and the CPU never took an IPI
		 * (2026-09-20: hart1 stuck in smp_call_function_many_cond).
		 * Look again in 20 us.
		 */
		uint64_t retry = mtime_now() + INJECT_RETRY_TICKS;

		mtimer_arm(retry < guest_deadline ? retry : guest_deadline);
		inj_deferred++;
		return;
	}
	if (pend & VIRQ_TIMER) {
		bit = VIRQ_TIMER; id = GUEST_ID_TIMER; inj_timer++;
	} else {
		bit = VIRQ_IPI; id = GUEST_ID_IPI; inj_ipi++;
	}
	s31_vcpu_vpending = pend & ~bit;
	if (pend & ~bit) {		/* another is waiting behind this one */
		uint64_t retry = mtime_now() + INJECT_RETRY_TICKS;

		mtimer_arm(retry < guest_deadline ? retry : guest_deadline);
	}

	RV_WRITE_CSR(sepc, area[31]);
	RV_WRITE_CSR(scause, 0x80000000u | id);
	/* SPP = the mode we interrupted, SPIE = 1 (SIE was set), SIE = 0,
	 * and the handler itself runs in S-mode. */
	mst &= ~(MST_SPP | MST_SIE | MST_MPP_MASK);
	if ((area[32] & MST_MPP_MASK) == MST_MPP_S)
		mst |= MST_SPP;
	if (area[32] & MST_SIE)
		mst |= MST_SPIE;
	else
		mst &= ~MST_SPIE;
	mst |= MST_MPP_S;
	area[32] = mst;
	/* Linux runs the CLIC non-vectored (no stvt, SHV clear): everything
	 * enters at stvec's base. The test guest has a vector table. */
	if (RV_READ_CSR(0x107 /* stvt */))
		area[31] = guest_load32(RV_READ_CSR(0x107) + 4u * id) & ~1u;
	else
		area[31] = RV_READ_CSR(stvec) & ~0x3fu;
}

static TaskHandle_t vcpu_handle;

static void IRAM_ATTR ipi_isr(void *arg)
{
	BaseType_t woken = pdFALSE;

	(void)arg;
	REG_WRITE(S31_FROM_CPU_1, 0);
	s31_vcpu_vpending |= VIRQ_IPI;
	if (vcpu_handle) {		/* it may be blocked in guest_idle() */
		vTaskNotifyGiveFromISR(vcpu_handle, &woken);
		if (woken)
			portYIELD_FROM_ISR();
	}
}

/*
 * The guest executed wfi with nothing pending: LEND THE TIME BACK. Until this
 * existed the idle CPU spun through the trap 460,000 times a second - hart0's
 * IDLE task never ran, and the spinning (kernel text from flash, data from
 * PSRAM, on the bus hart 1 shares) cost hart 1's benchmarks ~11% (gate canaries
 * 16.3 vs 14.7 ms, 2026-09-20). We are the vCPU task here, in M-mode on its own
 * stack with mscratch = 0 and FreeRTOS's coprocessor state already restored
 * (world_out), so blocking is legal. An IPI notifies us from its ISR at once.
 * The guest's timer is a raw vector and cannot notify, so: deadline far away
 * -> block a tick at a time; deadline near -> sleep the hart in M-mode wfi with
 * interrupts on, which the timer wakes to the microsecond.
 */
#define S31_GUEST_IDLE_BLOCKS	1
#define GUEST_IDLE_NEAR_TICKS	(25u * 320000u)		/* 25 ms at 320 MHz */
void IRAM_ATTR s31_vcpu_guest_idle(void)	/* from s31_vcpu_idle_stub: task context, MIE on */
{
	while (!s31_vcpu_vpending) {
		uint64_t now = mtime_now();
		int far = guest_deadline == ~0ull ||
			  (guest_deadline > now && guest_deadline - now > GUEST_IDLE_NEAR_TICKS);

		if (far)
			ulTaskNotifyTake(pdTRUE, 1);
		else
			__asm__ volatile ("wfi");
		s31_vcpu_lx[13]++;
	}
}

static volatile uint32_t trap_ecalls, trap_other, last_cause, last_epc, last_tval;
static uint32_t my_mtvt[48] __attribute__((aligned(64)));
static uint8_t guest_stack[1024];
static uint32_t leave_after = 0;
static volatile uint32_t linux_mode;
static volatile uint32_t snap_mintstatus, snap_mintthresh, snap_mstatus, snap_mcause;

/* Called from s31_vcpu_mtvec with the guest's state saved in `area`.
 * Return 0 to resume the guest, non-zero to make s31_vcpu_enter() return. */
int IRAM_ATTR s31_vcpu_trap(uint32_t *area)
{
	uint32_t cause = RV_READ_CSR(mcause) & 0xfff;	/* CLIC: mask the rest */

	if (cause == 2) {			/* illegal instruction: rdtime[h]? */
		uint32_t insn = RV_READ_CSR(mtval);

		if (!insn)
			insn = guest_load32(area[31]);
		/*
		 * sie/sip (and the h halves) do not exist on this CLIC-only
		 * core, and Linux's entry code touches them before it knows
		 * anything (secondary_start_sbi opens with csrw sie, zero).
		 * Our OpenSBI shadows them in software for hart1
		 * (sbi_emulate_csr.c); do the same here. Interrupt delivery on
		 * this hart is emulated and gated on sstatus.SIE alone.
		 */
		/*
		 * WFI, trapped by mstatus.TW. Linux idles in wfi with
		 * interrupts OFF and opens them for a few microseconds after it
		 * returns; a CPU whose interrupts are injected only when an
		 * M-mode interrupt crosses is then always sampled INSIDE the wfi
		 * with SIE = 0 and never takes anything (2026-09-20: 437,988
		 * deferrals, zero progress). So wfi is where an idle CPU takes
		 * its pending interrupt: the handler's sret comes back to the
		 * instruction after the wfi with interrupts still off, exactly
		 * the state the idle loop expects. Nothing pending: return at
		 * once (a polling idle) - TODO lend the time to FreeRTOS.
		 */
		if (linux_mode && insn == 0x10500073u) {
			area[31] += 4;
			s31_vcpu_lx[14]++;
			inject_force_next = 1;	/* whatever wakes it is taken AT the wfi */
			if (S31_GUEST_IDLE_BLOCKS && !s31_vcpu_vpending)
				return 5;	/* asm leaves the exception, then idles */
			return 0;		/* resume -> s31_vcpu_inject, forced */
		}
		if ((insn & 0x7f) == 0x73 && (insn & 0x7000)) {
			uint32_t csr = insn >> 20, f3 = (insn >> 12) & 7;
			uint32_t rd = (insn >> 7) & 31, rs = (insn >> 15) & 31;
			static uint32_t shadow[4];
			int k = csr == 0x104 ? 0 : csr == 0x144 ? 1 :
				csr == 0x114 ? 2 : csr == 0x154 ? 3 : -1;

			if (k >= 0) {
				uint32_t old = shadow[k];
				uint32_t src = (f3 & 4) ? rs : (rs ? area[rs - 1] : 0);

				switch (f3 & 3) {
				case 1: shadow[k] = src; break;			/* csrrw */
				case 2: if (rs) shadow[k] = old | src; break;	/* csrrs */
				case 3: if (rs) shadow[k] = old & ~src; break;	/* csrrc */
				}
				if (rd)
					area[rd - 1] = old;
				area[31] += 4;
				s31_vcpu_lx[7]++;		/* shadowed CSR accesses */
				return 0;
			}
		}
		if ((insn & 0xfff0707fu) == 0xc0102073u ||	/* csrrs rd, time, x0 */
		    (insn & 0xfff0707fu) == 0xc8102073u) {	/* ... timeh */
			uint32_t rd = (insn >> 7) & 31;
			uint64_t now = mtime_now();

			if (rd)
				area[rd - 1] = (insn & 0x08000000u) ? (uint32_t)(now >> 32)
								    : (uint32_t)now;
			area[31] += 4;
			emul_rdtime++;
			return 0;
		}
	}
	if (cause == 9) {			/* ecall from S-mode */
		trap_ecalls++;
		area[31] += 4;			/* step over the ecall */
		sbi_calls++;
		if (area[16] == 0x54494D45u && area[15] == 0) {	/* a7 TIME, a6 fid 0 */
			guest_set_timer(((uint64_t)area[10] << 32) | area[9]);
			area[9] = 0;		/* SBI_SUCCESS */
			area[10] = 0;
			if (s31_vcpu_crumbs[15])
				return 4;
			return 0;
		}
		/*
		 * Vendor coprocessor extension (arch/riscv/kernel/esp32s31-ext.c;
		 * hart1's copy is in our OpenSBI). The guest's HWLoop/PIE state
		 * is already swapped out to s31_vcpu_guest_cp by world_out, so
		 * these are copies; world_in loads the result. RESTORE turns the
		 * units on (CLEAN), as the OpenSBI routine leaves them.
		 */
		if (linux_mode && area[16] == 0x09000002u && area[15] <= 2) {
			uint32_t fid = area[15], a = area[9], b = area[10];
			int bad = (a & 0xf) || a < 0x50000000u || a > 0x50fe0000u - 0x100;

			if (fid == 0)
				bad |= (b & 0xf) || b < 0x50000000u || b > 0x50fe0000u - 0x100;
			s31_vcpu_lx[8]++;
			((volatile uint32_t *)0x50FEFFD0u)[1] = fid + 1;
			s31_vcpu_trace[1] = fid + 1;
			s31_vcpu_trace[2]++;
			((volatile uint32_t *)0x50FEFFD0u)[2]++;
			if (bad) {
				area[9] = (uint32_t)-5;		/* SBI_ERR_INVALID_ADDRESS */
				return 0;
			}
			if (fid == 0 || fid == 1)
				memcpy((void *)(uintptr_t)a, s31_vcpu_guest_cp, 0xf8);
			if (fid == 0 || fid == 2) {
				memcpy(s31_vcpu_guest_cp, (void *)(uintptr_t)(fid ? a : b), 0xf8);
				s31_vcpu_guest_cpst[0] = 2;
				s31_vcpu_guest_cpst[1] = 0;	/* no PIE on hart0; see s31_vcpu_world.S */
			}
			area[9] = 0;
			area[10] = 0;
			return 0;
		}
		/*
		 * SBI IPI send_ipi from the lent CPU: the only other hart is
		 * hart 1, whose Linux IPI is a software-set pending bit on its
		 * CLIC slot 47 (patches/0051). Set it from M-mode through the
		 * cross-hart alias of the M window.
		 */
		if (linux_mode && area[16] == 0x735049u && area[15] == 0) {
			if (area[9] & 2u) {
				*(volatile uint8_t *)(uintptr_t)(0x20801000u + 0x10000u + 47u * 4u) = 1;
				s31_vcpu_lx[9]++;
			}
			area[9] = 0;
			area[10] = 0;
			return 0;
		}
		if (linux_mode) {
			/* The mini-SBI of a Linux CPU on hart0: TIME above, and
			 * an honest "no" to everything else. Remember who asked. */
			s31_vcpu_lx[5] = area[16];	/* last refused EID */
			s31_vcpu_lx[6]++;
			area[9] = (uint32_t)-2;		/* SBI_ERR_NOT_SUPPORTED */
			area[10] = 0;
			return 0;
		}
		/* Bisect step A: three calls prove S-mode runs and the
		 * exception path round-trips; then leave so the task can say so. */
		if (leave_after && trap_ecalls >= leave_after)
			return 2;
		if (s31_vcpu_crumbs[15])	/* Linux: devmem <crumbs+60> 32 1 */
			return 4;
		/*
		 * Bisect step B: the guest calls in every million loops. If
		 * the FreeRTOS tick has not moved across several calls, M-mode
		 * interrupts are not reaching us while the guest runs - leave
		 * and let the task dump the interrupt controller's state.
		 */
		{
			static uint32_t last_tick, stuck;
			uint32_t now = xTaskGetTickCountFromISR();

			if (now == last_tick) {
				if (++stuck >= 40) {
					snap_mintstatus = RV_READ_CSR(0xFB1);
					snap_mintthresh = RV_READ_CSR(0x347);
					snap_mstatus = area[32];
					snap_mcause = RV_READ_CSR(mcause);
					return 3;
				}
			} else {
				stuck = 0;
				last_tick = now;
			}
		}
		return 0;
	}
	/* Stage 2b-0 reach probe: report the fault in a0 and carry on. */
	if (!linux_mode && (cause == 1 || cause == 5 || cause == 7) &&
	    ((area[31] >= (uint32_t)(uintptr_t)s31_vcpu_g3_first &&
	      area[31] <= (uint32_t)(uintptr_t)s31_vcpu_g3_last) || cause == 1)) {
		area[9] = 0xFA170000u | cause;
		area[31] = cause == 1 ? area[0] : area[31] + 4;	/* ra, or skip */
		return 0;
	}
	/*
	 * medeleg reads back 0xb100 on this core: causes 0-7 (misaligned, access
	 * faults, illegal instruction, BREAKPOINT) cannot be delegated, so they
	 * arrive here even though they are Linux's to handle - a WARN() on this
	 * CPU is an ebreak. Do what OpenSBI's sbi_trap_redirect() does for
	 * hart1: perform the S-mode trap entry by hand. Exceptions always enter
	 * at stvec's base.
	 */
	if (linux_mode && cause < 8) {
		uint32_t mst = area[32];

		RV_WRITE_CSR(sepc, area[31]);
		RV_WRITE_CSR(scause, cause);
		RV_WRITE_CSR(stval, RV_READ_CSR(mtval));
		mst &= ~(MST_SPP | MST_SPIE | MST_MPP_MASK);
		if ((area[32] & MST_MPP_MASK) == MST_MPP_S)
			mst |= MST_SPP;
		if (area[32] & MST_SIE)
			mst |= MST_SPIE;
		mst &= ~MST_SIE;
		mst |= MST_MPP_S;
		area[32] = mst;
		area[31] = RV_READ_CSR(stvec) & ~0x3fu;
		if (cause == 2) {	/* what did the units look like when it faulted? */
			/* IDF's handler always clears EXT_ILL (0x7F0, the reason an
			 * extension instruction was refused: 1 FPU, 2 HWLP, 4 PIE).
			 * We never did. Clear it, and remember what it said. */
			uint32_t ext_ill = RV_READ_CSR(0x7F0);

			RV_WRITE_CSR(0x7F0, 0);
			s31_vcpu_lx[12] = ext_ill;
			s31_vcpu_lx[10] = s31_vcpu_guest_cpst[0] | (s31_vcpu_guest_cpst[1] << 8) |
					  ((area[32] & 0x6000u) << 3) | 0x80000000u;
			s31_vcpu_lx[11] = RV_READ_CSR(mtval);
		}
		s31_vcpu_lx[3]++;			/* redirected exceptions */
		s31_vcpu_lx[2] = cause;
		return 0;
	}
	s31_vcpu_crumbs[8] = cause;
	s31_vcpu_crumbs[9] = area[31];
	s31_vcpu_crumbs[10] = RV_READ_CSR(mtval);
	trap_other++;
	last_cause = cause;
	last_epc = area[31];
	last_tval = RV_READ_CSR(mtval);
	return 1;				/* anything else ends the experiment */
}

static void vcpu_task(void *arg)
{
	int i, reason;

	(void)arg;
	/*
	 * Let IDLE run first. app_main's task deletes itself right after
	 * starting us, and only the IDLE task frees a deleted task's stack; a
	 * guest that never sleeps starves IDLE, the heap stays short, and
	 * esp_wifi_init() then fails with "create wifi task: failed"
	 * (ESP_ERR_NO_MEM, seen 2026-09-20). Real Linux will return the hart to
	 * FreeRTOS whenever it idles (WFI traps); this stub never idles.
	 */
#if CONFIG_S31_VCPU_LINUX
	vTaskDelay(pdMS_TO_TICKS(50));	/* Linux is waiting for this CPU: be there */
#else
	vTaskDelay(pdMS_TO_TICKS(1000));
#endif
#if !CONFIG_S31_VCPU_LINUX
	ESP_LOGW(TAG, "pmpcfg %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
		 (uint32_t)RV_READ_CSR(pmpcfg0), (uint32_t)RV_READ_CSR(pmpcfg1),
		 (uint32_t)RV_READ_CSR(pmpcfg2), (uint32_t)RV_READ_CSR(pmpcfg3));
	ESP_LOGW(TAG, "pmpaddr0-3 %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
		 "  12-15 %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
		 (uint32_t)RV_READ_CSR(pmpaddr0), (uint32_t)RV_READ_CSR(pmpaddr1),
		 (uint32_t)RV_READ_CSR(pmpaddr2), (uint32_t)RV_READ_CSR(pmpaddr3),
		 (uint32_t)RV_READ_CSR(pmpaddr12), (uint32_t)RV_READ_CSR(pmpaddr13),
		 (uint32_t)RV_READ_CSR(pmpaddr14), (uint32_t)RV_READ_CSR(pmpaddr15));
#endif

	/* Our vector table: IDF's, with every ordinary slot entering through us. */
	for (i = 0; i < 48; i++)
		my_mtvt[i] = _mtvt_table[i] == (uint32_t)(uintptr_t)_interrupt_handler ?
			     (uint32_t)(uintptr_t)s31_vcpu_irq_entry : _mtvt_table[i];

	my_mtvt[7] = (uint32_t)(uintptr_t)s31_vcpu_mtimer_entry;
	if (esp_intr_alloc(ETS_CPU_INTR_FROM_CPU_1_SOURCE, 0, ipi_isr, NULL, NULL) != ESP_OK)
		ESP_LOGE(TAG, "could not take the FROM_CPU_1 doorbell");
	RV_SET_CSR(mcounteren, 2);	/* let S-mode try rdtime natively */

#if CONFIG_S31_VCPU_LINUX
	{
		volatile uint32_t *mb = (volatile uint32_t *)(uintptr_t)S31_HART0_START_MAILBOX;

		/* Nothing to do until Linux on hart1 asks OpenSBI for hart 0. */
		while (mb[0] != S31_HART0_START_MAGIC)
			vTaskDelay(pdMS_TO_TICKS(20));
		linux_mode = 1;
		memset((void *)s31_vcpu_trace, 0, sizeof(s31_vcpu_trace));
		s31_vcpu_trace[7] = 0x54524143;	/* "TRAC" */
		/* Exceptions Linux handles itself: everything but ecall-from-S. */
		RV_WRITE_CSR(medeleg, 0xb1ff);
		s31_vcpu_lx[4] = RV_READ_CSR(medeleg);
		RV_WRITE_CSR(satp, 0);
		memset(s31_vcpu_area, 0, sizeof(s31_vcpu_area));
		s31_vcpu_area[9] = 0;			/* a0 = hartid */
		s31_vcpu_area[10] = mb[2];		/* a1 = opaque */
		s31_vcpu_area[31] = mb[1];		/* mepc = secondary_start_sbi */
		s31_vcpu_area[32] = (RV_READ_CSR(mstatus) & ~0x1888u) | 0x0880u | (1u << 21); /* TW */
		mb[0] = 0;
		mb[3]++;
		goto enter;
	}
#endif
	{
		/* 2b-1: does a PIE / HWLoop enable STICK on hart0, in M-mode,
		 * and does mstatus.FS matter? (Linux mode read PIE back as 0.) */
		uint32_t r[8], fs = RV_READ_CSR(mstatus) & 0x6000u;

		RV_CLEAR_CSR(mstatus, 0x6000);
		RV_WRITE_CSR(0x7F2, 1); r[0] = RV_READ_CSR(0x7F2);
		RV_WRITE_CSR(0x7F2, 2); r[1] = RV_READ_CSR(0x7F2);
		RV_WRITE_CSR(0x7F2, 3); r[2] = RV_READ_CSR(0x7F2);
		RV_WRITE_CSR(0x7F2, 0);
		RV_SET_CSR(mstatus, 0x2000);
		RV_WRITE_CSR(0x7F2, 1); r[3] = RV_READ_CSR(0x7F2);
		RV_WRITE_CSR(0x7F2, 2); r[4] = RV_READ_CSR(0x7F2);
		RV_WRITE_CSR(0x7F2, 0);
		RV_WRITE_CSR(0x7F1, 1); r[5] = RV_READ_CSR(0x7F1);
		RV_WRITE_CSR(0x7F1, 2); r[6] = RV_READ_CSR(0x7F1);
		RV_CLEAR_CSR(mstatus, 0x6000);
		RV_SET_CSR(mstatus, fs);
		ESP_LOGW(TAG, "2b-1 M-mode: PIE w1->%" PRIx32 " w2->%" PRIx32 " w3->%" PRIx32
			 " | FS on: w1->%" PRIx32 " w2->%" PRIx32 " | HWLP w1->%" PRIx32 " w2->%" PRIx32,
			 r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
		/* and let the S-mode guest try them: both units INITIAL */
		s31_vcpu_guest_cpst[0] = 1;
		s31_vcpu_guest_cpst[1] = 1;
	}
	memset(s31_vcpu_area, 0, sizeof(s31_vcpu_area));
	s31_vcpu_area[1] = (uint32_t)(uintptr_t)(guest_stack + sizeof(guest_stack)); /* x2 */
	s31_vcpu_area[9] = (uint32_t)(uintptr_t)guest_counters;			     /* a0 */
	s31_vcpu_area[31] = (uint32_t)(uintptr_t)s31_vcpu_guest3;		     /* mepc */
	/* mstatus for the guest: MPP = S (01), MPIE = 1, everything else as now. */
	s31_vcpu_area[32] = (RV_READ_CSR(mstatus) & ~0x1888u) | 0x0880u;

#if CONFIG_S31_VCPU_LINUX
enter:
#endif
	portDISABLE_INTERRUPTS();
	RV_WRITE_CSR(mscratch, 0);
	RV_WRITE_CSR(0x307 /* mtvt */, (uint32_t)(uintptr_t)my_mtvt);
	RV_WRITE_CSR(mtvec, ((uint32_t)(uintptr_t)s31_vcpu_mtvec) | 3);
	portENABLE_INTERRUPTS();
#if !CONFIG_S31_VCPU_LINUX
	ESP_LOGW(TAG, "vectors installed; entering the S-mode guest at %p, crumbs at %p",
		 (void *)s31_vcpu_guest, (void *)s31_vcpu_crumbs);
#endif

#ifdef S31_VCPU_VECTORS_ONLY		/* bisect: our vectors, no guest at all */
	ESP_LOGW(TAG, "VECTORS ONLY: not entering the guest");
	vTaskDelete(NULL);
#endif
	reason = s31_vcpu_enter();
	/* SILENT by default: hart0 and Linux share the console UART, and a log
	 * line from here landing mid-transmission is the prime suspect for
	 * Linux's console dying at the leave (2026-09-20). The outcome goes in
	 * the breadcrumbs: [11] = reason, [12] = loops, [13] = preemptions. */
	s31_vcpu_crumbs[11] = (uint32_t)reason;
	s31_vcpu_crumbs[12] = guest_counters[0];
	s31_vcpu_crumbs[13] = s31_vcpu_irq_count;
	if (reason != 3)
		vTaskDelete(NULL);
	ESP_LOGE(TAG, "guest left: reason %d cause %" PRIu32 " epc %08" PRIx32
		 " tval %08" PRIx32 " after %" PRIu32 " loops, %" PRIu32 " ecalls, %"
		 PRIu32 " preemptions", reason, last_cause, last_epc, last_tval,
		 guest_counters[0], trap_ecalls, s31_vcpu_irq_count);
	if (reason == 3) {
		/* CLIC per-interrupt bytes for this hart: IP, IE, ATTR, CTL. */
		const volatile uint8_t *c = (const volatile uint8_t *)0x20801000;
		int k;

		ESP_LOGE(TAG, "TICK STALLED in guest: mintstatus %08" PRIx32 " mintthresh %08"
			 PRIx32 " guest mstatus %08" PRIx32 " mcause %08" PRIx32,
			 snap_mintstatus, snap_mintthresh, snap_mstatus, snap_mcause);
		for (k = 0; k < 48; k++)
			if (c[k * 4 + 1])	/* enabled */
				ESP_LOGE(TAG, "  clic id %d: ip %u ie %u attr %02x ctl %02x", k,
					 c[k * 4], c[k * 4 + 1], c[k * 4 + 2], c[k * 4 + 3]);
	}
	vTaskDelete(NULL);
}

static void report_task(void *arg)
{
	int n;

	(void)arg;
#if CONFIG_S31_VCPU_LINUX
	/*
	 * SILENT. hart0's log and Linux's console share one UART, Linux drives
	 * it through UHCI DMA, and a hart0 line landing while Linux prints has
	 * twice left Linux's console dead after its first line (2026-09-20).
	 * The counters go to the PSRAM words after the start mailbox; Linux's
	 * s31-smp heartbeat prints them.
	 */
	for (;;) {
		volatile uint32_t *m = (volatile uint32_t *)0x50FEFFD0u;

		/* 2026-09-21: CPU1 found spinning in do_raw_spin_lock on stalled boots.
		 * Which lock, and who called? The heartbeat prints these three as
		 * "coproc ecalls / redirected / cause" in DECIMAL - convert on the host. */
		m[3] = s31_vcpu_area[9];		/* guest a0 = the lock, in do_raw_spin_lock */
		m[4] = s31_vcpu_area[0];		/* guest ra */
		m[5] = s31_vcpu_area[31];		/* guest pc */
		m[6] = s31_vcpu_guest_cpst[0] | (s31_vcpu_guest_cpst[1] << 8);
		m[7] = inj_timer;
		m[8] = inj_ipi;
		m[9] = inj_deferred;
		/* m[10] is written by world_in: readback of the enables */
		m[11] = (s31_vcpu_lx[12] << 24) | (s31_vcpu_lx[10] & 0xffffffu);	/* EXT_ILL | state at last illegal */
		vTaskDelay(pdMS_TO_TICKS(500));
	}
#endif
	for (n = 0; n < 8; n++) {
		vTaskDelay(pdMS_TO_TICKS(n < 2 ? 5000 : 30000));
		ESP_LOGW(TAG, "2a: guest timer irqs %" PRIu32 " ipis %" PRIu32 " | injected t %" PRIu32
			 " i %" PRIu32 " deferred %" PRIu32 " | rdtime emulated %" PRIu32 " sbi %" PRIu32
			 " | g2 loops %" PRIu32 ", counters at %p",
			 s31_vcpu_g2_counters[1], s31_vcpu_g2_counters[2], inj_timer, inj_ipi,
			 inj_deferred, emul_rdtime, sbi_calls, s31_vcpu_g2_counters[0],
			 (void *)s31_vcpu_g2_counters);
		REG_WRITE(0x2d0020cc, 3);	/* bus monitor: record hart1's PC/SP */
		ESP_LOGW(TAG, "hart1 pc %08" PRIx32 " sp %08" PRIx32 " | coproc ecalls %" PRIu32
			 " ipi->hart1 %" PRIu32 " wfi %" PRIu32 " redirected %" PRIu32 " (last cause %" PRIu32 ") guest cpst %"
			 PRIu32 "/%" PRIu32,
			 (uint32_t)REG_READ(0x2d0020d0), (uint32_t)REG_READ(0x2d0020d4),
			 s31_vcpu_lx[8], s31_vcpu_lx[9], s31_vcpu_lx[14], s31_vcpu_lx[3], s31_vcpu_lx[2],
			 s31_vcpu_guest_cpst[0], s31_vcpu_guest_cpst[1]);
		ESP_LOGW(TAG, "guest pc %08" PRIx32 " ra %08" PRIx32 " sp %08" PRIx32 " mstatus %08" PRIx32
			 " vpending %" PRIu32 " medeleg %08" PRIx32 " shadowed-csr %" PRIu32
			 " refused-eid %08" PRIx32 " x%" PRIu32,
			 s31_vcpu_area[31], s31_vcpu_area[0], s31_vcpu_area[1], s31_vcpu_area[32],
			 (uint32_t)s31_vcpu_vpending, s31_vcpu_lx[4], s31_vcpu_lx[7],
			 s31_vcpu_lx[5], s31_vcpu_lx[6]);
		ESP_LOGW(TAG, "2b-0 reach: psram %08" PRIx32 " xipflash %08" PRIx32 " sclic %08" PRIx32
			 " sclic+64k %08" PRIx32 " uart %08" PRIx32 " fromcpu1 %08" PRIx32
			 " mtime %08" PRIx32 " | psram-w %08" PRIx32 " flash-x %08" PRIx32
			 " done %08" PRIx32 " | ext step %" PRIu32,
			 s31_vcpu_probe[0], s31_vcpu_probe[1], s31_vcpu_probe[2], s31_vcpu_probe[3],
			 s31_vcpu_probe[4], s31_vcpu_probe[5], s31_vcpu_probe[6], s31_vcpu_probe[7],
			 s31_vcpu_probe[8], s31_vcpu_probe[9], s31_vcpu_probe[10]);
		ESP_LOGW(TAG, "guest loops %" PRIu32 " ecalls %" PRIu32
			 " preempted by %" PRIu32 " M-mode interrupts, other traps %" PRIu32
			 " (last cause %" PRIu32 " epc %08" PRIx32 " tval %08" PRIx32 ")",
			 guest_counters[0], trap_ecalls, s31_vcpu_irq_count, trap_other,
			 last_cause, last_epc, last_tval);
	}
	vTaskDelete(NULL);
}

extern void s31_vcpu_bisect_pad(void);
void (*s31_vcpu_bisect_keep)(void) = s31_vcpu_bisect_pad;

/*
 * H1 PC SAMPLER: hart0 as a logic analyser for hart 1.
 *
 * There is no perf/ftrace/PMU here, /proc/profile only sees kernel code that
 * runs with interrupts ON (96 samples in a 25 s run, 2026-09-21), and pcsample
 * ptrace-stops its target. The bus monitor records hart 1's PC continuously
 * and hart0 can read it without hart 1 knowing - kernel, user, interrupts off,
 * all of it, on a UP or an SMP kernel alike.
 *
 *   control word  s31_h1s_ctrl : Linux writes N (1..4000) to start; we write
 *                              0x80000000|count when done
 *   samples       s31_h1s_buf  : count x u32 PC (internal SRAM, 16 kB)
 *   Linux:  devmem 0x50FEFFB0 32 4000 ; <workload> ;
 *           dd if=/dev/mem bs=4096 skip=$((0x50FEC000/4096)) count=4 | ...
 * 1 kHz from an esp_timer: 4000 samples = 4 s.
 */
#include "esp_timer.h"
/* In INTERNAL SRAM, not PSRAM: devmem maps PSRAM as cached RAM, so Linux's
 * write of the control word sat in hart 1's D-cache and hart0 never saw it
 * (2026-09-21). SRAM is uncached for both harts. Addresses: nm hello_world.elf
 * | grep s31_h1s_  (they move with every loader build). */
volatile uint32_t s31_h1s_ctrl;
volatile uint32_t s31_h1s_buf[4000];
#define H1S_CTRL	(&s31_h1s_ctrl)
#define H1S_BUF		s31_h1s_buf
static volatile uint32_t h1s_want, h1s_have;

static void IRAM_ATTR h1s_tick(void *arg)
{
	(void)arg;
	if (h1s_have < h1s_want)
		H1S_BUF[h1s_have++] = REG_READ(0x2d0020d0);
}

static void h1s_task(void *arg)
{
	const esp_timer_create_args_t a = { .callback = h1s_tick, .name = "h1s" };
	esp_timer_handle_t t;

	(void)arg;
	if (esp_timer_create(&a, &t) != ESP_OK)
		vTaskDelete(NULL);
	*H1S_CTRL = 0;
	for (;;) {
		uint32_t n = *H1S_CTRL;

		if (n >= 1 && n <= 4000) {
			REG_WRITE(0x2d0020cc, 3);	/* record hart 1 */
			h1s_have = 0;
			h1s_want = n;
			esp_timer_start_periodic(t, 1000);
			while (h1s_have < h1s_want)
				vTaskDelay(pdMS_TO_TICKS(20));
			esp_timer_stop(t);
			*H1S_CTRL = 0x80000000u | h1s_have;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void s31_vcpu_start(void)
{
	/* Priority 1: above IDLE (whose hook sleeps the hart for a whole tick),
	 * below every IDF system, radio and USB task. */
	xTaskCreate(vcpu_task, "s31_vcpu", 4096, NULL, 1, &vcpu_handle);	/* blocks inside the trap path */
	xTaskCreate(h1s_task, "s31_h1s", 3072, NULL, 3, NULL);
	xTaskCreate(report_task, "s31_vcpu_rep", 2560, NULL, 2, NULL);
}
