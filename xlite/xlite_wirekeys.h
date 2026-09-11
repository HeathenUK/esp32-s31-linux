/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The keyboard wire contract between lvdesk's X shim and xlite.
 *
 * xlite documents that "a keycode IS its keysym" - the server has no keymap
 * and the client does no translation. That identity fits the wire for
 * Latin-1: a KeyPress carries its keycode in ONE byte (detail), and
 * printable characters are their own keysyms. It cannot fit Return (0xFF0D)
 * or an arrow key. So the server sends these low codes - none of which is a
 * printable keysym - and xlite widens them back to the real keysym while
 * decoding the event. Both sides include THIS header; a table in only one of
 * them is how the two ends drift.
 */
#ifndef XLITE_WIREKEYS_H
#define XLITE_WIREKEYS_H

#define XLW_RETURN	1	/* 0xFF0D */
#define XLW_BACKSPACE	2	/* 0xFF08 */
#define XLW_TAB		3	/* 0xFF09 */
#define XLW_ESCAPE	4	/* 0xFF1B */
#define XLW_DELETE	5	/* 0xFFFF */
#define XLW_LEFT	6	/* 0xFF51 */
#define XLW_UP		7	/* 0xFF52 */
#define XLW_RIGHT	8	/* 0xFF53 */
#define XLW_DOWN	9	/* 0xFF54 */
#define XLW_HOME	10	/* 0xFF50 */
#define XLW_END		11	/* 0xFF57 */
#define XLW_PRIOR	12	/* 0xFF55 Page_Up */
#define XLW_NEXT	13	/* 0xFF56 Page_Down */
#define XLW_INSERT	14	/* 0xFF63 */
#define XLW_KPENTER	15	/* 0xFF8D */
#define XLW_F1		16	/* 0xFFBE .. F12 = 27 */
/*
 * Modifier keys are keys. They were missing from this table, so lvdesk
 * tracked Shift/Ctrl/Alt as STATE and never sent them as events - and a
 * client that binds the key itself, rather than reading the modifier mask
 * on some other key, saw nothing at all. Doom fires on Ctrl, strafes on Alt
 * and runs on Shift: its menus worked (Return and the arrows are here) and
 * the game was unplayable, which is exactly how the gap presents.
 */
#define XLW_SHIFT_L	28	/* 0xFFE1 */
#define XLW_SHIFT_R	29	/* 0xFFE2 */
#define XLW_CONTROL_L	30	/* 0xFFE3 */
#define XLW_CONTROL_R	31	/* 0xFFE4 */
#define XLW_CAPS_LOCK	32	/* 0xFFE5 */
#define XLW_ALT_L	33	/* 0xFFE9 */
#define XLW_ALT_R	34	/* 0xFFEA */
#define XLW_SUPER_L	35	/* 0xFFEB */
#define XLW_SUPER_R	36	/* 0xFFEC */

static inline unsigned int xlw_widen(unsigned int c)
{
	static const unsigned short t[] = {
		0,      0xFF0D, 0xFF08, 0xFF09, 0xFF1B, 0xFFFF, 0xFF51,
		0xFF52, 0xFF53, 0xFF54, 0xFF50, 0xFF57, 0xFF55, 0xFF56,
		0xFF63, 0xFF8D, 0xFFBE, 0xFFBF, 0xFFC0, 0xFFC1, 0xFFC2,
		0xFFC3, 0xFFC4, 0xFFC5, 0xFFC6, 0xFFC7, 0xFFC8, 0xFFC9,
		0xFFE1, 0xFFE2, 0xFFE3, 0xFFE4, 0xFFE5, 0xFFE9, 0xFFEA,
		0xFFEB, 0xFFEC,
	};

	return c < sizeof(t) / sizeof(t[0]) ? t[c] : c;
}

#endif
