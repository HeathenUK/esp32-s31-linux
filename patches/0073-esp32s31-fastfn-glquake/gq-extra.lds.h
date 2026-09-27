/* static duplicates the picker skips as ambiguous, resolved to their object by
   their flash neighbours in `nm -nS vmlinux` of #393 (samples over prof9+prof10a-c) */
		*(.text..fast.rcu_qs)	/* kernel/rcu/tree.o 80 B 43 samples */
		*(.text..fast.mmiowb_set_pending)	/* drivers/irqchip/irq-esp32s31-clic.o 36 B 21 samples */
		*(.text..fast.__raw_spin_unlock)	/* kernel/irq/chip.o 22 B 20 samples */
		*(.text..fast.div_u64_rem)	/* lib/math/div64.o 42 B 17 samples; kernel/sched/build_policy.o 46 B 13 samples */
		*(.text..fast.mmiowb_spin_unlock)	/* kernel/irq/chip.o 48 B 11 samples */
		*(.text..fast.files_lookup_fd_raw)	/* fs/file.o 30 B 10 samples */
		*(.text..fast.irqd_set.isra.0)	/* kernel/irq/chip.o 8 B 8 samples */
