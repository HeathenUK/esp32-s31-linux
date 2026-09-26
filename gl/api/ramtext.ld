/*
 * ramtext.ld - lever L1 (tinygl/source/s31_ramtext.c): the hot functions
 * (sections renamed "s31hot_text" by build-lib.sh from api/ramtext.list)
 * as one contiguous range, [__s31hot_start, __s31hot_end), right after
 * .text. s31, MIT.
 *
 * In front of it go the generated GL stubs (api_gen_stubs.o: entry points
 * TinyGL does not implement, ~12 kB that nothing inside libGL calls). The
 * RISC-V linker relaxes a call to a 2-byte c.jal/c.j when its target is
 * within 2 KiB, and a compressed jump out of the range could not be fixed
 * once the range moves; with the stubs in front, the nearest code the hot
 * range can call is more than 2 KiB away. Behind it come .fini and
 * .rodata, which it does not call either. gl/api/ramtext.py checks all of
 * this on the linked file and fails the build if it does not hold.
 *
 * Used with INSERT, so it augments the default linker script (shared
 * library and static test links alike) instead of replacing it.
 */
SECTIONS
{
  .s31hot : ALIGN(32)
  {
    *api_gen_stubs.o(.text .text.*)
    . = ALIGN(32);
    HIDDEN(__s31hot_start = .);
    KEEP(*(s31hot_text))
    KEEP(*(s31hot_rodata))      /* their jump tables (ramtext.py) */
    HIDDEN(__s31hot_end = .);
  }
  ASSERT(__s31hot_start - ADDR(.s31hot) >= 2048,
         "ramtext.ld: fewer than 2 KiB of stubs in front of the hot range")
}
INSERT AFTER .text;
