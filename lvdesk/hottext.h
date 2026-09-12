/*
 * Move chosen code out of XIP flash and into RAM, in place.
 *
 * WHY. Userspace here executes straight out of the XIP cramfs, which is the
 * feature - /usr/bin/lvdesk maps 856 kB at RssFile 8 kB - and also a tax
 * nobody had measured. rootfs/ramtext.c measures it on this board:
 *
 *     footprint                      flash      RAM
 *     322-byte pixel loop            3623 us    3625 us     nothing
 *     41 KB straight-line, in place  1178 us     221 us     4.8x
 *
 * A tight loop fits in the instruction cache and cannot care where it is
 * backed. Code that does NOT fit pays 4.8x. That is the shape of LVGL, the
 * event plumbing and the request dispatch - and it is where lvdesk's CPU goes
 * that the present-path timers cannot account for: 37% of the machine while a
 * fullscreen game runs, of which the present path explains about a third.
 *
 * HOW. text_to_ram() saves the whole pages an address range touches, maps
 * anonymous RW over exactly those pages with MAP_FIXED, writes the bytes
 * back, then mprotects R+X. The code ends up at the SAME address, so every
 * auipc, jal and GOT reference is still correct. Saving whole pages means
 * anything sharing the first or last page survives byte for byte, so the
 * range need not be page aligned.
 *
 * The alternative - copy the function elsewhere and call it there - works
 * only for a self-contained leaf, because real code reaches its globals and
 * its neighbours PC-relative. Do not retry it.
 *
 * USE. Mark a function HOTTEXT to put it in the section that gets moved.
 * Marking a tight pixel loop is pointless (see the table); mark the SCATTERED
 * code - dispatch, event handling, the paths with many callees.
 *
 * The mover itself and everything it calls must not be inside the range it is
 * moving: between the mmap and the memcpy those pages are zeros, and code
 * executing there dies. HOTTEXT code is in text_hot; the mover is in
 * text_mover; libc is a different mapping entirely.
 */
#ifndef LVDESK_HOTTEXT_H
#define LVDESK_HOTTEXT_H

#include <stddef.h>

#define HOTTEXT __attribute__((section("text_hot")))
/*
 * aligned(4096) is load-bearing: it pads text_hot up to a page boundary, so
 * the mover never shares a page with the code it is moving. Without it ld puts
 * text_mover immediately after text_hot, the page-rounded range swallows the
 * mover, and the tail of the hot section has to be clipped and left in flash.
 */
#define TEXTMOVER __attribute__((noinline, aligned(4096), \
				 section("text_mover")))

/*
 * Bytes moved, or -1. Safe to call more than once; moving an already-anonymous
 * range just copies it to itself.
 */
long text_to_ram(void *start, void *stop);

/*
 * Move the whole text_hot section. Reports what it did on stderr, because a
 * silent optimisation that failed is indistinguishable from one that is not
 * helping - which has cost this project days more than once.
 */
void hottext_init(void);

#endif
