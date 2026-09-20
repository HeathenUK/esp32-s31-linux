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

void IRAM_ATTR s31_vcpu_inject(uint32_t *area)
{
	vcpu_inject(area, 0);
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

static void IRAM_ATTR ipi_isr(void *arg)
{
	(void)arg;
	REG_WRITE(S31_FROM_CPU_1, 0);
	s31_vcpu_vpending |= VIRQ_IPI;
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
			s31_vcpu_crumbs[14]++;
			vcpu_inject(area, 1);
			return 0;
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
				s31_vcpu_crumbs[7]++;		/* shadowed CSR accesses */
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
		if (linux_mode) {
			/* The mini-SBI of a Linux CPU on hart0: TIME above, and
			 * an honest "no" to everything else. Remember who asked. */
			s31_vcpu_crumbs[5] = area[16];	/* last refused EID */
			s31_vcpu_crumbs[6]++;
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
		s31_vcpu_crumbs[3]++;			/* redirected exceptions */
		s31_vcpu_crumbs[2] = cause;
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
	vTaskDelay(pdMS_TO_TICKS(1000));
	ESP_LOGW(TAG, "pmpcfg %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
		 (uint32_t)RV_READ_CSR(pmpcfg0), (uint32_t)RV_READ_CSR(pmpcfg1),
		 (uint32_t)RV_READ_CSR(pmpcfg2), (uint32_t)RV_READ_CSR(pmpcfg3));
	ESP_LOGW(TAG, "pmpaddr0-3 %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
		 "  12-15 %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
		 (uint32_t)RV_READ_CSR(pmpaddr0), (uint32_t)RV_READ_CSR(pmpaddr1),
		 (uint32_t)RV_READ_CSR(pmpaddr2), (uint32_t)RV_READ_CSR(pmpaddr3),
		 (uint32_t)RV_READ_CSR(pmpaddr12), (uint32_t)RV_READ_CSR(pmpaddr13),
		 (uint32_t)RV_READ_CSR(pmpaddr14), (uint32_t)RV_READ_CSR(pmpaddr15));

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
		/* Exceptions Linux handles itself: everything but ecall-from-S. */
		RV_WRITE_CSR(medeleg, 0xb1ff);
		s31_vcpu_crumbs[4] = RV_READ_CSR(medeleg);
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
	ESP_LOGW(TAG, "vectors installed; entering the S-mode guest at %p, crumbs at %p",
		 (void *)s31_vcpu_guest, (void *)s31_vcpu_crumbs);

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
	for (n = 0; n < 4; n++) {
		vTaskDelay(pdMS_TO_TICKS(n < 2 ? 5000 : 30000));
		ESP_LOGW(TAG, "2a: guest timer irqs %" PRIu32 " ipis %" PRIu32 " | injected t %" PRIu32
			 " i %" PRIu32 " deferred %" PRIu32 " | rdtime emulated %" PRIu32 " sbi %" PRIu32
			 " | g2 loops %" PRIu32 ", counters at %p",
			 s31_vcpu_g2_counters[1], s31_vcpu_g2_counters[2], inj_timer, inj_ipi,
			 inj_deferred, emul_rdtime, sbi_calls, s31_vcpu_g2_counters[0],
			 (void *)s31_vcpu_g2_counters);
		ESP_LOGW(TAG, "guest pc %08" PRIx32 " ra %08" PRIx32 " sp %08" PRIx32 " mstatus %08" PRIx32
			 " vpending %" PRIu32 " medeleg %08" PRIx32 " shadowed-csr %" PRIu32
			 " refused-eid %08" PRIx32 " x%" PRIu32,
			 s31_vcpu_area[31], s31_vcpu_area[0], s31_vcpu_area[1], s31_vcpu_area[32],
			 (uint32_t)s31_vcpu_vpending, s31_vcpu_crumbs[4], s31_vcpu_crumbs[7],
			 s31_vcpu_crumbs[5], s31_vcpu_crumbs[6]);
		ESP_LOGW(TAG, "2b-0 reach: psram %08" PRIx32 " xipflash %08" PRIx32 " sclic %08" PRIx32
			 " sclic+64k %08" PRIx32 " uart %08" PRIx32 " fromcpu1 %08" PRIx32
			 " mtime %08" PRIx32 " | psram-w %08" PRIx32 " flash-x %08" PRIx32
			 " done %08" PRIx32,
			 s31_vcpu_probe[0], s31_vcpu_probe[1], s31_vcpu_probe[2], s31_vcpu_probe[3],
			 s31_vcpu_probe[4], s31_vcpu_probe[5], s31_vcpu_probe[6], s31_vcpu_probe[7],
			 s31_vcpu_probe[8], s31_vcpu_probe[9]);
		ESP_LOGW(TAG, "guest loops %" PRIu32 " ecalls %" PRIu32
			 " preempted by %" PRIu32 " M-mode interrupts, other traps %" PRIu32
			 " (last cause %" PRIu32 " epc %08" PRIx32 " tval %08" PRIx32 ")",
			 guest_counters[0], trap_ecalls, s31_vcpu_irq_count, trap_other,
			 last_cause, last_epc, last_tval);
	}
	vTaskDelete(NULL);
}

void s31_vcpu_start(void)
{
	/* Priority 1: above IDLE (whose hook sleeps the hart for a whole tick),
	 * below every IDF system, radio and USB task. */
	xTaskCreate(vcpu_task, "s31_vcpu", 2560, NULL, 1, NULL);
	xTaskCreate(report_task, "s31_vcpu_rep", 2560, NULL, 2, NULL);
}
