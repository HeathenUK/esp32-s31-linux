"""The fixes learned from 7.1: dw_mmc refactor + DRM plumbing."""
import os, sys
B = sys.argv[1].rstrip('/') + '/'
def rd(p): return open(B+p).read()
def wr(p,s): open(B+p,'w').write(s)

# 1. descriptor advance must keep one descriptor per 64-byte cache line;
#    dw_mci_idmac_init lays the ring out that way, so prepare_desc must match.
p='drivers/mmc/host/dw_mmc.c'; s=rd(p)
old = """			if (is_64bit) {
				desc64_last = desc64;
				desc64++;
			} else {
				desc_last = desc;
				desc++;
			}"""
new = """			if (is_64bit) {
				desc64_last = desc64;
				desc64 += 4;
			} else {
				desc_last = desc;
				desc += 4;
			}"""
if old in s: s=s.replace(old,new,1); print('  descriptor stride -> += 4')
# 2. our budget-loop handler needs its lock initialised
if 'spin_lock_init(&host->irq_handler_lock)' not in s and 'irq_handler_lock' in s:
    a='\tspin_lock_init(&host->lock);'
    if a in s: s=s.replace(a, a+'\n\tspin_lock_init(&host->irq_handler_lock);',1); print('  spin_lock_init(irq_handler_lock)')
# 3. low_pwr guard so the module parameter is live
o='\t\tif (!test_bit(DW_MMC_CARD_NO_LOW_PWR, &host->flags))'
n='\t\tif (low_pwr && !test_bit(DW_MMC_CARD_NO_LOW_PWR, &host->flags))'
if o in s and n not in s: s=s.replace(o,n,1); print('  low_pwr guard')
# 4. OWN-bit poll must invalidate each iteration on a noncoherent SoC
old = """			if (is_64bit)
				err = readl_poll_timeout_atomic(&desc64->des0, val,
					IDMAC_OWN_CLR64(val), 10, 100 * USEC_PER_MSEC);
			else
				err = readl_poll_timeout_atomic(&desc->des0, val,
					IDMAC_OWN_CLR64(val), 10, 100 * USEC_PER_MSEC);
			if (err)
				goto err_own_bit;"""
new = """			if (dw_mci_idmac_desc_noncoherent(host)) {
				/*
				 * readl_poll_timeout_atomic() reads the
				 * descriptor directly, and that memory is
				 * cached here - the IDMAC's clear of OWN stays
				 * invisible until the line is invalidated.
				 */
				int retries = 10000;

				do {
					if (is_64bit) {
						dw_mci_idmac_sync_desc_for_cpu(host, desc64,
									       sizeof(*desc64));
						val = READ_ONCE(desc64->des0);
					} else {
						dw_mci_idmac_sync_desc_for_cpu(host, desc,
									       sizeof(*desc));
						val = READ_ONCE(desc->des0);
					}
					if (IDMAC_OWN_CLR64(val))
						break;
					udelay(10);
				} while (--retries);
				if (!retries)
					goto err_own_bit;
			} else {
				if (is_64bit)
					err = readl_poll_timeout_atomic(&desc64->des0, val,
						IDMAC_OWN_CLR64(val), 10, 100 * USEC_PER_MSEC);
				else
					err = readl_poll_timeout_atomic(&desc->des0, val,
						IDMAC_OWN_CLR64(val), 10, 100 * USEC_PER_MSEC);
				if (err)
					goto err_own_bit;
			}"""
if old in s: s=s.replace(old,new,1); print('  noncoherent OWN-bit poll')
wr(p,s)

# 5. DRM Kconfig source line
p='drivers/gpu/drm/Kconfig'; s=rd(p)
if 'espressif/Kconfig' not in s:
    for a in ('source "drivers/gpu/drm/tiny/Kconfig"\n','source "drivers/gpu/drm/panel/Kconfig"\n'):
        if a in s:
            s=s.replace(a, a+'\nsource "drivers/gpu/drm/espressif/Kconfig"\n',1); wr(p,s)
            print('  DRM Kconfig source line'); break

# 6. drm_print.h is no longer transitively included
p='drivers/gpu/drm/espressif/esp32s31-lcd.c'; s=rd(p)
if '#include <drm/drm_print.h>' not in s:
    a='#include <drm/drm_fbdev_dma.h>'
    if a in s: wr(p, s.replace(a,'#include <drm/drm_print.h>\n'+a,1)); print('  drm_print.h')
