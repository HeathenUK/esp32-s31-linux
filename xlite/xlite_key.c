/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Keysyms, and the window-manager property calls.
 *
 * The keysym table is not decoration: Xt parses an application's translation
 * table by resolving every name in it, so a stubbed XStringToKeysym makes a
 * perfectly good table fail to compile and the application comes up with no
 * keyboard bindings at all. xcalc's table alone names Return, BackSpace,
 * Delete, Clear, space and the whole KP_ keypad.
 *
 * Printable ASCII needs no table: for Latin-1 the keysym IS the character
 * code, which is most of the entries a client asks for.
 */
#include "xlite.h"
#include "xlite_wirekeys.h"

#include <X11/keysym.h>

static const struct { const char *name; KeySym ks; } knames[] = {
	{ "BackSpace", 0xFF08 }, { "Tab", 0xFF09 }, { "Linefeed", 0xFF0A },
	{ "Clear", 0xFF0B },     { "Return", 0xFF0D }, { "Pause", 0xFF13 },
	{ "Escape", 0xFF1B },    { "Delete", 0xFFFF }, { "space", 0x20 },
	{ "Home", 0xFF50 }, { "Left", 0xFF51 }, { "Up", 0xFF52 },
	{ "Right", 0xFF53 }, { "Down", 0xFF54 }, { "Prior", 0xFF55 },
	{ "Page_Up", 0xFF55 }, { "Next", 0xFF56 }, { "Page_Down", 0xFF56 },
	{ "End", 0xFF57 }, { "Begin", 0xFF58 }, { "Insert", 0xFF63 },
	{ "KP_Space", 0xFF80 }, { "KP_Tab", 0xFF89 }, { "KP_Enter", 0xFF8D },
	{ "KP_Multiply", 0xFFAA }, { "KP_Add", 0xFFAB },
	{ "KP_Separator", 0xFFAC }, { "KP_Subtract", 0xFFAD },
	{ "KP_Decimal", 0xFFAE }, { "KP_Divide", 0xFFAF },
	{ "KP_0", 0xFFB0 }, { "KP_1", 0xFFB1 }, { "KP_2", 0xFFB2 },
	{ "KP_3", 0xFFB3 }, { "KP_4", 0xFFB4 }, { "KP_5", 0xFFB5 },
	{ "KP_6", 0xFFB6 }, { "KP_7", 0xFFB7 }, { "KP_8", 0xFFB8 },
	{ "KP_9", 0xFFB9 }, { "KP_Equal", 0xFFBD },
	{ "F1", 0xFFBE }, { "F2", 0xFFBF }, { "F3", 0xFFC0 }, { "F4", 0xFFC1 },
	{ "F5", 0xFFC2 }, { "F6", 0xFFC3 }, { "F7", 0xFFC4 }, { "F8", 0xFFC5 },
	{ "F9", 0xFFC6 }, { "F10", 0xFFC7 }, { "F11", 0xFFC8 },
	{ "F12", 0xFFC9 },
	{ "Shift_L", 0xFFE1 }, { "Shift_R", 0xFFE2 }, { "Control_L", 0xFFE3 },
	{ "Control_R", 0xFFE4 }, { "Caps_Lock", 0xFFE5 }, { "Meta_L", 0xFFE7 },
	{ "Meta_R", 0xFFE8 }, { "Alt_L", 0xFFE9 }, { "Alt_R", 0xFFEA },
	{ "Num_Lock", 0xFF7F },
	/* Punctuation, which a translation table names rather than quotes. */
	{ "exclam", 0x21 }, { "quotedbl", 0x22 }, { "numbersign", 0x23 },
	{ "dollar", 0x24 }, { "percent", 0x25 }, { "ampersand", 0x26 },
	{ "apostrophe", 0x27 }, { "parenleft", 0x28 }, { "parenright", 0x29 },
	{ "asterisk", 0x2A }, { "plus", 0x2B }, { "comma", 0x2C },
	{ "minus", 0x2D }, { "period", 0x2E }, { "slash", 0x2F },
	{ "colon", 0x3A }, { "semicolon", 0x3B }, { "less", 0x3C },
	{ "equal", 0x3D }, { "greater", 0x3E }, { "question", 0x3F },
	{ "at", 0x40 }, { "bracketleft", 0x5B }, { "backslash", 0x5C },
	{ "bracketright", 0x5D }, { "asciicircum", 0x5E }, { "underscore", 0x5F },
	{ "grave", 0x60 }, { "braceleft", 0x7B }, { "bar", 0x7C },
	{ "braceright", 0x7D }, { "asciitilde", 0x7E },
};

XLITE_IMPL(XStringToKeysym)
KeySym XStringToKeysym(const char *s)
{
	int i;

	if (!s || !*s)
		return NoSymbol;
	if (!s[1] && (unsigned char)s[0] >= 0x20 && (unsigned char)s[0] < 0x7F)
		return (unsigned char)s[0];	/* a single printable character */
	for (i = 0; i < (int)(sizeof(knames) / sizeof(knames[0])); i++)
		if (!strcmp(s, knames[i].name))
			return knames[i].ks;
	/* "U+ABCD" and "0x1234" forms, which some tables use. */
	if (s[0] == 'U' && s[1] == '+')
		return strtoul(s + 2, NULL, 16);
	if (s[0] == '0' && s[1] == 'x')
		return strtoul(s + 2, NULL, 16);
	xlite_note("unknown keysym name '%s'", s);
	return NoSymbol;
}

XLITE_IMPL(XKeysymToString)
char *XKeysymToString(KeySym ks)
{
	static char buf[16];
	int i;

	for (i = 0; i < (int)(sizeof(knames) / sizeof(knames[0])); i++)
		if (knames[i].ks == ks)
			return (char *)knames[i].name;
	if (ks >= 0x20 && ks < 0x7F) {
		buf[0] = (char)ks;
		buf[1] = 0;
		return buf;
	}
	return NULL;
}

/*
 * The server here has no keymap of its own: lvdesk delivers keycodes that are
 * already Latin-1 character codes, and XGetKeyboardMapping reports that
 * identity. So a keycode IS its keysym.
 */
XLITE_IMPL(XKeycodeToKeysym)
KeySym XKeycodeToKeysym(Display *dpy, KeyCode kc, int index)
{
	(void)dpy; (void)index;
	return xlw_widen(kc);
}

XLITE_IMPL(XLookupKeysym)
KeySym XLookupKeysym(XKeyEvent *ev, int index)
{
	(void)index;
	return ev ? ev->keycode : NoSymbol;
}

XLITE_IMPL(XLookupString)
int XLookupString(XKeyEvent *ev, char *buf, int n, KeySym *ks, XComposeStatus *st)
{
	KeySym k = ev ? xlw_widen(ev->keycode) : NoSymbol;

	(void)st;
	if (ks)
		*ks = k;
	if (!buf || n < 1)
		return 0;
	if (k >= 0x20 && k < 0x7F) {
		/* Control folds the letter into its C0 code, as X defines. */
		if ((ev->state & ControlMask) && (k | 0x20) >= 'a' &&
		    (k | 0x20) <= 'z')
			buf[0] = (char)((k | 0x20) - 'a' + 1);
		else
			buf[0] = (char)k;
		return 1;
	}
	if (k == 0xFF0D || k == 0xFF8D) { buf[0] = '\r'; return 1; }
	if (k == 0xFF09) { buf[0] = '\t'; return 1; }
	if (k == 0xFF08) { buf[0] = '\b'; return 1; }
	if (k == 0xFFFF) { buf[0] = 0x7F; return 1; }
	return 0;
}

XLITE_IMPL(XGetModifierMapping)
XModifierKeymap *XGetModifierMapping(Display *dpy)
{
	XModifierKeymap *m = calloc(1, sizeof(*m));

	(void)dpy;
	if (!m)
		return NULL;
	m->max_keypermod = 2;
	m->modifiermap = calloc(8 * m->max_keypermod, 1);
	return m;
}

XLITE_IMPL(XFreeModifiermap)
int XFreeModifiermap(XModifierKeymap *m)
{
	if (m) {
		free(m->modifiermap);
		free(m);
	}
	return 1;
}

/* ------------------------------------------------- window-manager hints */

XLITE_IMPL(XStringListToTextProperty)
Status XStringListToTextProperty(char **list, int count, XTextProperty *tp)
{
	int i, n = 0;
	char *p;

	for (i = 0; i < count; i++)
		n += strlen(list[i]) + 1;
	p = malloc(n ? n : 1);
	if (!p)
		return 0;
	tp->value = (unsigned char *)p;
	for (i = 0; i < count; i++) {
		size_t l = strlen(list[i]) + 1;

		memcpy(p, list[i], l);
		p += l;
	}
	tp->encoding = XA_STRING;
	tp->format = 8;
	tp->nitems = n;
	return 1;
}

XLITE_IMPL(XmbTextListToTextProperty)
int XmbTextListToTextProperty(Display *dpy, char **list, int count,
			      XICCEncodingStyle style, XTextProperty *tp)
{
	(void)dpy; (void)style;
	/*
	 * There is no locale support here (XSupportsLocale is False), so the
	 * multibyte form is the string form. Returning success matters: a
	 * client that cannot set its own name usually gives up on the rest of
	 * its window-manager properties too.
	 */
	return XStringListToTextProperty(list, count, tp) ? Success
							 : XNoMemory;
}

XLITE_IMPL(XSetTextProperty)
void XSetTextProperty(Display *dpy, Window w, XTextProperty *tp, Atom prop)
{
	if (tp && tp->value)
		XChangeProperty(dpy, w, prop, tp->encoding, tp->format,
				PropModeReplace, tp->value, tp->nitems);
}

XLITE_IMPL(XSetWMName)
void XSetWMName(Display *dpy, Window w, XTextProperty *tp)
{
	XSetTextProperty(dpy, w, tp, XA_WM_NAME);
}

XLITE_IMPL(XSetWMIconName)
void XSetWMIconName(Display *dpy, Window w, XTextProperty *tp)
{
	XSetTextProperty(dpy, w, tp, XA_WM_ICON_NAME);
}

XLITE_IMPL(XStoreName)
int XStoreName(Display *dpy, Window w, const char *name)
{
	return XChangeProperty(dpy, w, XA_WM_NAME, XA_STRING, 8,
			       PropModeReplace, (const unsigned char *)name,
			       strlen(name));
}

XLITE_IMPL(XSetIconName)
int XSetIconName(Display *dpy, Window w, const char *name)
{
	return XChangeProperty(dpy, w, XA_WM_ICON_NAME, XA_STRING, 8,
			       PropModeReplace, (const unsigned char *)name,
			       strlen(name));
}

XLITE_IMPL(XSetWMNormalHints)
void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *h)
{
	if (h)
		XChangeProperty(dpy, w, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS,
				32, PropModeReplace, (unsigned char *)h, 18);
}

XLITE_IMPL(XSetWMHints)
int XSetWMHints(Display *dpy, Window w, XWMHints *h)
{
	if (h)
		XChangeProperty(dpy, w, XA_WM_HINTS, XA_WM_HINTS, 32,
				PropModeReplace, (unsigned char *)h, 9);
	return 1;
}

XLITE_IMPL(XSetClassHint)
int XSetClassHint(Display *dpy, Window w, XClassHint *h)
{
	char buf[256];
	size_t n = 0;

	if (!h)
		return 0;
	n += snprintf(buf, sizeof(buf), "%s", h->res_name ? h->res_name : "");
	n++;
	if (n < sizeof(buf))
		n += snprintf(buf + n, sizeof(buf) - n, "%s",
			      h->res_class ? h->res_class : "") + 1;
	return XChangeProperty(dpy, w, XA_WM_CLASS, XA_STRING, 8,
			       PropModeReplace, (unsigned char *)buf, n);
}

XLITE_IMPL(XSetWMProperties)
void XSetWMProperties(Display *dpy, Window w, XTextProperty *name,
		      XTextProperty *icon, char **argv, int argc,
		      XSizeHints *sh, XWMHints *wh, XClassHint *ch)
{
	if (name)
		XSetWMName(dpy, w, name);
	if (icon)
		XSetWMIconName(dpy, w, icon);
	if (sh)
		XSetWMNormalHints(dpy, w, sh);
	if (wh)
		XSetWMHints(dpy, w, wh);
	if (ch)
		XSetClassHint(dpy, w, ch);
	(void)argv; (void)argc;
}

XLITE_IMPL(XSetTransientForHint)
int XSetTransientForHint(Display *dpy, Window w, Window prop)
{
	unsigned char b[4];

	b[0] = prop; b[1] = prop >> 8; b[2] = prop >> 16; b[3] = prop >> 24;
	return XChangeProperty(dpy, w, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32,
			       PropModeReplace, b, 1);
}

XLITE_IMPL(XSetWMProtocols)
Status XSetWMProtocols(Display *dpy, Window w, Atom *protocols, int count)
{
	Atom prop = XInternAtom(dpy, "WM_PROTOCOLS", False);

	return XChangeProperty(dpy, w, prop, XA_ATOM, 32, PropModeReplace,
			       (unsigned char *)protocols, count);
}

/*
 * Input methods. There is no XIM here - XSupportsLocale() is False - so no
 * event is ever consumed by one, and False is the answer that lets the
 * toolkit dispatch every event itself.
 */
XLITE_IMPL(XFilterEvent)
Bool XFilterEvent(XEvent *ev, Window w) { (void)ev; (void)w; return False; }

/*
 * Thread support. There is exactly one thread in every client here, and this
 * library holds no state that two of them could race on - so the locking is a
 * no-op and Success is the honest answer.
 *
 * Returning 0 is NOT the safe choice: a client that asks for threads and is
 * refused usually exits, which is precisely what xfiles did ("could not
 * initialize support for threads") before this existed.
 */
XLITE_IMPL(XInitThreads)
Status XInitThreads(void) { return 1; }

XLITE_IMPL(XLockDisplay)
void XLockDisplay(Display *dpy) { (void)dpy; }

XLITE_IMPL(XUnlockDisplay)
void XUnlockDisplay(Display *dpy) { (void)dpy; }

XLITE_IMPL(XParseGeometry)
int XParseGeometry(const char *spec, int *x, int *y, unsigned *w, unsigned *h)
{
	int flags = 0, v, sign;
	const char *p = spec;

	if (!p)
		return 0;
	if (*p != '+' && *p != '-' && *p != 'x' && *p != 'X') {
		*w = strtoul(p, (char **)&p, 10);
		flags |= 4;			/* WidthValue */
		if (*p == 'x' || *p == 'X') {
			p++;
			*h = strtoul(p, (char **)&p, 10);
			flags |= 8;		/* HeightValue */
		}
	}
	while (*p == '+' || *p == '-') {
		sign = (*p == '-') ? -1 : 1;
		p++;
		v = (int)strtol(p, (char **)&p, 10) * sign;
		if (!(flags & 1)) {
			*x = v;
			flags |= 1 | (sign < 0 ? 16 : 0);   /* XValue|XNegative */
		} else {
			*y = v;
			flags |= 2 | (sign < 0 ? 32 : 0);   /* YValue|YNegative */
		}
	}
	return flags;
}

/*
 * Visual lookup. There is one visual on this server - the panel's RGB565
 * TrueColor - so a request either matches it or does not.
 */
XLITE_IMPL(XMatchVisualInfo)
Status XMatchVisualInfo(Display *dpy, int screen, int depth, int class,
			XVisualInfo *vi)
{
	Visual *v = DefaultVisual(dpy, screen);

	if (!v || !vi)
		return 0;
	if (depth != DefaultDepth(dpy, screen) || class != v->class)
		return 0;
	memset(vi, 0, sizeof(*vi));
	vi->visual = v;
	vi->visualid = v->visualid;
	vi->screen = screen;
	vi->depth = depth;
	vi->class = v->class;
	vi->red_mask = v->red_mask;
	vi->green_mask = v->green_mask;
	vi->blue_mask = v->blue_mask;
	vi->colormap_size = v->map_entries;
	vi->bits_per_rgb = v->bits_per_rgb;
	return 1;
}

XLITE_IMPL(XFreeColormap)
int XFreeColormap(Display *dpy, Colormap c) { (void)dpy; (void)c; return 1; }

/*
 * XKB's keysym lookup, which xfiles uses for every keystroke. Same identity
 * as the rest of this file: the keycode in the event IS the keysym (wire
 * specials were already widened at decode). mods_rtrn reports which
 * modifiers the lookup consumed - the server folded Shift into the character
 * before sending, so Shift is consumed and the caller must not reapply it.
 */
XLITE_IMPL(XkbLookupKeySym)
Bool XkbLookupKeySym(Display *dpy, KeyCode key, unsigned int mods,
		     unsigned int *mods_rtrn, KeySym *sym_rtrn)
{
	(void)dpy;
	if (mods_rtrn)
		*mods_rtrn = mods & 1;	/* ShiftMask, already applied */
	if (sym_rtrn)
		*sym_rtrn = xlw_widen(key);
	return key != 0;
}
