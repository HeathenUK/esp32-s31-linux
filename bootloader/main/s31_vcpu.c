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
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "riscv/csr.h"
#include "riscv/rv_utils.h"

static const char *TAG = "s31_vcpu";

extern void _interrupt_handler(void);
extern uint32_t _mtvt_table[48];
extern void s31_vcpu_mtvec(void);
extern void s31_vcpu_irq_entry(void);
extern int s31_vcpu_enter(void);
extern void s31_vcpu_guest(void);

uint32_t s31_vcpu_area[64];
volatile uint32_t s31_vcpu_irq_count;
volatile uint32_t s31_vcpu_crumbs[16];	/* see s31_vcpu_asm.S; read with devmem */
static volatile uint32_t guest_counters[4];	/* [0] loops, written by the guest */
static volatile uint32_t trap_ecalls, trap_other, last_cause, last_epc, last_tval;
static uint32_t my_mtvt[48] __attribute__((aligned(64)));
static uint8_t guest_stack[1024];
static uint32_t leave_after = 0;
static volatile uint32_t snap_mintstatus, snap_mintthresh, snap_mstatus, snap_mcause;

/* Called from s31_vcpu_mtvec with the guest's state saved in `area`.
 * Return 0 to resume the guest, non-zero to make s31_vcpu_enter() return. */
int IRAM_ATTR s31_vcpu_trap(uint32_t *area)
{
	uint32_t cause = RV_READ_CSR(mcause) & 0xfff;	/* CLIC: mask the rest */

	if (cause == 9) {			/* ecall from S-mode */
		trap_ecalls++;
		area[31] += 4;			/* step over the ecall */
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

	memset(s31_vcpu_area, 0, sizeof(s31_vcpu_area));
	s31_vcpu_area[1] = (uint32_t)(uintptr_t)(guest_stack + sizeof(guest_stack)); /* x2 */
	s31_vcpu_area[9] = (uint32_t)(uintptr_t)guest_counters;			     /* a0 */
	s31_vcpu_area[31] = (uint32_t)(uintptr_t)s31_vcpu_guest;		     /* mepc */
	/* mstatus for the guest: MPP = S (01), MPIE = 1, everything else as now. */
	s31_vcpu_area[32] = (RV_READ_CSR(mstatus) & ~0x1888u) | 0x0880u;

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
	for (n = 0; n < 2; n++) {
		vTaskDelay(pdMS_TO_TICKS(n < 2 ? 5000 : 30000));
		ESP_LOGW(TAG, "guest loops %" PRIu32 " ecalls %" PRIu32
			 " preempted by %" PRIu32 " M-mode interrupts, other traps %" PRIu32,
			 guest_counters[0], trap_ecalls, s31_vcpu_irq_count, trap_other);
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
