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
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XTLITE_IMPL(name)	/* implemented: name */

/* What a widget can be. Only the classes xcalc instantiates exist. */
enum wclass { W_SHELL, W_FORM, W_LABEL, W_COMMAND, W_TOGGLE };

#define MAXACT	64

struct wid;

struct action {
	const char *name;
	void (*proc)(struct wid *, XEvent *, char **, unsigned *);
};

/* One parsed translation: an event pattern and the actions it fires. */
struct trans {
	int type;			/* ButtonPress, KeyPress, ... */
	unsigned detail;		/* button number or keysym */
	unsigned mods;			/* required modifier mask */
	char *actions;			/* "digit(7)" etc., verbatim */
};

struct wid {
	enum wclass cls;
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
	int pref_w, pref_h;		/* width/height resources, 0 = from label */
	int justify;			/* 0 left, 1 centre, 2 right - Xaw's values */
	int bw;
	unsigned long fg, bg, border;
	char label[64];
	int managed, realized, mapped;

	/* Command/Toggle state */
	int set, highlighted;
	void (*callback)(struct wid *, void *, void *);
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

void xt_note(const char *fmt, ...);
void xt_missing(const char *name);
void xt_ignored(const char *kind, const char *name);
int xt_tracing(void);

#endif /* XTLITE_H */
