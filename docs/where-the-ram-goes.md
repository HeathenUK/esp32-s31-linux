# Where the 16 MB actually goes

Measured, not estimated. Two kernels were used: the shipping one, and a
diagnostic build with `CONFIG_SLUB_TINY` off and `CONFIG_SLUB_DEBUG` on, which
is the only way to get `/proc/slabinfo` here.

## The top-level split

	 PSRAM on the die                     16,384 kB
	 MemTotal Linux sees                  15,428 kB
	 -> never reaches the allocator          956 kB

That 956 kB is OpenSBI, the audio SRAM ring reservation and the kernel's own
`.data`/`.bss`. Kernel *text* costs nothing here because it executes XIP from
flash.

Of the 15,428 kB, with nothing running but init and the network:

	 Slab                4,448 kB   29%
	 Page cache          3,696 kB   (Cached 2,880 + Buffers 816)
	 MemFree             3,320 kB
	 Vmalloc               880 kB
	 AnonPages             772 kB
	 KernelStack           344 kB
	 PageTables            300 kB

## Slab is the biggest single consumer, and it is mostly self-description

From the diagnostic kernel, caches over 32 kB:

	   783 kB  kernfs_node_cache     9,108 objects
	   384 kB  kmalloc-1k
	   324 kB  kmalloc-64
	   311 kB  debugfs_inode_cache     948 objects
	   305 kB  radix_tree_node
	   295 kB  dentry
	   270 kB  biovec-max               90 objects @ 3 kB
	   211 kB  shmem_inode_cache
	   208 kB  kmalloc-512
	   204 kB  kmalloc-128
	   200 kB  inode_cache
	   192 kB  kmalloc-8k               24 objects @ 8 kB
	   191 kB  task_struct              90 objects
	   126 kB  mm_struct
	   110 kB  ext4_groupinfo_4k

**`kernfs_node_cache` + `debugfs_inode_cache` is about 1.1 MB - roughly 7% of
the entire machine spent describing itself through sysfs and debugfs.** That is
the single most striking number here, and it is the one genuinely tailorable to
an embedded target.

## MemAvailable is understated by about 1.5 MB

The shipping kernel reports:

	 SReclaimable        0 kB
	 SUnreclaim      4,448 kB

Every byte of slab unreclaimable is not plausible, and it is not true. It is
`CONFIG_SLUB_TINY`, which does not do reclaimable-slab accounting. The
diagnostic kernel, same workload, reports **SReclaimable 1,556 kB**.

So the dentry and inode caches *are* reclaimable; the kernel simply is not
counting them, and `MemAvailable` - which is computed from that figure -
understates what is actually available by around 1.5 MB. Any decision made on
`MemAvailable` on this board is being made on a pessimistic number.

## What is worth cutting

In rough order of return:

1. **debugfs.** 311 kB of inodes outright, plus a large share of those 9,108
   kernfs nodes. It exists here only for instrumentation - the LCD driver's
   counters, `deskbench`'s scanout discovery, the SD tracing. The obvious shape
   is to ship with `CONFIG_DEBUG_FS` off and keep a diagnostic build with it on,
   exactly as is already done for `CONFIG_PROFILING`.
2. **sysfs node count.** 783 kB scales with how many devices and attributes are
   registered, so it falls out of trimming drivers rather than being tunable
   directly.
3. **90 `task_struct`s** at 2,176 bytes each, plus 344 kB of kernel stacks. That
   is a lot of kernel threads for this machine; most come from subsystems we
   could compile out.
4. **`biovec-max`, 270 kB** in 90 objects of 3 kB - block-layer preallocation,
   sized for a machine much larger than this one.

## What is *not* the problem

Userspace. With the LVGL desktop running, `lvdesk` is 1,264 kB RSS and the
largest other process is a 580 kB shell. The X11 stack it replaced was ~4.7 MB
for Xorg alone, which was worth removing - but even so, the kernel's own
metadata outweighs everything userspace does on this board.


## Measured: what turning debugfs off actually returns

`make linux DIAG=0` compiles out `CONFIG_DEBUG_FS`. Same workload, LVGL desktop
running, both measured on a cold boot:

	                    DIAG=1        DIAG=0 (ship)
	 Slab              4,448 kB        4,040 kB
	 MemAvailable      3,584-3,648     4,020 kB
	 kernel image      6,021,185       5,787,553 bytes

**About 400 kB of slab and 400 kB of MemAvailable, plus 233 kB of flash.**

That is less than this document first suggested. The earlier reading paired
`debugfs_inode_cache` (311 kB) with `kernfs_node_cache` (783 kB) and implied
roughly 1.1 MB was available. It is not: **most of those kernfs nodes back
sysfs, which stays**, and only debugfs's share leaves. 400 kB on a 15.4 MB
board is still worth having - it is two thirds of the LVGL desktop's entire
resident size - but the 1.1 MB figure was wrong and should not be quoted.

The cost is that `/sys/kernel/debug` disappears, taking the LCD driver's
counters, the PPA register dump and `deskbench`'s scanout discovery with it. So
`DIAG=1` stays the default for development and `DIAG=0` is the shipping build,
exactly as `CONFIG_PROFILING` is already handled.
