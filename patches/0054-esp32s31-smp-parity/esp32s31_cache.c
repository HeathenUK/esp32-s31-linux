// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31 external-memory cache maintenance
 *
 * The register programming follows ESP-IDF's ESP32-S31 cache LL and ROM
 * writeback workaround. In particular, writeback-class operations must be
 * issued twice on this SoC to avoid losing a synchronization request.
 */

#include <linux/cpumask.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>

#include <linux/soc/espressif/esp32s31-cache.h>

#define ESP32S31_CACHE_SYNC_CTRL		0x09c
#define ESP32S31_CACHE_SYNC_MAP		0x0a0
#define ESP32S31_CACHE_SYNC_ADDR		0x0a4
#define ESP32S31_CACHE_SYNC_SIZE		0x0a8

#define ESP32S31_CACHE_INVALIDATE	BIT(0)
#define ESP32S31_CACHE_WRITEBACK		BIT(2)
#define ESP32S31_CACHE_WB_INVALIDATE	BIT(3)
#define ESP32S31_CACHE_SYNC_DONE		BIT(4)

#define ESP32S31_CACHE_MAP_ICACHE0	BIT(0)
#define ESP32S31_CACHE_MAP_ICACHE1	BIT(1)
#define ESP32S31_CACHE_MAP_DCACHE	BIT(4)

/*
 * Instruction-cache prelock and preload.
 *
 * The L1 caches are fixed in silicon - every CACHESIZE field in the register
 * map is hardware read-only, so the 16 KiB I-cache cannot be enlarged. What
 * *is* writable is which lines stay in it: two prelock sections per I-cache
 * pin an address range so it is never evicted, and preload fills it without
 * waiting for the code to be executed first.
 *
 * That matters here because this kernel executes XIP from flash and the CPU is
 * memory-bound even at idle (~23% of its theoretical rate with nothing else
 * running). Code that stays resident never pays a fetch again.
 *
 * Register offsets are for I-cache 1, the one Linux's hart uses; I-cache 0
 * belongs to hart0 and is left alone.
 */
#define ESP32S31_ICACHE1_PRELOCK_CONF		0x04c
#define ESP32S31_ICACHE1_PRELOCK_SCT0_ADDR	0x050
#define ESP32S31_ICACHE1_PRELOCK_SCT1_ADDR	0x054
#define ESP32S31_ICACHE1_PRELOCK_SCT_SIZE	0x058
#define ESP32S31_ICACHE1_PRELOAD_CTRL		0x0b8
#define ESP32S31_ICACHE1_PRELOAD_ADDR		0x0bc
#define ESP32S31_ICACHE1_PRELOAD_SIZE		0x0c0

#define ESP32S31_PRELOCK_SCT0_EN		BIT(0)
#define ESP32S31_PRELOCK_SCT1_EN		BIT(1)
#define ESP32S31_PRELOCK_SCT0_SIZE_S		0
#define ESP32S31_PRELOCK_SCT1_SIZE_S		16
#define ESP32S31_PRELOCK_SIZE_MAX		GENMASK(13, 0)
#define ESP32S31_PRELOAD_ENA			BIT(0)
#define ESP32S31_PRELOAD_SIZE_MAX		GENMASK(13, 0)

#define ESP32S31_CACHE_LINE_SIZE		64U
#define ESP32S31_CACHE_SYNC_SIZE_MAX	GENMASK(27, 0)

static void __iomem *esp32s31_cache_base;
static u32 esp32s31_icache_map = ESP32S31_CACHE_MAP_ICACHE0;
static DEFINE_RAW_SPINLOCK(esp32s31_cache_lock);

static void esp32s31_cache_wait_done(void)
{
	while (!(readl_relaxed(esp32s31_cache_base +
				ESP32S31_CACHE_SYNC_CTRL) &
		 ESP32S31_CACHE_SYNC_DONE))
		cpu_relax();
}

static void esp32s31_cache_issue(u32 map, u32 addr, u32 size, u32 op)
{
	writel_relaxed(map, esp32s31_cache_base + ESP32S31_CACHE_SYNC_MAP);
	writel_relaxed(addr, esp32s31_cache_base + ESP32S31_CACHE_SYNC_ADDR);
	writel_relaxed(size, esp32s31_cache_base + ESP32S31_CACHE_SYNC_SIZE);
	wmb();

	writel_relaxed(op, esp32s31_cache_base + ESP32S31_CACHE_SYNC_CTRL);
	esp32s31_cache_wait_done();

	/* ESP-IDF's S31 workaround repeats writeback-class sync requests. */
	if (op & (ESP32S31_CACHE_WRITEBACK |
		  ESP32S31_CACHE_WB_INVALIDATE)) {
		writel_relaxed(op, esp32s31_cache_base +
			       ESP32S31_CACHE_SYNC_CTRL);
		esp32s31_cache_wait_done();
	}

	mb();
}

static bool esp32s31_cache_align_range(phys_addr_t paddr, size_t size,
				       u32 *addr, u32 *len)
{
	u64 start, end;

	if (!size || paddr > U32_MAX)
		return false;

	start = (u64)paddr & ~(ESP32S31_CACHE_LINE_SIZE - 1);
	end = ALIGN((u64)paddr + size, ESP32S31_CACHE_LINE_SIZE);
	if (end <= start || end > (u64)U32_MAX + 1 ||
	    end - start > ESP32S31_CACHE_SYNC_SIZE_MAX)
		return false;

	*addr = start;
	*len = end - start;
	return true;
}

/*
 * Cache maintenance is the dominant CPU cost of DMA on this SoC, so account for
 * it: whether it is dominated by per-operation overhead (spinlock, register
 * writes, the wait) or by bytes decides whether the fix is fewer, larger
 * operations or avoiding the cache entirely.
 *
 *     /sys/kernel/debug/esp32s31_cache/stats
 */
static atomic64_t cache_stat_ops[2];		/* [0] inv-class, [1] wb-class */
static atomic64_t cache_stat_bytes[2];
static atomic64_t cache_stat_ns[2];

static void esp32s31_cache_range(phys_addr_t paddr, size_t size, u32 op)
{
	unsigned long flags;
	u32 addr, len;

	if (unlikely(!esp32s31_cache_base) ||
	    !esp32s31_cache_align_range(paddr, size, &addr, &len))
		return;

	unsigned int slot = (op & (ESP32S31_CACHE_WRITEBACK |
				   ESP32S31_CACHE_WB_INVALIDATE)) ? 1 : 0;
	u64 t0 = ktime_get_ns();

	raw_spin_lock_irqsave(&esp32s31_cache_lock, flags);
	esp32s31_cache_issue(ESP32S31_CACHE_MAP_DCACHE, addr, len, op);
	raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);

	atomic64_inc(&cache_stat_ops[slot]);
	atomic64_add(len, &cache_stat_bytes[slot]);
	atomic64_add(ktime_get_ns() - t0, &cache_stat_ns[slot]);
}

static void esp32s31_cache_dma_writeback(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_range(paddr, size, ESP32S31_CACHE_WRITEBACK);
}

void esp32s31_cache_writeback(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_range(paddr, size, ESP32S31_CACHE_WRITEBACK);
}
EXPORT_SYMBOL_GPL(esp32s31_cache_writeback);

void esp32s31_cache_invalidate(phys_addr_t paddr, size_t size)
{
	unsigned long flags;
	u32 addr, len;

	if (unlikely(!esp32s31_cache_base) ||
	    !esp32s31_cache_align_range(paddr, size, &addr, &len))
		return;

	/* External Flash aliases are served by I-cache; PSRAM data uses D-cache. */
	raw_spin_lock_irqsave(&esp32s31_cache_lock, flags);
	esp32s31_cache_issue(ESP32S31_CACHE_MAP_DCACHE | esp32s31_icache_map,
			     addr, len, ESP32S31_CACHE_INVALIDATE);
	raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);
}
EXPORT_SYMBOL_GPL(esp32s31_cache_invalidate);

static void esp32s31_cache_dma_invalidate(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_range(paddr, size, ESP32S31_CACHE_INVALIDATE);
}

static void esp32s31_cache_dma_wback_invalidate(phys_addr_t paddr, size_t size)
{
	esp32s31_cache_range(paddr, size, ESP32S31_CACHE_WB_INVALIDATE);
}

void esp32s31_cache_sync_for_exec(phys_addr_t paddr, size_t size)
{
	unsigned long flags;
	u32 addr, len;

	if (unlikely(!esp32s31_cache_base) ||
	    !esp32s31_cache_align_range(paddr, size, &addr, &len))
		return;

	raw_spin_lock_irqsave(&esp32s31_cache_lock, flags);
	esp32s31_cache_issue(ESP32S31_CACHE_MAP_DCACHE, addr, len,
			     ESP32S31_CACHE_WRITEBACK);
	esp32s31_cache_issue(esp32s31_icache_map, addr, len,
			     ESP32S31_CACHE_INVALIDATE);
	raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);
}

static const struct riscv_nonstd_cache_ops esp32s31_cache_ops __initconst = {
	.wback = esp32s31_cache_dma_writeback,
	.inv = esp32s31_cache_dma_invalidate,
	.wback_inv = esp32s31_cache_dma_wback_invalidate,
};

static int esp32s31_cache_stats_show(struct seq_file *m, void *v)
{
	static const char * const names[] = { "invalidate", "writeback" };
	unsigned int i;

	for (i = 0; i < 2; i++) {
		u64 ops = atomic64_read(&cache_stat_ops[i]);
		u64 bytes = atomic64_read(&cache_stat_bytes[i]);
		u64 ns = atomic64_read(&cache_stat_ns[i]);

		seq_printf(m, "%s ops=%llu bytes=%llu ns=%llu ns_per_op=%llu ns_per_kb=%llu\n",
			   names[i], ops, bytes, ns,
			   ops ? div64_u64(ns, ops) : 0,
			   bytes ? div64_u64(ns * 1024, bytes) : 0);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(esp32s31_cache_stats);

/*
 * Pin [addr, addr+size) into I-cache section @sct, then preload it so the
 * lines are actually present rather than merely reserved. Passing size 0
 * releases the section.
 *
 * The hardware size field is 14 bits, so a section covers at most 16383 bytes,
 * which is the whole cache. Two sections exist; using both to cover more than
 * the cache holds would simply thrash.
 */
static int esp32s31_icache_prelock(unsigned int sct, u32 addr, u32 size)
{
	unsigned long flags;
	u32 conf, sizes, en;

	if (sct > 1)
		return -EINVAL;
	if (size > ESP32S31_PRELOCK_SIZE_MAX)
		return -EINVAL;
	if (size && (addr & (ESP32S31_CACHE_LINE_SIZE - 1)))
		return -EINVAL;	/* must start on a cache line */

	en = sct ? ESP32S31_PRELOCK_SCT1_EN : ESP32S31_PRELOCK_SCT0_EN;

	raw_spin_lock_irqsave(&esp32s31_cache_lock, flags);

	conf = readl_relaxed(esp32s31_cache_base + ESP32S31_ICACHE1_PRELOCK_CONF);
	/* Drop the lock before moving it, so no stale range stays pinned. */
	writel_relaxed(conf & ~en,
		       esp32s31_cache_base + ESP32S31_ICACHE1_PRELOCK_CONF);

	if (!size) {
		raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);
		return 0;
	}

	writel_relaxed(addr, esp32s31_cache_base +
		       (sct ? ESP32S31_ICACHE1_PRELOCK_SCT1_ADDR :
			      ESP32S31_ICACHE1_PRELOCK_SCT0_ADDR));

	sizes = readl_relaxed(esp32s31_cache_base +
			      ESP32S31_ICACHE1_PRELOCK_SCT_SIZE);
	if (sct) {
		sizes &= ~(ESP32S31_PRELOCK_SIZE_MAX << ESP32S31_PRELOCK_SCT1_SIZE_S);
		sizes |= size << ESP32S31_PRELOCK_SCT1_SIZE_S;
	} else {
		sizes &= ~(ESP32S31_PRELOCK_SIZE_MAX << ESP32S31_PRELOCK_SCT0_SIZE_S);
		sizes |= size << ESP32S31_PRELOCK_SCT0_SIZE_S;
	}
	writel_relaxed(sizes, esp32s31_cache_base +
		       ESP32S31_ICACHE1_PRELOCK_SCT_SIZE);

	writel_relaxed(conf | en,
		       esp32s31_cache_base + ESP32S31_ICACHE1_PRELOCK_CONF);

	/*
	 * Preload the range. Without this the lock only reserves the lines;
	 * they are filled on first execution, which is the cost we are trying
	 * to avoid.
	 */
	writel_relaxed(addr, esp32s31_cache_base + ESP32S31_ICACHE1_PRELOAD_ADDR);
	writel_relaxed(min_t(u32, size, ESP32S31_PRELOAD_SIZE_MAX),
		       esp32s31_cache_base + ESP32S31_ICACHE1_PRELOAD_SIZE);
	writel_relaxed(ESP32S31_PRELOAD_ENA,
		       esp32s31_cache_base + ESP32S31_ICACHE1_PRELOAD_CTRL);
	/* ENA is self-clearing; wait for the fill to retire. */
	while (readl_relaxed(esp32s31_cache_base +
			     ESP32S31_ICACHE1_PRELOAD_CTRL) & ESP32S31_PRELOAD_ENA)
		cpu_relax();

	raw_spin_unlock_irqrestore(&esp32s31_cache_lock, flags);
	return 0;
}

static ssize_t esp32s31_prelock_write(struct file *file,
				      const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	unsigned int sct;
	u32 addr, size;
	char buf[64];
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%u %x %u", &sct, &addr, &size) != 3)
		return -EINVAL;

	ret = esp32s31_icache_prelock(sct, addr, size);
	if (ret)
		return ret;

	pr_info("ESP32-S31 cache: I-cache1 section %u %s 0x%08x+%u\n",
		sct, size ? "locked" : "released", addr, size);
	return len;
}

static int esp32s31_prelock_show(struct seq_file *m, void *v)
{
	u32 conf = readl_relaxed(esp32s31_cache_base +
				 ESP32S31_ICACHE1_PRELOCK_CONF);
	u32 sizes = readl_relaxed(esp32s31_cache_base +
				  ESP32S31_ICACHE1_PRELOCK_SCT_SIZE);

	seq_printf(m, "sct0 en=%u addr=0x%08x size=%lu\n",
		   !!(conf & ESP32S31_PRELOCK_SCT0_EN),
		   readl_relaxed(esp32s31_cache_base +
				 ESP32S31_ICACHE1_PRELOCK_SCT0_ADDR),
		   (unsigned long)((sizes >> ESP32S31_PRELOCK_SCT0_SIZE_S) &
				   ESP32S31_PRELOCK_SIZE_MAX));
	seq_printf(m, "sct1 en=%u addr=0x%08x size=%lu\n",
		   !!(conf & ESP32S31_PRELOCK_SCT1_EN),
		   readl_relaxed(esp32s31_cache_base +
				 ESP32S31_ICACHE1_PRELOCK_SCT1_ADDR),
		   (unsigned long)((sizes >> ESP32S31_PRELOCK_SCT1_SIZE_S) &
				   ESP32S31_PRELOCK_SIZE_MAX));
	seq_puts(m, "write: \"<sct 0|1> <hex addr, 64-byte aligned> <size, 0 releases>\"\n");
	return 0;
}

static int esp32s31_prelock_open(struct inode *inode, struct file *file)
{
	return single_open(file, esp32s31_prelock_show, inode->i_private);
}

static const struct file_operations esp32s31_prelock_fops = {
	.owner = THIS_MODULE,
	.open = esp32s31_prelock_open,
	.read = seq_read,
	.write = esp32s31_prelock_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct of_device_id esp32s31_cache_ids[] __initconst = {
	{ .compatible = "espressif,esp32s31-cache" },
	{ }
};

static int __init esp32s31_cache_init(void)
{
	struct device_node *np;
	u32 icache_id = 0;

	np = of_find_matching_node(NULL, esp32s31_cache_ids);
	if (!np)
		return -ENODEV;

	esp32s31_cache_base = of_iomap(np, 0);
	if (!esp32s31_cache_base) {
		of_node_put(np);
		return -ENOMEM;
	}

	of_property_read_u32(np, "espressif,icache-id", &icache_id);
	if (icache_id > 1) {
		pr_warn("ESP32-S31 cache: invalid I-cache ID %u, using 0\n",
			icache_id);
		icache_id = 0;
	}
	esp32s31_icache_map = icache_id ? ESP32S31_CACHE_MAP_ICACHE1 :
					 ESP32S31_CACHE_MAP_ICACHE0;
#ifdef CONFIG_SMP
	/*
	 * ONE shared D-cache, TWO I-caches: I-cache 0 is hart 0's, I-cache 1 is
	 * hart 1's. With a CPU lent by hart 0, executable memory that changes
	 * (a page of program text faulted in from the card, a breakpoint, a
	 * module) must be invalidated in BOTH, or that CPU runs stale
	 * instructions. Found by enumerating what is hart-specific, 2026-09-21,
	 * before it bit - PIE pinning had kept nearly everything on CPU0.
	 * FreeRTOS never executes from these PSRAM addresses, so invalidating
	 * its I-cache there costs it nothing.
	 */
	if (num_possible_cpus() > 1)
		esp32s31_icache_map = ESP32S31_CACHE_MAP_ICACHE0 |
				      ESP32S31_CACHE_MAP_ICACHE1;
#endif
	of_node_put(np);

	riscv_noncoherent_register_cache_ops(&esp32s31_cache_ops);
	pr_info("ESP32-S31 cache: 64-byte lines, I-cache %u, writeback workaround enabled\n",
		icache_id);
	return 0;
}
early_initcall(esp32s31_cache_init);

/*
 * Separate from the init above: cache maintenance is registered from an
 * early_initcall, long before debugfs exists, so creating the file there fails
 * silently and leaves no stats.
 */
static int __init esp32s31_cache_debugfs_init(void)
{
	if (esp32s31_cache_base) {
		struct dentry *d = debugfs_create_dir("esp32s31_cache", NULL);

		debugfs_create_file("stats", 0444, d, NULL,
				    &esp32s31_cache_stats_fops);
		debugfs_create_file("icache_prelock", 0644, d, NULL,
				    &esp32s31_prelock_fops);
	}
	return 0;
}
late_initcall(esp32s31_cache_debugfs_init);
