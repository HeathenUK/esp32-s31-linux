/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XTLITE_H
#define XTLITE_H

/*
 * xtlite - the parts of the X Toolkit Intrinsics and the Athena widget set
 * that an application here actually uses.
 *
 * Audited on a running xcalc: it references NINETEEN functions from libXt,
 * FIVE symbols from libXaw and NOTHING from libXmu. Those three libraries cost
 * 720 kB resident - libXaw7 328, libXt 304, libXmu 88 - because the kernel's
 * fault-around maps ~64 kB around every fault and a widget set is touched all
 * over.
 *
 * Replacing libXaw as well as libXt is what makes this small: libXaw's own 113
 * references into libXt disappear with it, so the real surface is those 24
 * symbols and nothing else.
 *
 * The decisive freedom is that `Widget` and `commandWidgetClass` are OPAQUE to
 * the application. xcalc receives them from us and hands them straight back,
 * so we owe no one a struct layout - unlike xlite, where Xlib.h publishes the
 * Display record. That means this is not a reimplementation of Xt's
 * architecture; it is an implementation of the behaviour xcalc depends on.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XTLITE_IMPL(name)	/* implemented: name */

/*
 * What a widget can be. The first five are ours, and opaque to the
 * application. W_CUSTOM is a class the APPLICATION defined - see xtclass.c -
 * where the instance layout is the application's and not ours.
 */
enum wclass { W_SHELL, W_FORM, W_LABEL, W_COMMAND, W_TOGGLE, W_CUSTOM };

#define MAXACT	64

struct wid;

struct action {
	const char *name;
	void (*proc)(Widget, XEvent *, char **, unsigned *);
};

/* One parsed translation: an event pattern and the actions it fires. */
struct trans {
	int type;			/* ButtonPress, KeyPress, ... */
	unsigned detail;		/* button number or keysym */
	unsigned mods;			/* required modifier mask */
	char *actions;			/* "digit(7)" etc., verbatim */
};

/*
 * The widget record the application sees sits IMMEDIATELY AFTER our own, in
 * one allocation:
 *
 *     [ struct wid | CorePart ... SimplePart ... ClockPart ]
 *                  ^
 *                  the Widget the application is handed
 *
 * An application-defined widget lays out its own instance record, starting
 * with CorePart at offset zero, and reads w->core.width directly - so our
 * bookkeeping cannot live inside it. Putting it in front makes the conversion
 * a constant offset in both directions, with no side table and no lookup on
 * the event path.
 */
#define WIDGET(p)	((Widget)((char *)(p) + sizeof(struct wid)))
#define WID(w)		((struct wid *)((char *)(w) - sizeof(struct wid)))

struct wid {
	enum wclass cls;
	WidgetClass wclass;		/* the application's class, or NULL */
	char name[32];
	struct wid *parent;
	/*
	 * Grown on demand, not reserved. A fixed kids[80] and trans[96] cost
	 * ~10 kB per widget, and xcalc builds sixty of them - 620 kB of dirty
	 * anonymous memory, which is the single worst kind on this board. The
	 * same mistake as the resource database, made twice.
	 */
	struct wid **kids;
	int nkids, kidcap;

	Window win;
	int x, y, w, h;
	/*
	 * Layout-basis geometry: what layout() computed before any resize.
	 * A shell resize scales every descendant FROM THIS, not from the
	 * current geometry, so repeated resizes cannot accumulate rounding
	 * drift and shrinking back restores the original layout exactly.
	 */
	int lx, ly, lw, lh;
	int pref_w, pref_h;		/* width/height resources, 0 = from label */
	int justify;			/* 0 left, 1 centre, 2 right - Xaw's values */
	XFontStruct *fnt;		/* per-widget font, NULL = the default */
	int bw;
	unsigned long fg, bg, border;
	char label[64];
	int managed, realized, mapped;

	/* Command/Toggle state */
	int set, highlighted;
	void (*callback)(Widget, void *, void *);
	void *closure;
	char radio_group[32];

	/* Form constraints, straight out of the resource file */
	struct wid *from_horiz, *from_vert;
	int horiz_dist, vert_dist;

	struct trans *trans;
	int ntrans, transcap;
};

/* Globals live in xtlite.c. */
extern Display *xt_dpy;
extern struct wid *xt_root;

struct wid *xt_wid_new(const char *name, enum wclass cls, struct wid *parent,
		       size_t recsize);
const char *xt_res_lookup(Widget w, const char *name, const char *class);
void xt_set_typed(void *slot, const char *type, unsigned size, long value);
void xt_set_from_string(Widget w, void *slot, const char *type, unsigned size,
			const char *v);
/* xtclass.c: run an application-registered type converter. 1 if it worked. */
int xt_convert(Widget w, const char *type, const char *v, void *slot,
	       unsigned size);

/* xtclass.c - the Intrinsics class mechanism. */
Widget xt_custom_create(const char *name, WidgetClass wc, struct wid *parent,
			ArgList args, Cardinal nargs);
void xt_custom_expose(struct wid *p, XEvent *ev);
void xt_custom_resized(struct wid *p);
int xt_is_intrinsics_class(WidgetClass wc);
int xt_is_builtin_class(WidgetClass c);
const char *xt_class_name(WidgetClass wc);
size_t xt_class_size(WidgetClass wc);
int xt_timer_wait_ms(void);
void xt_timer_fire_due(void);
unsigned long xt_now_ms(void);

void xt_note(const char *fmt, ...);
void xt_missing(const char *name);
void xt_ignored(const char *kind, const char *name);
int xt_tracing(void);

#endif /* XTLITE_H */
