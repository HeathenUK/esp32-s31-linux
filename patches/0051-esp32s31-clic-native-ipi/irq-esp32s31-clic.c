// SPDX-License-Identifier: GPL-2.0
/*
 * ESP32-S31 Core-Local Interrupt Controller (CLIC) driver
 *
 * The ESP32-S31 CLIC operates in CLIC mode (mtvec.MODE hardwired
 * to 0x3).  For vectored interrupts (clicintattr[i].SHV=1), the
 * hardware reads a 32-bit handler address from (mtvt + 4×interrupt_id)
 * and jumps to it via an indirect jump.
 * We build this table at boot and install it via the mtvt CSR.
 * Non-vectored interrupts (SHV=0) jump to mtvec.BASE.
 *
 * Key ESP32-S31 CLIC characteristics:
 *   - Register base at 0x2080_0000 (DR_REG_CLIC_BASE)
 *   - Per-interrupt control at 0x2080_1000 (DR_REG_CLIC_CTRL_BASE)
 *   - CLICINTCTLBITS = 3 → 8 interrupt levels (3 bits in clicintctl[7:5])
 *   - NMBITS = 0 → M-mode only (no S/U interrupt delivery)
 *   - 32 external interrupts (CLIC IDs 16-47) routed via Interrupt Matrix
 *   - CLINT timer (ID 7) and software (ID 3) interrupts
 *   - NO claim register — vectored delivery
 *   - Per-core address virtualization: each core accesses its OWN
 *     registers at the *same* physical address.  The hardware remaps
 *     based on which core performs the access.  The +0x10000 offset
 *     accesses the OTHER core's registers (for cross-core IPI).
 *   - CLIC_INT_CONFIG register uses legacy layout:
 *       NMBITS  at bits [6:5] (RO, hardwired 0)
 *       MNLBITS at bits [4:1] (R/W, default 0, we set to 3)
 *       NVBITS  at bit  [0]   (RO, hardwired 1)
 *   - Threshold at offset 0x8: byte in bits [31:24]
 *   - CLICINTCTL byte (offset 3 of per-int word):
 *       bits [7:5] = interrupt level (0-7, writable)
 *       bits [4:0] = not used for priority with CLICINTCTLBITS=3
 */

#include <linux/cpu.h>
#include <linux/hrtimer_rearm.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/smp.h>
#include <asm/csr.h>
#include <asm/irq.h>
#include <asm/smp.h>
#include <asm/sbi.h>

int esp32s31_clic_set_priority(unsigned int irq, unsigned int level,
			      unsigned int prio);
void esp32s31_clic_handle_irq(struct pt_regs *regs);
void esp32s31_clic_unexpected(struct pt_regs *regs);

/* ── CLIC register map ──────────────────────────────────────────── */

#ifdef CONFIG_SOC_ESP32S31
#define ESP32S31_CLIC_BASE 0x10a00000	/* S-mode sclicbase window */
#else
#define ESP32S31_CLIC_BASE 0x20800000
#endif
#define ESP32S31_CLIC_DUALCORE_OFF 0x10000
#define ESP32S31_CLIC_SIZE (ESP32S31_CLIC_DUALCORE_OFF * 2)

/* Global registers (offset from CLIC_BASE) */
#define ESP32S31_CLIC_MCLICCFG 0x0000 /* INT_CONFIG (legacy layout) */
#define ESP32S31_CLIC_CLICINFO 0x0004 /* CLICINTCTLBITS, version, count */
#define ESP32S31_CLIC_MINTTHRESH \
	0x0008 /* M-mode threshold (mmio, per-core via addr virt) */

/*
 * Per-interrupt control registers start at 0x2080_1000.
 * Each interrupt gets a 32-bit word with four byte-accessible sub-registers.
 * The same address is used by ALL cores — the hardware virtualises per core.
 * To access the OTHER core's registers, add ESP32S31_CLIC_DUALCORE_OFF.
 */
#ifdef CONFIG_SOC_ESP32S31
#define ESP32S31_CLIC_CTRL_BASE 0x10a01000	/* S-mode per-interrupt window */
#else
#define ESP32S31_CLIC_CTRL_BASE 0x20801000
#endif
#define ESP32S31_CLIC_INT_STRIDE 4 /* 4 bytes per interrupt */
#define ESP32S31_CLIC_INT_IP 0x0 /* Interrupt pending (byte 0, bit 0) */
#define ESP32S31_CLIC_INT_IE 0x1 /* Interrupt enable  (byte 1, bit 0) */
#define ESP32S31_CLIC_INT_ATTR 0x2 /* Interrupt attributes (byte 2) */
#define ESP32S31_CLIC_INT_CTL 0x3 /* Interrupt level (byte 3, bits [7:5]) */

/* ── Interrupt numbering ────────────────────────────────────────── */

#define CLIC_CLINT_SW_ID 3 /* CLINT M-mode software int */
#define CLIC_CLINT_TIMER_ID 7 /* CLINT M-mode timer int    */
#define CLIC_S_TIMER_ID 5 /* S-mode timer interrupt */
#define CLIC_EXT_MIN_ID 16 /* First external IRQ        */
#define CLIC_EXT_MAX_ID 47 /* Last external IRQ         */
#define CLIC_NR_EXTERNAL 32 /* 16-47 inclusive           */
#define CLIC_MAX_ID 47 /* Max interrupt ID          */
/* ── CLIC configuration constants ───────────────────────────────── */

#define ESP32S31_CLICINTCTLBITS 3 /* Hardwired on S31 */
#define ESP32S31_NR_LEVELS (1 << ESP32S31_CLICINTCTLBITS) /* 8 */
#define ESP32S31_EXTERNAL_LEVEL 1
#define ESP32S31_MAX_PRIORITY \
	31 /* Max sub-priority (unused at CLICINTCTLBITS=3) */

/* ── CLIC mcliccfg field encoding (ESP32-S31 uses legacy layout) ─── */
/*
 * CLIC_INT_CONFIG (offset 0x0), standardised across S31:
 *   NMBITS  at bits [6:5]  — RO, hardwired 0 (M-mode only)
 *   MNLBITS at bits [4:1]  — R/W, effective priority bits (we set 3)
 *   NVBITS  at bit  [0]    — RO, hardwired 1 (vectoring supported)
 */
#define CLIC_CFG_NMBITS_MASK 0x60 /* bits [6:5] */
#define CLIC_CFG_MNLBITS_MASK 0x1E /* bits [4:1] */
#define CLIC_CFG_MNLBITS_SHIFT 1

/* ── CLIC CSRs ─────────────────────────────────────────────────── */

#ifdef CONFIG_SOC_ESP32S31
#ifndef CSR_STVT
#define CSR_STVT 0x15c /* Supervisor Trap Vector Table */
#endif
#else
#ifndef CSR_MTVT
#define CSR_MTVT 0x307 /* Machine Trap Vector Table */
#endif
#endif
#ifndef CSR_SINTTHRESH
#define CSR_SINTTHRESH 0x147
#endif

/* ── clicintctl[i] level encoding with CLICINTCTLBITS=3 ──────────── */
/*
 * clicintctl[i] byte (byte 3 of the per-interrupt 32-bit word):
 *   bits [7:5] = interrupt level  (0-7, writable per ESP-IDF)
 *   bits [4:0] = not used for priority discrimination at CLICINTCTLBITS=3
 *
 * Higher level = higher priority.  With CLICINTCTLBITS=3 there are
 * 8 distinct levels and sub-priority bits [4:0] do not affect ordering.
 */
#define CLICCTL_MAKE(level, prio) \
	(((level) << (8 - ESP32S31_CLICINTCTLBITS)) | ((prio) & 0x1F))

/*
 * Build the threshold byte that goes into bits [31:24] of the
 * memory-mapped MINTTHRESH register at offset 0x8.
 * With CLICINTCTLBITS=3: (level << 5) | 0x1F.
 */
#define CLIC_THRESH_BYTE(level)                      \
	(((level) << (8 - ESP32S31_CLICINTCTLBITS)) | \
	 ((1 << (8 - ESP32S31_CLICINTCTLBITS)) - 1))
#define CLIC_THRESH_MMIO(level) ((CLIC_THRESH_BYTE(level)) << 24)

/* ── Interrupt attribute bits (byte 2 of per-interrupt word) ────── */
/*
 *   bit 0:     SHV  — Selective Hardware Vectoring
 *   bits [2:1]: TRIG — Trigger type
 *     X0 = level, 01 = rising edge, 11 = falling edge
 *   bits [7:6]: MODE — Privilege mode (RO, hardwired 0b11 = M-mode on S31)
 */
#define CLIC_ATTR_SHV 0x01 /* bit 0: hardware-vectored */
#define CLIC_ATTR_TRIG_MASK 0x06 /* bits [2:1]: trigger type */
#define CLIC_ATTR_TRIG_LEVEL 0x00 /* level-triggered           */
#define CLIC_ATTR_TRIG_EDGE_RISE 0x02 /* rising-edge triggered     */
#define CLIC_ATTR_TRIG_EDGE_FALL 0x06 /* falling-edge triggered    */
#define CLIC_ATTR_TRIG_EDGE 0x02 /* bit 1: 1 = edge, 0 = level */
#define CLIC_ATTR_MODE_S 0x40 /* S-mode on ESP32-S31 */
#define CLIC_ATTR_MODE_M 0xC0 /* M-mode (RO on S31)         */

#ifdef CONFIG_SOC_ESP32S31
#define ESP32S31_INTMATRIX_BASE 0x20585000
#define ESP32S31_INTMATRIX_CORE_STRIDE 0x800
#define ESP32S31_INTMATRIX_SIZE (ESP32S31_INTMATRIX_CORE_STRIDE * 2)
#define ESP32S31_INTMATRIX_MAP_MASK 0x3f
#define ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT 8
#define ESP32S31_INTMATRIX_PASS_LEVEL_MASK (0x3 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)
#define ESP32S31_INTMATRIX_PASS_LEVEL_S (1 << ESP32S31_INTMATRIX_PASS_LEVEL_SHIFT)
#endif

/* ── Per-CPU CLIC structure ─────────────────────────────────────── */

struct esp32s31_clic {
	void __iomem *regs; /* ioremapped CLIC base (0x2080_0000) */
#ifdef CONFIG_SOC_ESP32S31
	void __iomem *intmatrix_regs;
#endif
	struct irq_domain *domain;
	u32 num_interrupts;
	u32 num_harts;
};

static DEFINE_PER_CPU(struct esp32s31_clic *, clic_per_cpu);
static DEFINE_RAW_SPINLOCK(clic_dev_lock);

/* ── Register access helpers ────────────────────────────────────── */
/*
 * The ESP32-S31 CLIC uses per-core address virtualisation.
 * Every core accesses its OWN per-interrupt registers at the same
 * physical address (ESP32S31_CLIC_CTRL_BASE + irq_id*4 + byte_offset).
 * The +0x10000 dual-core offset is ONLY used to access the OTHER core's
 * registers from a different core — never for the current core's own
 * registers.
 */
/*
 * SMP: EVERY device interrupt lives in hart 1's CLIC, and the window is
 * virtualised per hart - the same address reaches the registers of whichever
 * hart executes the access. So an unmask that happened to run on the CPU lent
 * by hart 0 (an async probe on kworker/1, a threaded handler, enable_irq())
 * set IE in hart 0's CLIC and left hart 1's slot masked: that device's
 * interrupt was dead for the whole boot. 2026-09-20: codec I2C timing out on
 * every transfer in 3 boots of 5, the console dying after one line in 1 of 5,
 * by which CPU ran the probe. From the lent CPU go through the +0x10000 alias,
 * which reaches hart 1 (verified: the IPI pending bit is written that way).
 */
static inline unsigned int clic_dev_off(void)
{
#ifdef CONFIG_SMP
	if (cpuid_to_hartid_map(raw_smp_processor_id()) == 0)
		return ESP32S31_CLIC_DUALCORE_OFF;
#endif
	return 0;
}

static inline bool clic_on_lent_cpu(void)
{
	return clic_dev_off() != 0;
}

static inline u8 clic_readb(struct esp32s31_clic *clic, unsigned int irq_id,
			    unsigned int byte_off)
{
	return readb(clic->regs + clic_dev_off() + ESP32S31_CLIC_CTRL_BASE - ESP32S31_CLIC_BASE +
		     (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

static inline void clic_writeb(struct esp32s31_clic *clic, unsigned int irq_id,
			       unsigned int byte_off, u8 val)
{
	writeb(val, clic->regs + clic_dev_off() + ESP32S31_CLIC_CTRL_BASE - ESP32S31_CLIC_BASE +
			    (irq_id * ESP32S31_CLIC_INT_STRIDE) + byte_off);
}

/*
 * Write the threshold register.  Same address for both cores — the
 * hardware virtualises.  To set the OTHER core's threshold, call
 * clic_write_other_threshold().
 */
static inline void clic_write_threshold(struct esp32s31_clic *clic, u8 level)
{
	writel(CLIC_THRESH_MMIO(level), clic->regs + ESP32S31_CLIC_MINTTHRESH);
}

#ifdef CONFIG_SMP
/*
 * Native IPIs. An SMP kernel IPIs ITSELF constantly (irq_work: deferred
 * printk, RCU, the tick) and our OpenSBI platform registers no IPI device,
 * so with the SBI IPI extension the first self-IPI is lost and the boot
 * parks in WFI right after the console comes up (2026-09-20).
 *
 * FROM_CPU_1 is dedicated to Linux on hart1, as a hardware level source
 * routed to slot 47. FROM_CPU_3 is shared with hosted traffic into hart0;
 * an SRAM request counter lets its ISR distinguish Linux IPIs. FROM_CPU_0
 * remains IDF's yield line and FROM_CPU_2 remains hosted traffic to hart1.
 */
#define ESP32S31_CLIC_IPI_SLOT	47

/*
 * The lent hart0 runs under the FreeRTOS monitor. Its shared hardware
 * doorbell wakes that monitor, which injects Linux cause 47; Linux must not
 * write hart0's IDF-owned CLIC slot directly. Self-IPIs use the same paths.
 */
#define ESP32S31_LENT_HARTID	0
static void __iomem *esp32s31_ipi_doorbell __ro_after_init;
static void __iomem *esp32s31_ipi_req __ro_after_init;
static void __iomem *esp32s31_hart1_ipi_doorbell __ro_after_init;
static DEFINE_RAW_SPINLOCK(esp32s31_ipi_req_lock);

/* DIAGNOSTIC (smp bring-up): who sent what to whom, and who took it. */
static atomic_t s31_ipi_sent[2][2], s31_ipi_taken[2];
static void s31_smp_stall_probe(struct pt_regs *regs);

static void esp32s31_clic_ipi_send(unsigned int cpu)
{

	atomic_inc(&s31_ipi_sent[smp_processor_id() & 1][cpu & 1]);
	if (cpuid_to_hartid_map(cpu) == ESP32S31_LENT_HARTID) {
		/*
		 * The hart1->hart0 doorbell (FROM_CPU_3) is shared with the hosted
		 * transport; hart 0 tells our IPI from its kicks by this request
		 * counter, which only Linux writes. Serialize the two senders:
		 * if both read N and write N+1, the monitor can consume the first
		 * IPI before the second write, see no change and lose the second.
		 * Keep the increment and doorbell ordered inside the IRQ-safe
		 * lock. The monitor never takes this lock or writes the counter.
		 */
		unsigned long flags;

		raw_spin_lock_irqsave(&esp32s31_ipi_req_lock, flags);
		writel(readl(esp32s31_ipi_req) + 1, esp32s31_ipi_req);
		writel(1, esp32s31_ipi_doorbell);
		/* Complete MMIO before another CPU can enter the critical section. */
		readl(esp32s31_ipi_doorbell);
		raw_spin_unlock_irqrestore(&esp32s31_ipi_req_lock, flags);
		return;
	}
	/* Real level doorbell: unlike a software CLIC pending bit, this wakes
	 * hart 1 from WFI. Self-IPIs use exactly the same path. */
	writel(1, esp32s31_hart1_ipi_doorbell);
}

static void esp32s31_clic_ipi_local_init(struct esp32s31_clic *clic)
{
	clic_writeb(clic, ESP32S31_CLIC_IPI_SLOT, ESP32S31_CLIC_INT_IE, 0);
	clic_writeb(clic, ESP32S31_CLIC_IPI_SLOT, ESP32S31_CLIC_INT_ATTR,
		    /* NO SHV: stvt is not installed on S31 (see
		     * esp32s31_clic_install_vectors), so a vectored slot jumps
		     * to 4*id - slot 47 oopsed at epc 0xbc. */
		    CLIC_ATTR_MODE_S | CLIC_ATTR_TRIG_LEVEL);
	clic_writeb(clic, ESP32S31_CLIC_IPI_SLOT, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(ESP32S31_EXTERNAL_LEVEL, ESP32S31_MAX_PRIORITY));
	clic_writeb(clic, ESP32S31_CLIC_IPI_SLOT, ESP32S31_CLIC_INT_IP, 0);
	clic_writeb(clic, ESP32S31_CLIC_IPI_SLOT, ESP32S31_CLIC_INT_IE, 1);
}
#endif

/* ── irq_chip callbacks ─────────────────────────────────────────── */

static void esp32s31_clic_irq_mask(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = &clic_dev_lock;	/* one register set: hart 1's */
	unsigned long flags;

	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, 0);
	raw_spin_unlock_irqrestore(lock, flags);
}

static void esp32s31_clic_irq_unmask(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = &clic_dev_lock;	/* one register set: hart 1's */
	unsigned long flags;

	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_IE, 1);
	raw_spin_unlock_irqrestore(lock, flags);
}

static void esp32s31_clic_irq_ack(struct irq_data *d)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	raw_spinlock_t *lock = &clic_dev_lock;	/* one register set: hart 1's */
	u32 hwirq = d->hwirq;
	unsigned long flags;

	/*
	 * S31 external slots retain clicintip after the matrix source has
	 * deasserted.  Acknowledge both edge and level slots by writing zero.
	 * handle_level_irq masks the slot before this callback, so a source that
	 * is still genuinely asserted is sampled again only after the device
	 * handler has cleared it and the slot is unmasked.
	 */
	raw_spin_lock_irqsave(lock, flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_IP, 0);
	raw_spin_unlock_irqrestore(lock, flags);
}

static int esp32s31_clic_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct esp32s31_clic *clic = irq_data_get_irq_chip_data(d);
	u32 hwirq = d->hwirq;
	unsigned long flags;
	u8 attr;

	/*
	 * ESP32-S31 CLIC trigger encoding (ATTR byte bits [2:1]):
	 *   0bX0 = level-triggered
	 *   0b01 = rising-edge
	 *   0b11 = falling-edge
	 *
	 * The CLIC does not distinguish level-high from level-low.
	 * We map both Linux LEVEL_HIGH and LEVEL_LOW to level mode.
	 */
	switch (flow_type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		attr = CLIC_ATTR_TRIG_LEVEL;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQ_TYPE_EDGE_RISING:
		attr = CLIC_ATTR_TRIG_EDGE_RISE;
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		attr = CLIC_ATTR_TRIG_EDGE_FALL;
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * MODE bits [7:6] are RO (hardwired 0b11 = M-mode on S31) —
	 * writes are ignored but we include them for clarity.
	 * SHV is set for external interrupts (IDs 16-47) only;
	 * CLINT timer/SW (IDs 3, 7) use the non-vectored mtvec.BASE
	 * path to match ESP-IDF behaviour.
	 */
	if (IS_ENABLED(CONFIG_SOC_ESP32S31))
		attr |= CLIC_ATTR_MODE_S;
	else
		attr |= CLIC_ATTR_MODE_M;
#ifndef CONFIG_SOC_ESP32S31
	if (hwirq >= CLIC_EXT_MIN_ID)
		attr |= CLIC_ATTR_SHV;
#endif

	raw_spin_lock_irqsave(&clic_dev_lock, flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_ATTR, attr);
	raw_spin_unlock_irqrestore(&clic_dev_lock, flags);

	return 0;
}

static int esp32s31_clic_set_affinity(struct irq_data *d,
				     const struct cpumask *mask_val, bool force)
{
	/*
	 * CLIC is per-hart with address virtualisation.  Each hart
	 * manages its own interrupt enables/pending via the same
	 * register addresses.  Affinity changes require reprogramming
	 * the Interrupt Matrix to steer the source to a different core's
	 * CLIC input, then configuring the target core's CLIC registers.
	 * Not currently implemented.
	 */
	return -EINVAL;
}

/*
 * Set interrupt priority.  Higher level = higher priority.
 * Level range: 0 (lowest) to 7 (highest).
 * Priority sub-level: 0-31 within same level.
 *
 * Note: with CLICINTCTLBITS=3, only bits [7:5] (the level) affect
 * interrupt prioritisation.  The prio parameter is accepted but has
 * no hardware effect at this configuration (8 discrete levels).
 */
int esp32s31_clic_set_priority(unsigned int irq, unsigned int level,
			      unsigned int prio)
{
	struct irq_data *d = irq_get_irq_data(irq);
	struct esp32s31_clic *clic;
	unsigned long flags;

	if (!d || !d->chip_data)
		return -EINVAL;

	clic = irq_data_get_irq_chip_data(d);

	/*
	 * S31 is intentionally exposed as a non-nested, single-level interrupt
	 * controller.  All S interrupts, including the timer configured by
	 * OpenSBI, use the same effective CLIC level (ctl=0x3f).  Ignore caller
	 * priority requests so no Linux driver can re-enable hardware nesting.
	 */
	if (IS_ENABLED(CONFIG_SOC_ESP32S31)) {
		raw_spin_lock_irqsave(&clic_dev_lock, flags);
		clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(ESP32S31_EXTERNAL_LEVEL,
					 ESP32S31_MAX_PRIORITY));
		raw_spin_unlock_irqrestore(&clic_dev_lock, flags);
		return 0;
	}

	if (level >= ESP32S31_NR_LEVELS || prio > ESP32S31_MAX_PRIORITY)
		return -EINVAL;

	raw_spin_lock_irqsave(&clic_dev_lock, flags);
	clic_writeb(clic, d->hwirq, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(level, prio));
	raw_spin_unlock_irqrestore(&clic_dev_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(esp32s31_clic_set_priority);

static struct irq_chip esp32s31_clic_chip = {
	.name = "ESP32S31-CLIC",
	.irq_mask = esp32s31_clic_irq_mask,
	.irq_unmask = esp32s31_clic_irq_unmask,
	.irq_ack = esp32s31_clic_irq_ack,
	.irq_set_type = esp32s31_clic_set_type,
	.irq_set_affinity = esp32s31_clic_set_affinity,
};

#ifdef CONFIG_SOC_ESP32S31
/* ── S-mode CLIC chip for the riscv,cpu-intc domain ─────────────── */
/*
 * On ESP32-S31 in CLIC mode the standard S-mode interrupt-enable CSRs
 * (sie/sieh) are illegal.  Local riscv,cpu-intc interrupts (timer, IPI)
 * must be masked/unmasked through the S-mode CLIC MMIO window instead.
 * This chip replaces the upstream riscv_intc_chip on the INTC domain.
 */

static void __iomem *esp32s31_sclic_regs __ro_after_init;

#define ESP32S31_SCLIC_CTRL_OFF	0x1000	/* per-interrupt control offset from base */

static void esp32s31_intc_clic_irq_mask(struct irq_data *d)
{
	irq_hw_number_t hwirq = d->hwirq;

	/* The lent CPU's timer and IPI are injected by the hart0 monitor and
	 * gated on sstatus.SIE alone; its CLIC is FreeRTOS's. */
	if (clic_on_lent_cpu())
		return;

	if (WARN_ON_ONCE(!esp32s31_sclic_regs))
		return;

	/*
	 * Linux names the clock event as supervisor timer IRQ5, but S31's
	 * physical compare is delivered directly on CLIC ID7.
	 */
	if (hwirq == CLIC_S_TIMER_ID)
		hwirq = CLIC_CLINT_TIMER_ID;

	writeb(0, esp32s31_sclic_regs + ESP32S31_SCLIC_CTRL_OFF +
		  (hwirq * ESP32S31_CLIC_INT_STRIDE) +
		  ESP32S31_CLIC_INT_IE);
}

static void esp32s31_intc_clic_irq_unmask(struct irq_data *d)
{
	irq_hw_number_t hwirq = d->hwirq;

	/* The lent CPU's timer and IPI are injected by the hart0 monitor and
	 * gated on sstatus.SIE alone; its CLIC is FreeRTOS's. */
	if (clic_on_lent_cpu())
		return;

	if (WARN_ON_ONCE(!esp32s31_sclic_regs))
		return;

	if (hwirq == CLIC_S_TIMER_ID)
		hwirq = CLIC_CLINT_TIMER_ID;

	writeb(1, esp32s31_sclic_regs + ESP32S31_SCLIC_CTRL_OFF +
		  (hwirq * ESP32S31_CLIC_INT_STRIDE) +
		  ESP32S31_CLIC_INT_IE);
}

static void esp32s31_intc_clic_irq_eoi(struct irq_data *d)
{
	/*
	 * Empty EOI — matches riscv_intc_irq_eoi().  Required so that
	 * chained_irq_enter()/chained_irq_exit() in child irqchip drivers
	 * do not trigger unnecessary mask/unmask cycles.
	 */
}

static struct irq_chip esp32s31_intc_chip = {
	.name		= "ESP32-S31 CLIC local INTC",
	.irq_mask	= esp32s31_intc_clic_irq_mask,
	.irq_unmask	= esp32s31_intc_clic_irq_unmask,
	.irq_eoi	= esp32s31_intc_clic_irq_eoi,
};
#endif

/* ── IRQ domain operations ──────────────────────────────────────── */

#ifdef CONFIG_SOC_ESP32S31
static void esp32s31_intmatrix_route(struct esp32s31_clic *clic,
				     unsigned int source,
				     irq_hw_number_t hwirq)
{
	void __iomem *reg;
	u32 val;

	if (!clic->intmatrix_regs)
		return;

	if (source * 4 >= ESP32S31_INTMATRIX_CORE_STRIDE) {
		pr_warn("CLIC: S31 interrupt source %u out of matrix range\n",
			source);
		return;
	}

	/*
	 * Linux exposes physical hart1 as its single logical CPU0.  The S31
	 * interrupt matrix is not address-virtualised, so route every Linux
	 * peripheral through the core1 register bank.
	 */
	reg = clic->intmatrix_regs + ESP32S31_INTMATRIX_CORE_STRIDE + source * 4;
	val = readl(reg);
	val &= ~(ESP32S31_INTMATRIX_MAP_MASK |
		 ESP32S31_INTMATRIX_PASS_LEVEL_MASK);
	val |= (hwirq & ESP32S31_INTMATRIX_MAP_MASK) |
	       ESP32S31_INTMATRIX_PASS_LEVEL_S;
	writel(val, reg);
}
#endif

static int esp32s31_clic_parse_fwspec(struct irq_fwspec *fwspec,
				     irq_hw_number_t *hwirq,
				     unsigned int *source,
				     unsigned int *level,
				     unsigned int *type)
{
	if (fwspec->param_count != 1 && fwspec->param_count != 2 &&
	    fwspec->param_count != 3)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*source = UINT_MAX;
	*level = ESP32S31_EXTERNAL_LEVEL;
	*type = IRQ_TYPE_NONE;

	if (fwspec->param_count == 2)
		*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	else if (fwspec->param_count == 3) {
#ifdef CONFIG_SOC_ESP32S31
		/*
		 * ESP32-S31 uses <raw CLIC ID, IDF interrupt source, type>.
		 * For example UART0 is <32 9 IRQ_TYPE_LEVEL_HIGH>, where
		 * source 9 is ETS_UART0_INTR_SOURCE in the S31 ESP-IDF table.
		 */
		*source = fwspec->param[1];
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
#else
		*level = min_t(unsigned int, fwspec->param[1],
			       ESP32S31_NR_LEVELS - 1);
		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
#endif
	}

	return 0;
}

static int esp32s31_clic_domain_map(struct irq_domain *d, unsigned int irq,
				   irq_hw_number_t hwirq)
{
	struct esp32s31_clic *clic = d->host_data;

	irq_domain_set_info(d, irq, hwirq, &esp32s31_clic_chip, clic,
			    handle_level_irq, NULL, NULL);

	return 0;
}

static int esp32s31_clic_domain_translate(struct irq_domain *domain,
					 struct irq_fwspec *fwspec,
					 irq_hw_number_t *hwirq,
					 unsigned int *type)
{
	unsigned int source, level;

	return esp32s31_clic_parse_fwspec(fwspec, hwirq, &source, &level, type);
}

static int esp32s31_clic_domain_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *data)
{
	struct esp32s31_clic *clic = domain->host_data;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int source, level, type;
	unsigned long flags;

	/*
	 * The current ESP32-S31 DTS uses one-cell interrupt specifiers
	 * (<irq>). ESP32-S31 bring-up DTS uses three-cell specifiers:
	 * (<raw CLIC ID>, <IDF interrupt source>, <IRQ_TYPE_*>).
	 */
	if (esp32s31_clic_parse_fwspec(fwspec, &hwirq, &source, &level, &type))
		return -EINVAL;

	if (hwirq >= clic->num_interrupts)
		return -EINVAL;

#ifdef CONFIG_SOC_ESP32S31
	if (source != UINT_MAX)
		esp32s31_intmatrix_route(clic, source, hwirq);
#endif

	irq_domain_set_info(domain, virq, hwirq, &esp32s31_clic_chip, clic,
			    handle_level_irq, NULL, NULL);

	raw_spin_lock_irqsave(&clic_dev_lock, flags);
	clic_writeb(clic, hwirq, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(level, ESP32S31_MAX_PRIORITY));
	raw_spin_unlock_irqrestore(&clic_dev_lock, flags);

	if (type != IRQ_TYPE_NONE)
		irq_set_irq_type(virq, type);

	return 0;
}

static void esp32s31_clic_domain_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);

	irq_domain_reset_irq_data(data);
}

static const struct irq_domain_ops esp32s31_clic_domain_ops = {
	.map = esp32s31_clic_domain_map,
	.alloc = esp32s31_clic_domain_alloc,
	.free = esp32s31_clic_domain_free,
	.translate = esp32s31_clic_domain_translate,
};

/* ── Vector table ───────────────────────────────────────────────── */

/*
 * ESP32-S31 CLIC vector table — an array of 32-bit handler *addresses*.
 *
 * The S31 CLIC hardware reads the 32-bit word at (mtvt + 4*i), interprets
 * it as an address, and jumps to it.  This is an *indirect* jump through
 * a pointer table — unlike standard RISC-V vectored mode which uses JAL
 * instructions in the table.  Confirmed by ESP-IDF _mtvt_table.
 *
 * mtvt →  ┌─────────────┐
 *         │ slot[ 0]     │  &esp32s31_unexpected_irq
 *         │ slot[ 1]     │  &esp32s31_unexpected_irq
 *         │ slot[ 2]     │  &esp32s31_unexpected_irq
 *         │ slot[ 3]     │  &esp32s31_clic_trampoline  (SW int)
 *         │ slot[ 4..6]  │  &esp32s31_unexpected_irq
 *         │ slot[ 7]     │  &esp32s31_clic_trampoline  (timer)
 *         │ slot[ 8..15] │  &esp32s31_unexpected_irq
 *         │ slot[16..47] │  &esp32s31_clic_trampoline  (ext IRQs)
 *         └─────────────┘
 *
 * 48 entries × 4 bytes = 192 bytes.  Aligned to 64 bytes
 * (matches ESP-IDF _mtvt_table alignment).
 *
 * Each interrupt must also have clicintattr[i].SHV = 1 for the
 * hardware to use this table.  Non-vectored interrupts (SHV=0) jump
 * to mtvec.BASE instead.
 */

/* Assembly entry points — defined in clic-trampoline.S */
extern u8 esp32s31_clic_trampoline;
extern u8 esp32s31_unexpected_irq;

static bool clic_vector_table_built;
static u32 __aligned(64) __ro_after_init clic_vector_table[CLIC_MAX_ID + 1];

/*
 * Build the shared vector table once (hart 0 at boot).
 * Only fills the in-memory table — does NOT write per-hart CSRs.
 */
static void __maybe_unused __init esp32s31_clic_build_vector_table(void)
{
	int i;

	if (clic_vector_table_built)
		return;

	for (i = 0; i <= CLIC_MAX_ID; i++) {
		void *handler;

		if (i == CLIC_CLINT_SW_ID || i == CLIC_CLINT_TIMER_ID ||
		    (i >= CLIC_EXT_MIN_ID && i <= CLIC_EXT_MAX_ID)) {
			handler = &esp32s31_clic_trampoline;
		} else {
			handler = &esp32s31_unexpected_irq;
		}

		/* Store raw address — hardware dereferences it */
		clic_vector_table[i] = (u32)(unsigned long)handler;
	}

	clic_vector_table_built = true;
}

/*
 * Install the CLIC vector table on the *current* hart.
 * Must be called on every hart during init — mtvt and mtvec are
 * per-hart CSRs and the hardware needs them set before any
 * vectored interrupt can be delivered on this hart.
 */
static void __maybe_unused esp32s31_clic_install_vectors(void)
{
#ifdef CONFIG_SOC_ESP32S31
	/*
	 * ESP32-S31 Linux runs in S-mode with the common Linux stvec entry.
	 * The S-mode CLIC vector-table CSR is not part of the validated
	 * hardware contract, so keep interrupts non-vectored here.
	 */
	return;
#else
	/*
	 * mtvt — Machine Trap Vector Table (CLIC CSR 0x307).
	 * In CLIC vectored mode the hardware reads handler addresses
	 * from (mtvt + 4*i) and jumps to them.
	 */
#ifdef CONFIG_SOC_ESP32S31
	csr_write(CSR_STVT, (unsigned long)clic_vector_table);
	csr_write(CSR_STVEC, ((unsigned long)&esp32s31_clic_trampoline) | 0x3);
#else
	csr_write(CSR_MTVT, (unsigned long)clic_vector_table);

	/*
	 * mtvec.BASE — fallback handler for any interrupt with SHV=0.
	 * mtvec.MODE is hardwired to 0x3 on S31, so we only write BASE.
	 */
	csr_write(CSR_MTVEC, (unsigned long)&esp32s31_clic_trampoline);
#endif

	pr_debug("CLIC: vectors installed on current hart (mtvt=0x%p, mtvec=0x%p)\n",
		 clic_vector_table, &esp32s31_clic_trampoline);
#endif
}

/* ── Interrupt handler (vectored path) ──────────────────────────── */

/*
 * This is called from the assembly trampoline with:
 *   a0 = struct pt_regs * (saved context)
 *
 * The trampoline has already filtered synchronous exceptions. Interrupts
 * arrive here with raw mcause in regs->cause.
 */
static void (*fallback_handle_irq)(struct pt_regs *);

void esp32s31_clic_handle_irq(struct pt_regs *regs)
{
	struct esp32s31_clic *clic = this_cpu_read(clic_per_cpu);
	unsigned long logical_cause;
	unsigned long irq_id;

	if (WARN_ON(!clic))
		return;

	logical_cause = regs->cause;

	/*
	 * entry.S has already separated the raw CLIC return token from the
	 * logical Linux cause.  Only the architectural interrupt bit and the
	 * low CLIC ID are visible here.
	 */
	irq_id = regs->cause & 0xfff;

	/*
	 * Interrupt IDs below the first CLIC external source are standard local
	 * RISC-V interrupts routed through the riscv,cpu-intc domain.
	 */
	if (irq_id < CLIC_EXT_MIN_ID) {
		unsigned long linux_irq_id = irq_id;

#ifdef CONFIG_SOC_ESP32S31
		/*
		 * The local compare source is rearmed by programming MTIMECMP.
		 * Clear its software IP latch with 0; unlike external edge slots,
		 * writing 1 here prevents the next compare edge from being seen.
		 */
		/* Injected on the lent CPU: nothing to clear, and through the alias
		 * this would clear HART 1's timer latch. */
		if (!clic_on_lent_cpu()) {
			u8 attr = clic_readb(clic, irq_id, ESP32S31_CLIC_INT_ATTR);

			if (attr & CLIC_ATTR_TRIG_EDGE)
				clic_writeb(clic, irq_id, ESP32S31_CLIC_INT_IP, 0);
		}

		/*
		 * S31's physical compare source is local ID7.  The generic timer
		 * is registered on architectural supervisor timer IRQ5.
		 */
		if (irq_id == CLIC_CLINT_TIMER_ID) {
#ifdef CONFIG_SMP
			s31_smp_stall_probe(regs);
#endif
			linux_irq_id = CLIC_S_TIMER_ID;
		}
#endif
		regs->cause = CAUSE_IRQ_FLAG | linux_irq_id;
#ifdef CONFIG_SOC_ESP32S31
		if (fallback_handle_irq)
			fallback_handle_irq(regs);
#else
		handle_arch_irq(regs);
#endif
		regs->cause = logical_cause;
		goto out;
	}

	if (irq_id >= clic->num_interrupts) {
		pr_warn_ratelimited("CLIC: spurious interrupt %lu\n", irq_id);
		goto out;
	}

#ifdef CONFIG_SMP
	if (irq_id == ESP32S31_CLIC_IPI_SLOT) {
		/* Clear the latch BEFORE processing: a send that lands during
		 * ipi_mux_process() must re-raise it, not be wiped after. */
		/* On the lent CPU the IPI is injected; through the alias this write
		 * would swallow an IPI pending for HART 1. */
		if (!clic_on_lent_cpu()) {
			writel(0, esp32s31_hart1_ipi_doorbell);
			readl(esp32s31_hart1_ipi_doorbell);
			clic_writeb(clic, irq_id, ESP32S31_CLIC_INT_IP, 0);
		}
		atomic_inc(&s31_ipi_taken[smp_processor_id() & 1]);
		ipi_mux_process();
		goto out;
	}
#endif

	generic_handle_domain_irq(clic->domain, irq_id);

	/*
	 * No claim/completion is needed.  The architecture return path restores
	 * cause_raw before sstatus; Linux never interprets or mutates that CLIC
	 * hardware state.
	 */

out:
#ifdef CONFIG_SMP
	/*
	 * The lent-hart monitor injects a wake interrupt at trapped WFI even
	 * though the idle loop has SIE clear. Generic irqentry_exit correctly
	 * skips deferred hrtimer rearming on an IRQ-disabled return, but here
	 * the idle loop will enable IRQs without another irqentry exit. Flush
	 * the deferred rearm while still in IRQ context with RCU watching;
	 * otherwise the comparator stays stopped and queued timers (including
	 * DRM vblank and nanosleep) never fire until some unrelated wakeup.
	 */
	if (clic_on_lent_cpu() && regs_irqs_disabled(regs))
		hrtimer_rearm_deferred();
#endif
	/*
	 * RISC-V enters this handler through do_irq()/handle_riscv_irq(),
	 * which already performs irqentry state management, irq_enter_rcu(),
	 * irq_exit_rcu(), and set_irq_regs() around handle_arch_irq().
	 * Repeating that work here corrupts nested IRQ accounting and can
	 * trip scheduler invariants during timer-driven reschedule.
	 */
}

/*
 * "Unexpected" interrupt handler for unused vector slots.
 * Should never be reached — spins with interrupts disabled.
 */
void esp32s31_clic_unexpected(struct pt_regs *regs)
{
	pr_emerg("CLIC: unexpected interrupt — cause=0x%lx, epc=0x%lx\n",
		 regs->cause, regs->epc);
	while (1)
		cpu_relax();
}

/* ── Initialization ─────────────────────────────────────────────── */

static void __init esp32s31_clic_init_hart(struct esp32s31_clic *clic, int hart)
{
	int i;
#ifdef CONFIG_SOC_ESP32S31
	const u8 mode_attr = CLIC_ATTR_MODE_S;

	csr_write(CSR_SINTTHRESH, 0);

	/*
	 * ID7 is configured by OpenSBI as a direct S-mode timer at the same
	 * fixed level as every external interrupt.  Do not overwrite it here.
	 */

	for (i = CLIC_EXT_MIN_ID; i <= CLIC_EXT_MAX_ID; i++) {
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_ATTR,
			    mode_attr | CLIC_ATTR_TRIG_LEVEL);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(ESP32S31_EXTERNAL_LEVEL,
					 ESP32S31_MAX_PRIORITY));
	}

	per_cpu(clic_per_cpu, hart) = clic;
#ifdef CONFIG_SMP
	{
		/*
		 * One CLIC instance serves every CPU (the S window virtualises
		 * per hart), and a CPU lent by hart0 never runs this init - its
		 * interrupts are injected by the hart0 monitor. Without this
		 * its first IPI tripped WARN_ON(!clic) (2026-09-20).
		 */
		int cpu;

		for_each_possible_cpu(cpu)
			per_cpu(clic_per_cpu, cpu) = clic;
	}
#endif

	pr_debug("CLIC: S31 hart %d using S-mode sclicbase window\n", hart);
	return;
#else
	u32 info, clicintctlbits;

	/* ── Read CLIC capabilities ────────────────────────────────── */
	/*
	 * CLICINFO register (offset 0x4):
	 *   bits [24:21] = CLICINTCTLBITS (RO, default 3 on S31)
	 *   bits [20:13] = version (implementation-defined)
	 *   bits [12:0]  = NUM_INT (RO, default 48 on S31)
	 */
	info = readl(clic->regs + ESP32S31_CLIC_CLICINFO);
	clicintctlbits = (info >> 21) & 0xF;

	pr_debug("CLIC: hart %d, CLICINTCTLBITS=%u (%u levels), NUM_INT=%u\n",
		 hart, clicintctlbits, 1u << clicintctlbits, info & 0x1FFF);

	/* ── Configure mcliccfg ────────────────────────────────────── */
	/*
	 * ESP32-S31 uses the legacy CLIC_INT_CONFIG layout.
	 * Set MNLBITS=3 (8 effective priority bits in clicintctl),
	 * leave NMBITS=0 (M-mode only, RO) and NVBITS=1 (RO, vectored).
	 */
	{
		u32 cfg = readl(clic->regs + ESP32S31_CLIC_MCLICCFG);
		cfg &= ~CLIC_CFG_MNLBITS_MASK;
		cfg |= (3 << CLIC_CFG_MNLBITS_SHIFT);
		writel(cfg, clic->regs + ESP32S31_CLIC_MCLICCFG);
	}

	/* ── Set interrupt threshold ───────────────────────────────── */
	/*
	 * Threshold 0 (byte 0x1F in bits [31:24]) allows interrupt levels
	 * above 0 through.  Level-0 interrupts compare equal to the threshold
	 * and remain masked, so regular external interrupts default to level 1.
	 * The CLIC hardware handles nesting — higher-level interrupts preempt
	 * lower-level ones automatically via mintstatus.
	 *
	 * IMPORTANT: the threshold register at 0x8 is per-core via address
	 * virtualisation.  Do NOT add a hart offset — each core accesses
	 * its own threshold at the same physical address (0x2080_0008).
	 */
	clic_write_threshold(clic, 0);

	/* ── Initialise all interrupt slots ────────────────────────── */
	/*
	 * Initialise all interrupts: disabled, M-mode, level-triggered.
	 * External IDs (16-47) default to level 1 so they are not masked by the
	 * level-0 threshold. They also get SHV=1 for hardware vectoring via the
	 * mtvt table — this matches ESP-IDF behaviour.
	 *
	 * CLINT timer (ID 7) and software (ID 3) interrupts are
	 * NOT vectored on ESP32-S31; they use the mtvec.BASE path.
	 * MODE bits [7:6] are RO (hardwired 0b11 = M-mode).
	 */
	for (i = 0; i <= CLIC_MAX_ID; i++) {
		u8 attr = CLIC_ATTR_MODE_M | CLIC_ATTR_TRIG_LEVEL;
		u8 level = 0;

#ifdef CONFIG_SOC_ESP32S31
		/* Preserve OpenSBI-configured timer/software interrupts */
		if (i == CLIC_CLINT_TIMER_ID || i == CLIC_CLINT_SW_ID)
			continue;
#endif

		if (i >= CLIC_EXT_MIN_ID) {
			attr |= CLIC_ATTR_SHV;
			level = ESP32S31_EXTERNAL_LEVEL;
		}

		clic_writeb(clic, i, ESP32S31_CLIC_INT_IP, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_IE, 0);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_ATTR, attr);
		clic_writeb(clic, i, ESP32S31_CLIC_INT_CTL,
			    CLICCTL_MAKE(level, ESP32S31_MAX_PRIORITY));
	}

	/*
	 * CLINT timer interrupt (ID 7): highest level so scheduler
	 * tick always preempts other interrupts.  Non-vectored.
	 *
	 * CLINT software interrupt (ID 3): level 1 for IPI delivery so it is
	 * above the level-0 threshold. IPIs are still low-priority relative to
	 * the scheduler tick and regular high-priority interrupts. Non-vectored.
	 *
	 * On S31 these are managed by OpenSBI — skip to avoid overwriting
	 * IE/ATTR configured before entering S-mode.
	 */
#ifndef CONFIG_SOC_ESP32S31
	clic_writeb(clic, CLIC_CLINT_TIMER_ID, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(7, ESP32S31_MAX_PRIORITY));
	clic_writeb(clic, CLIC_CLINT_SW_ID, ESP32S31_CLIC_INT_CTL,
		    CLICCTL_MAKE(1, ESP32S31_MAX_PRIORITY));
#endif

	/* Store per-CPU reference for the handler */
	per_cpu(clic_per_cpu, hart) = clic;

	/*
	 * Build the shared vector table once (hart 0 at boot).
	 * Then install the vectors on EVERY hart — mtvt and mtvec
	 * are per-hart CSRs and each core must have its own copy.
	 */
	esp32s31_clic_build_vector_table();
	esp32s31_clic_install_vectors();
#endif
}

static int __init esp32s31_clic_probe(struct device_node *node,
				     struct device_node *parent)
{
	struct esp32s31_clic *clic;
	struct resource res;
	u32 num_interrupts = CLIC_MAX_ID + 1;
	u32 num_harts = 2; /* ESP32-S31 has 2 HP cores */
	int ret;

	clic = kzalloc(sizeof(*clic), GFP_KERNEL);
	if (!clic)
		return -ENOMEM;

	/*
	 * Map CLIC registers.  The CLIC occupies 0x2080_0000–0x2081_FFFF
	 * (two 64 KB windows for dual-core).  Falls back to the known
	 * base if no DT reg property.
	 *
	 * We map the full region even though the hardware virtualises
	 * per-core access — this covers the threshold register and
	 * allows future cross-core register access at +0x10000.
	 */
	ret = of_address_to_resource(node, 0, &res);
	if (ret || IS_ENABLED(CONFIG_SOC_ESP32S31)) {
		res.start = ESP32S31_CLIC_BASE;
		res.end = ESP32S31_CLIC_BASE + ESP32S31_CLIC_SIZE - 1;
	}

	clic->regs = ioremap(res.start, resource_size(&res));
	if (!clic->regs) {
		pr_err("CLIC: Failed to ioremap 0x%llx\n",
		       (unsigned long long)res.start);
		ret = -ENOMEM;
		goto err_free;
	}

#ifdef CONFIG_SOC_ESP32S31
	clic->intmatrix_regs = ioremap(ESP32S31_INTMATRIX_BASE,
				       ESP32S31_INTMATRIX_SIZE);
	if (!clic->intmatrix_regs) {
		pr_err("CLIC: Failed to ioremap S31 interrupt matrix\n");
		ret = -ENOMEM;
		goto err_unmap;
	}
#endif

	/*
	 * The ESP32-S31 CLIC is hardwired to 48 interrupts (IDs 0-47)
	 * with 2 HP cores.  DT properties can override for variants.
	 */

	clic->num_interrupts = 48;
	clic->num_harts = 2;

	/* Create IRQ domain */
	clic->domain = irq_domain_add_linear(node, num_interrupts,
					     &esp32s31_clic_domain_ops, clic);
	if (!clic->domain) {
		pr_err("CLIC: Failed to create IRQ domain\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* Initialize hart 0 (boot CPU) */
	esp32s31_clic_init_hart(clic, 0);

#ifdef CONFIG_SOC_ESP32S31
	/*
	 * Take over as the top-level IRQ handler.  The riscv-intc has
	 * already registered riscv_intc_irq() via set_handle_irq().
	 * We capture it as fallback for local interrupts (IDs < 16)
	 * and install our CLIC handler which dispatches both local
	 * and external (CLIC IDs 16-47) interrupts.
	 */
	fallback_handle_irq = handle_arch_irq;
	handle_arch_irq = esp32s31_clic_handle_irq;

	/*
	 * Map the S-mode CLIC window for local interrupt masking and
	 * swap the riscv,cpu-intc domain's irq_chip so that mask/unmask
	 * of local interrupts (timer, IPI) goes through the CLIC MMIO
	 * registers instead of the illegal sie/sieh CSRs.
	 */
	esp32s31_sclic_regs = ioremap(ESP32S31_CLIC_BASE, SZ_128K);
	if (!esp32s31_sclic_regs) {
		pr_err("CLIC: failed to ioremap S-mode CLIC window\n");
		ret = -ENOMEM;
		goto err_restore_handler;
	}
	{
		struct fwnode_handle *intc_fwnode = riscv_get_intc_hwnode();
		struct irq_domain *intc_domain;

		if (intc_fwnode) {
			intc_domain = irq_find_matching_fwnode(intc_fwnode,
							      DOMAIN_BUS_ANY);
			if (intc_domain)
				intc_domain->host_data = &esp32s31_intc_chip;
			else
				pr_warn("CLIC: could not find INTC domain to swap chip\n");
		} else {
			pr_warn("CLIC: no INTC hwnode — local IRQ masking may fail\n");
		}
	}
#endif

#ifdef CONFIG_SMP
	{
		int virq = ipi_mux_create(BITS_PER_BYTE, esp32s31_clic_ipi_send);

		if (virq <= 0) {
			pr_err("CLIC: ipi_mux_create failed (%d)\n", virq);
		} else {
			esp32s31_ipi_doorbell = ioremap(0x2058601c, 4);	/* FROM_CPU_3 */
			esp32s31_ipi_req = ioremap(0x2f06afbc, 4);	/* s31_hosted_control.lent_cpu_ipi_req */
			esp32s31_hart1_ipi_doorbell = ioremap(0x20586014, 4);
			if (!esp32s31_ipi_doorbell || !esp32s31_ipi_req ||
			    !esp32s31_hart1_ipi_doorbell)
				panic("CLIC: cannot map SMP doorbells");
			writel(0, esp32s31_hart1_ipi_doorbell);
			esp32s31_intmatrix_route(clic, 66, ESP32S31_CLIC_IPI_SLOT);
			esp32s31_clic_ipi_local_init(clic);
			/* Before sbi_ipi_init(), which then stands down. */
			riscv_ipi_set_virq_range(virq, BITS_PER_BYTE);
			pr_info("CLIC: hardware FROM_CPU_1 IPIs on slot %d (level)\n",
				ESP32S31_CLIC_IPI_SLOT);
			/* Idle policy is per hart: arch_cpu_idle() in process.c. */
		}
	}
#endif

	pr_info("CLIC: initialized — %u interrupts, %u harts, vectored mode\n",
		num_interrupts, num_harts);

	return 0;

err_restore_handler:
#ifdef CONFIG_SOC_ESP32S31
	handle_arch_irq = fallback_handle_irq;
	fallback_handle_irq = NULL;
#endif
err_unmap:
#ifdef CONFIG_SOC_ESP32S31
	if (clic->intmatrix_regs)
		iounmap(clic->intmatrix_regs);
#endif
	iounmap(clic->regs);
err_free:
	kfree(clic);
	return ret;
}

#ifdef CONFIG_SMP
#include <linux/timer.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/kernel_stat.h>
#include <linux/sched/stat.h>
#include <linux/sched/debug.h>
#include <linux/hrtimer.h>
static bool s31_smp_diag;
core_param(s31_smp_diag, s31_smp_diag, bool, 0400);
static unsigned long s31_timer_probe;
core_param(s31_timer_probe, s31_timer_probe, ulong, 0400);
static void __iomem *s31_timer_trace;
static unsigned int s31_smp_dump_after;
core_param(s31_smp_dump_after, s31_smp_dump_after, uint, 0400);
static unsigned long s31_smp_dump_at;
static bool s31_smp_dump_done;
static struct timer_list s31_smp_beat;
static void __iomem *s31_busmon, *s31_h0trace;

/* Diagnostic only: softirq lockups can stop the normal heartbeat before RCU
 * can print a stack. Sample once per CPU from any still-running timer IRQ, before
 * the generic timer handler enters RCU stall reporting and its node locks. */
static unsigned long s31_smp_last_beat;
static bool s31_smp_beat_started;
static DEFINE_PER_CPU(bool, s31_smp_stall_reported);

static void s31_smp_stall_probe(struct pt_regs *regs)
{
	if (!s31_smp_diag ||
	    !READ_ONCE(s31_smp_beat_started) || this_cpu_read(s31_smp_stall_reported) ||
	    !time_after(jiffies, READ_ONCE(s31_smp_last_beat) + 6 * HZ))
		return;
	this_cpu_write(s31_smp_stall_reported, true);
	if (s31_busmon) {
		writel(3, s31_busmon + 0x44);
		writel(3, s31_busmon + 0xcc);
	}
	pr_emerg("s31-stall: cpu%u pc %08lx ra %08lx sp %08lx a0 %08lx status %08lx pid %d comm %s | hart0 pc %08x sp %08x guest pc %08x ra %08x a0 %08x | hart1 pc %08x sp %08x\n",
		smp_processor_id(), regs->epc, regs->ra, regs->sp, regs->a0, regs->status,
		current->pid, current->comm,
		s31_busmon ? readl(s31_busmon + 0x48) : 0,
		s31_busmon ? readl(s31_busmon + 0x4c) : 0,
		s31_h0trace ? readl(s31_h0trace + 20) : 0,
		s31_h0trace ? readl(s31_h0trace + 16) : 0,
		s31_h0trace ? readl(s31_h0trace + 12) : 0,
		s31_busmon ? readl(s31_busmon + 0xd0) : 0,
		s31_busmon ? readl(s31_busmon + 0xd4) : 0);
}

static void s31_smp_beat_fn(struct timer_list *t)
{
	WRITE_ONCE(s31_smp_last_beat, jiffies);
	WRITE_ONCE(s31_smp_beat_started, true);
	if (s31_smp_dump_after && !s31_smp_dump_done &&
	    time_after_eq(jiffies, s31_smp_dump_at)) {
		s31_smp_dump_done = true;
		pr_info("s31-smp: one-shot task snapshot (diagnostic boot)\n");
		show_state_filter(0);
		sysrq_timer_list_show();
	}
	if (s31_timer_trace)
		pr_info("s31-timer: now %08x:%08x cmp %08x:%08x deadline %08x:%08x clic %08x pending %x ctl %x\n",
			readl(s31_timer_trace + 4), readl(s31_timer_trace),
			readl(s31_timer_trace + 12), readl(s31_timer_trace + 8),
			readl(s31_timer_trace + 20), readl(s31_timer_trace + 16),
			readl(s31_timer_trace + 24), readl(s31_timer_trace + 28),
			readl(s31_timer_trace + 40));
	/* (A 'hart0 frozen' panic lived here. It is WRONG now that an idle CPU1
	 * lets hart0 sleep in FreeRTOS's idle loop: a constant PC is the goal.) */
	if (s31_busmon) {
		writel(3, s31_busmon + 0x44);	/* hart0 record enable */
		pr_info("s31-smp: hart0 pc %08x sp %08x\n",
			readl(s31_busmon + 0x48), readl(s31_busmon + 0x4c));
	}
	if (s31_h0trace)
		pr_info("s31-smp: hart0 monitor: world step %02x guest a0 %08x ra %08x pc %08x cpst %04x inj t %u i %u deferred %u | enable readback %08x | last illegal: EXT_ILL<<24|FS|cpst %08x\n",
			readl(s31_h0trace), readl(s31_h0trace + 12), readl(s31_h0trace + 16),
			readl(s31_h0trace + 20), readl(s31_h0trace + 24), readl(s31_h0trace + 28),
			readl(s31_h0trace + 32), readl(s31_h0trace + 36), readl(s31_h0trace + 40),
			readl(s31_h0trace + 44));
	pr_info("s31-smp: ipi sent 0>0 %d 0>1 %d 1>0 %d 1>1 %d | taken cpu0 %d cpu1 %d | irqs cpu0 %llu cpu1 %llu | running %u\n",
		atomic_read(&s31_ipi_sent[0][0]), atomic_read(&s31_ipi_sent[0][1]),
		atomic_read(&s31_ipi_sent[1][0]), atomic_read(&s31_ipi_sent[1][1]),
		atomic_read(&s31_ipi_taken[0]), atomic_read(&s31_ipi_taken[1]),
		(unsigned long long)kstat_cpu_irqs_sum(0),
		(unsigned long long)(num_online_cpus() > 1 ? kstat_cpu_irqs_sum(1) : 0),
		nr_running());
	mod_timer(&s31_smp_beat, jiffies + 2 * HZ);
}

static int __init s31_smp_beat_init(void)
{
	if (!s31_smp_diag && !s31_timer_probe && !s31_smp_dump_after)
		return 0;
	if (s31_timer_probe)
		s31_timer_trace = ioremap(s31_timer_probe, 48);
	s31_smp_dump_at = jiffies + s31_smp_dump_after * HZ;
	s31_busmon = ioremap(0x2d002000, 0x100);
	s31_h0trace = ioremap(0x50fef000, 0x1000);
	if (s31_h0trace)
		s31_h0trace += 0xfd0;
	timer_setup(&s31_smp_beat, s31_smp_beat_fn, TIMER_PINNED);
	s31_smp_beat.expires = jiffies + 2 * HZ;	/* NOT 0: jiffies starts at -5 min */
	add_timer_on(&s31_smp_beat, 0);
	return 0;
}
early_initcall(s31_smp_beat_init);

/*
 * MEASURED 2026-09-21 by a one-shot self-test that lived here (6 boots of 6):
 * writes made on the lent CPU through the +0x10000 alias reach hart 1's CLIC
 * for ALL FOUR bytes of a slot - IE (1 -> 1, 0 -> 0), ATTR (42 -> 42, 40 ->
 * 40), CTL (1f -> 1f, 3f -> 3f) - and IP can be set through it. The alias is
 * sound; do not suspect it again. The test also showed initcalls running on
 * CPU1 on some boots despite isolcpus=1 (CONFIG_CPU_ISOLATION was disabled).
 * An LR/SC-versus-plain-store test was attempted next and removed: its inline
 * asm was wrong (a0 clobbered before lr.w) and it oopsed; the hardware
 * question it asked is still OPEN.
 */
#endif

/*
 * MEASURED 2026-09-21 (step 0 of docs/smp-ipi-plan.md, one-shot test since
 * removed): a real CPU_INT_FROM_CPU source line DOES wake hart 1 from wfi.
 * Source 66 (FROM_CPU_1) routed to a free slot, edge-latched; hart 1 asleep in
 * wfi with interrupts off, the lent CPU rings 2 ms later. 36 of 40 trials had
 * hart 1 truly asleep (one wfi, zero fall-throughs): ring -> awake latency
 * min 0, median 1, max 9 us. (The other 4 had a tick pending first - with
 * interrupts off that turns wfi into a no-op, so they were excluded.) The
 * former software-set IPI pending bit does NOT wake wfi.
 */

IRQCHIP_DECLARE(esp32s31_clic, "espressif,esp32s31-clic", esp32s31_clic_probe);
