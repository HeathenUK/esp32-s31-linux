"""Apply every known ESP32-S31 fix for kernels newer than 6.12, idempotently."""
import os, re, sys
B = sys.argv[1].rstrip('/') + '/'

def ed(path, pairs, label):
    p = B + path
    if not os.path.exists(p):
        print('  %-42s (file absent)' % label); return
    s = open(p).read(); n = 0
    for a, b in pairs:
        if a in s and b not in s:
            s = s.replace(a, b, 1); n += 1
    open(p, 'w').write(s)
    print('  %-42s %d edit(s)' % (label, n))

# --- hand ports (context differs per release) ---
ed('arch/riscv/include/asm/cacheflush.h', [(
"""#define flush_icache_user_page(vma, pg, addr, len)	\\
do {							\\
	if (vma->vm_flags & VM_EXEC)			\\
		flush_icache_mm(vma->vm_mm, 0);		\\
} while (0)""",
"""#define flush_icache_user_page(vma, pg, addr, len)		\\
do {								\\
	if (vma->vm_flags & VM_EXEC) {				\\
		esp32s31_cache_sync_for_exec(page_to_phys(pg), PAGE_SIZE); \\
		flush_icache_mm(vma->vm_mm, 0);			\\
	}							\\
} while (0)""")], 'cacheflush.h user_page')

for flags in ('flags.f', 'flags'):
    ed('arch/riscv/mm/cacheflush.c', [(
"""	struct folio *folio = page_folio(pte_page(pte));

	if (!test_bit(PG_dcache_clean, &folio->%s)) {
		flush_icache_mm(mm, false);
		set_bit(PG_dcache_clean, &folio->%s);
	}""" % (flags, flags),
"""	struct folio *folio;

	/*
	 * XIP cramfs text is mapped with remap_pfn_range() from a flash window
	 * below PHYS_RAM_BASE, so it has no memmap entry and page_folio()
	 * would walk a wild pointer during execve.
	 */
	if (!pfn_valid(pte_pfn(pte))) {
		flush_icache_mm(mm, false);
		return;
	}

	folio = page_folio(pte_page(pte));

	if (!test_bit(PG_dcache_clean, &folio->%s)) {
		esp32s31_cache_sync_for_exec(page_to_phys(pte_page(pte)),
					    PAGE_SIZE);
		flush_icache_mm(mm, false);
		set_bit(PG_dcache_clean, &folio->%s);
	}""" % (flags, flags))], 'cacheflush.c flush_icache_pte (%s)' % flags)

ed('drivers/mmc/host/dw_mmc.c', [
 ('\tret = dw_mci_hsq_init(host, mmc);\n\tif (ret)\n\t\tgoto err_host_allocated;',
  '\tret = dw_mci_hsq_init(host, mmc);\n\tif (ret)\n\t\treturn ret;'),
 ('\thrtimer_init(&host->irq_poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);\n\thost->irq_poll_timer.function = dw_mci_irq_poll_timer;',
  '\thrtimer_setup(&host->irq_poll_timer, dw_mci_irq_poll_timer,\n\t\t      CLOCK_MONOTONIC, HRTIMER_MODE_REL);'),
], 'dw_mmc.c')

# --- API changes ---
ed('arch/riscv/Kconfig.socs', [(
 'config SOC_ESP32S31\n\tbool "Espressif ESP32-S31 SoC"\n\tselect ESP32S31_CLIC\n',
 'config SOC_ESP32S31\n\tbool "Espressif ESP32-S31 SoC"\n\tselect ESP32S31_CLIC\n'
 '\t# Not in ESP32S31_CACHE: CACHEMAINT_FOR_DMA is a menuconfig, and selecting\n'
 '\t# from inside it loops with ERRATA_STARFIVE_JH7100.\n\tselect DMA_DIRECT_REMAP\n')], 'Kconfig.socs DMA_DIRECT_REMAP')
ed('drivers/cache/Kconfig', [('\tselect DMA_DIRECT_REMAP\n\tselect RISCV_DMA_NONCOHERENT',
 '\t# DMA_DIRECT_REMAP is selected by SOC_ESP32S31.\n\tselect RISCV_DMA_NONCOHERENT')], 'cache/Kconfig')

ed('drivers/pinctrl/pinctrl-esp32s31.c', [
 ('''	{
		struct function_desc *function = pinmux_generic_get_function(pctldev,
									 ret);

		/* pinmux_generic_add_function returns the existing selector too. */
		function->func.groups = group_names;
		function->func.ngroups = ngroups;
	}
''', '''	/* function_desc holds a const struct pinfunction * now; add_function
	 * already builds it from the groups passed above. */
'''),
 ('		case PIN_CONFIG_OUTPUT:\n\t\t\tpctl->gc.direction_output(&pctl->gc, pin, arg);\n\t\t\tcontinue;',
  '		case PIN_CONFIG_LEVEL: {\t/* PIN_CONFIG_OUTPUT before 6.18 */\n'
  '\t\t\tint err = pctl->gc.direction_output(&pctl->gc, pin, arg);\n\n'
  '\t\t\tif (err)\n\t\t\t\treturn err;\n\t\t\tcontinue;\n\t\t}'),
 ('static void esp32s31_gpio_set(struct gpio_chip *gc, unsigned int offset,\n\t\t\t      int value)\n{',
  'static int esp32s31_gpio_set(struct gpio_chip *gc, unsigned int offset,\n\t\t\t     int value)\n{'),
 ('\twritel_relaxed(BIT(offset % 32), pctl->gpio_base + reg);\n}\n\nstatic int esp32s31_gpio_direction_output(',
  '\twritel_relaxed(BIT(offset % 32), pctl->gpio_base + reg);\n\n\treturn 0;\n}\n\nstatic int esp32s31_gpio_direction_output('),
 ('static void esp32s31_gpio_set_multiple(struct gpio_chip *gc,\n\t\t\t\t       unsigned long *mask,\n\t\t\t\t       unsigned long *bits)\n{',
  'static int esp32s31_gpio_set_multiple(struct gpio_chip *gc,\n\t\t\t\t      unsigned long *mask,\n\t\t\t\t      unsigned long *bits)\n{'),
 ('\twritel_relaxed((u32)((m & ~b) >> 32), pctl->gpio_base + GPIO_OUT1_W1TC);\n}',
  '\twritel_relaxed((u32)((m & ~b) >> 32), pctl->gpio_base + GPIO_OUT1_W1TC);\n\n\treturn 0;\n}'),
], 'pinctrl-esp32s31.c')

for f in ('drivers/net/ethernet/espressif/esp32s31-hosted-sram.c','drivers/dma/esp32s31-axi-gdma.c',
          'drivers/tty/serial/esp32_uart.c'):
    p = B + f
    if os.path.exists(p):
        s = open(p).read(); n = s.count('.remove_new')
        open(p,'w').write(s.replace('.remove_new', '.remove'))
        print('  %-42s %d edit(s)' % (os.path.basename(f) + ' remove_new', n))

ed('drivers/gpu/drm/espressif/esp32s31-lcd.c', [
 ('\thrtimer_init(&lcd->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);\n\tlcd->vblank_timer.function = esp32s31_lcd_vblank_tick;',
  '\thrtimer_setup(&lcd->vblank_timer, esp32s31_lcd_vblank_tick,\n\t\t      CLOCK_MONOTONIC, HRTIMER_MODE_REL);'),
 ('\tDRM_GEM_DMA_DRIVER_OPS_VMAP,\n\t.name\t\t\t= DRIVER_NAME,',
  '\tDRM_GEM_DMA_DRIVER_OPS_VMAP,\n\tDRM_FBDEV_DMA_DRIVER_OPS,\n\t.name\t\t\t= DRIVER_NAME,'),
 ('\tdrm_fbdev_dma_setup(drm, 16);', '\tdrm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);'),
 ('#include <drm/drm_fbdev_dma.h>', '#include <drm/clients/drm_client_setup.h>\n#include <drm/drm_fbdev_dma.h>'),
], 'esp32s31-lcd.c')

ed('drivers/net/ethernet/espressif/esp32s31-hosted-sram.c', [
 ('\t.attr = cpufreq_generic_attr,\n', '\t/* cpufreq_generic_attr removed; the core publishes it. */\n'),
 ('static int s31_wifi_get_station(struct wiphy *wiphy, struct net_device *ndev,',
  'static int s31_wifi_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,'),
 ('#include <linux/etherdevice.h>', '#include <linux/hex.h>\n#include <linux/etherdevice.h>'),
], 'hosted-sram.c')

ed('drivers/crypto/esp32s31-crypto.c', [
 ('\t.sign = s31_rsa_decrypt,\n\t.verify = s31_rsa_encrypt,\n',
  '\t/* .sign/.verify moved to the "sig" alg type. */\n')], 'esp32s31-crypto.c')
ed('drivers/iio/proximity/esp32s31-comparator.c',
   [('\t\t\t\t\t    int state)', '\t\t\t\t\t    bool state)')], 'comparator bool state')

for f in ('drivers/counter/esp32s31-pcnt.c','drivers/counter/esp32s31-system-timers.c'):
    p = B + f
    if os.path.exists(p):
        s = open(p).read()
        s2, n = re.subn(r'MODULE_IMPORT_NS\(([A-Za-z_][A-Za-z0-9_]*)\)', r'MODULE_IMPORT_NS("\1")', s)
        open(p,'w').write(s2); print('  %-42s %d edit(s)' % (os.path.basename(f) + ' NS', n))

ed('drivers/clk/clk-esp32s31.c', [(
"""static long esp32s31_txc_round_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate)
{
	if (rate >= 125000000)
		return 125000000;
	if (rate >= 25000000)
		return 25000000;

	return 2500000;
}""",
"""static int esp32s31_txc_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	if (req->rate >= 125000000)
		req->rate = 125000000;
	else if (req->rate >= 25000000)
		req->rate = 25000000;
	else
		req->rate = 2500000;

	return 0;
}"""),
 ('\t.round_rate = esp32s31_txc_round_rate,', '\t.determine_rate = esp32s31_txc_determine_rate,'),
], 'clk determine_rate')

ed('sound/drivers/esp32s31-audio.c',
   [('.pcm_construct = s31_pcm_construct,', '.pcm_new = s31_pcm_construct,')], 'ASoC pcm_new')
p = B + 'sound/soc/codecs/es8389.c'
if os.path.exists(p):
    s = open(p).read()
    n = s.count('snd_soc_dapm_kcontrol_component(') + s.count('snd_soc_dapm_kcontrol_dapm(')
    s = s.replace('snd_soc_dapm_kcontrol_component(', 'snd_soc_dapm_kcontrol_to_component(')
    s = s.replace('snd_soc_dapm_kcontrol_dapm(', 'snd_soc_dapm_kcontrol_to_dapm(')
    open(p,'w').write(s); print('  %-42s %d edit(s)' % ('es8389 dapm helpers', n))

# --- the two that decide whether it boots at all ---
p = B + 'arch/riscv/include/asm/runtime-const.h'
if os.path.exists(p):
    s = open(p).read()
    if 'CONFIG_XIP_KERNEL' not in s:
        i = s.index('\n', s.index('#define _ASM_RISCV_RUNTIME_CONST_H')) + 1
        s = s[:i] + ('\n/*\n * runtime_const_init() rewrites lui/addi parcels in place; under XIP that\n'
                     ' * memory is read-only flash. This is the breakage named in 9b3a2be84803.\n */\n'
                     '#ifdef CONFIG_XIP_KERNEL\n#include <asm-generic/runtime-const.h>\n#else\n') + s[i:]
        last = s.rindex('#endif')
        s = s[:last] + '#endif /* CONFIG_XIP_KERNEL */\n\n' + s[last:]
        open(p,'w').write(s); print('  %-42s applied' % 'runtime-const XIP fallback')
    else:
        print('  %-42s already present' % 'runtime-const XIP fallback')
else:
    print('  %-42s (no riscv runtime-const: XIP unaffected)' % 'runtime-const')

p = B + 'arch/riscv/kernel/signal.c'
s = open(p).read()
if 'total_context_size += ESP32S31_EXT_SC_SIZE' not in s:
    m = re.search(r'\n(\tframe_size \+= total_context_size;)', s)
    assert m, 'frame size site'
    s = s[:m.start()] + ('\n#ifdef CONFIG_SOC_ESP32S31\n'
        '\t/*\n\t * save_esp32s31_ext_state() always appends a record, so the frame\n'
        '\t * must be sized for it or it lands over the user stack and\n'
        '\t * rt_sigreturn restores a corrupt context.\n\t */\n'
        '\ttotal_context_size += ESP32S31_EXT_SC_SIZE;\n#endif\n\n') + m.group(1) + s[m.end():]
    open(p,'w').write(s); print('  %-42s applied' % 'signal frame sized for ext record')
else:
    print('  %-42s already present' % 'signal frame sized')
