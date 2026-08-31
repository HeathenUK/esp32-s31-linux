# Touch on the Korvo-1: GT1158, and why it does not run

**RESOLVED 2026-08-31: the conclusion below was WRONG - touch works, with no
INT, no RESET, no config write.** The vendor component (esp_lcd_touch_gt1151,
used by the factory demo) does only two things this investigation did not:
it acknowledges 0x814E by writing 0 on EVERY poll cycle - data or not - and
it never expects zero-touch frames to keep arriving while nobody touches the
panel. Probed on this exact board with that rhythm: the controller publishes
0x80 (ready, no touch) continuously at idle and clean checksummed frames
under a finger. The "runtime resolution 257x2 garbage" below was GT9xx
register lore applied to a GT1x part. The shipping driver is
drivers/input/touchscreen/gt1158_polled.c; the one non-obvious kernel-side
trap was INPUT_MT_DROP_UNUSED (without it BTN_TOUCH latches after the first
ever touch and no tap ever clicks). Everything below is preserved as the
record of a wrong conclusion confidently held.

Status: ~~blocked on hardware, not software~~ **WRONG - see above.** The controller is present and
correctly configured, but is not scanning, and the two lines normally used to
start it are not wired to the SoC.

## What is established

- The part is a **GT1158**, not the GT1151 the board comments claim. It reports
  `1158` at `GOODIX_REG_ID` (0x8140), firmware 0x0100, sensor ID 0.
- It sits at **I2C address 0x14** on i2c-0, the same bus as the ES8389 codec,
  and answers register reads reliably.
- Its stored configuration is **valid and correct for this panel**: version 131,
  800x480, 5 touch points.
- **The kernel's config length is wrong for this part.** `goodix.c` uses
  `GOODIX_CONFIG_GT9X_LENGTH` = 240 for the gt1x family. Solved against the
  controller, exactly one length between 60 and 240 makes the stored checksum
  match a checksum computed over the block: **239** - checksum at bytes
  236..237 (0x4261), `config_fresh` at 238. At 240 the driver's
  `goodix_check_cfg_16` would reject a config that is in fact valid.
- **It is not scanning.** `READ_COOR_ADDR` (0x814E) held one stale frame from
  power-on; once acknowledged it reads 0x00 indefinitely. A scanning Goodix part
  republishes frames continuously, including zero-point frames on release.
- The runtime resolution registers at 0x8146 read `257x2` - garbage - while the
  stored config reads 800x480. So the firmware has not applied its own config.

## What was tried

- Writing 0x00 (normal read mode) and 0x02 (software reset) to the command
  register 0x8040: no effect.
- Rewriting the full 239-byte config with a correct checksum and the
  `config_fresh` flag set, which is what `goodix_send_cfg()` does and the
  standard way to restart a GT1x without toggling hardware: accepted, but the
  controller still reports `257x2` and still does not scan.

## Why this is hard

Espressif's own BSP for this board marks **both** touch lines unconnected:

    #define BSP_LCD_TOUCH_INT    (GPIO_NUM_NC)
    #define BSP_LCD_RST          (GPIO_NUM_NC)

The BSP header is otherwise an exact match for this board - the LCD data pins
(8-19, 33-36) and the GPIO38 LCD_CS note both agree with our loader - so it is
describing our hardware. Goodix parts use INT and RESET during power-on to
select the I2C address and start the sensing loop, and `goodix_resume()` exits
sleep by driving INT high for 2-5 ms. With neither line reachable, there is no
way to reset or wake the controller from the SoC.

Note the contradiction worth resolving: the same BSP's README lists touch as
supported via `espressif/esp_lcd_touch_gt1151`, yet that component's init is only
an optional reset plus an ID read - nothing that would start a halted controller.
Either their board revision wires the reset, or their controller scans from
power-on where ours stops after one frame.

## If picked up again

1. Check the V1.1 schematic for where touch RESET and INT actually go - they may
   be tied to a rail, an IO expander, or the LCD connector rather than the SoC.
2. Failing that, confirm whether the factory demo gets working touch on this
   exact board. If it does, the difference is in software and worth diffing.
3. The kernel driver needs work regardless: it requires an interrupt
   (`devm_request_threaded_irq` is unconditional), so a poll mode would be needed
   even once the controller runs. The config-length bug above should go upstream.

`rootfs/gtcfg.c` reads, validates and optionally rewrites the config. It refuses
to write unless its computed checksum matches what the controller already
stores, because a rejected config is only recoverable by power cycling.
