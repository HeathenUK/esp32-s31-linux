/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The Intrinsics class mechanism, for applications that define their own
 * widget.
 *
 * xtlite's original bet was that a widget class is an OPAQUE token: xcalc
 * receives commandWidgetClass from us and hands it straight back, so what it
 * points at is nobody's business but ours. That is true of every application
 * built entirely out of Athena widgets, and it is what makes xtlite 24 kB
 * instead of 304.
 *
 * It is not true of xclock. xclock declares a ClockClassRec of its own,
 * statically initialised with `(WidgetClass) &simpleClassRec` as its
 * superclass and XtInherit* sentinels for the methods it does not override,
 * and it lays out a ClockRec as { CorePart core; SimplePart simple;
 * ClockPart clock; }. Its Redisplay proc reads w->core.width directly. So for
 * this class of application the layout is not ours to choose - the compiler
 * has already chosen it, out of the headers, at the application's build time.
 *
 * Hence this file: real class records with the real layouts (taken from the
 * system's CoreP.h and SimpleP.h, never transcribed), and the small part of
 * Xt's class protocol that an application-defined widget actually depends on:
 *
 *   - superclass chaining, so ClockClassRec -> simpleClassRec -> widgetClassRec
 *   - _XtInherit resolution, so XtInheritRealize means the superclass's realize
 *   - class_initialize / class_part_initialize, run once, superclass first
 *   - the resource list of every class in the chain, applied to the instance
 *   - initialize, realize, expose, resize and set_values, called on ours
 *
 * The instance record is the application's, so xtlite's own bookkeeping can no
 * longer live inside it. It lives immediately BEFORE it, in one allocation -
 * see WIDGET()/WID() in xtlite.h. That is a constant offset rather than a
 * lookup, so nothing in the event path pays for it.
 */
#include "xtlite.h"

#include <X11/IntrinsicP.h>
#include <X11/CoreP.h>
#include <X11/Xaw/SimpleP.h>
#include <X11/ShellP.h>

#include <sys/time.h>
#include <poll.h>

/*
 * The inheritance sentinel. Xt never calls it; a class record stores it in a
 * method slot to mean "whatever my superclass does", and class_init() below
 * replaces it with the superclass's pointer. It has to exist as a real symbol
 * because the application's static initialisers take its address.
 */
void _XtInherit(void)
{
	xt_missing("_XtInherit called");
}

/*
 * Core and Simple, as real records. They are zeroed .bss filled in by a
 * constructor rather than written out as struct literals: the literal would be
 * thirty lines of NULLs whose correctness depends on field ORDER, and the
 * field order belongs to the header, not to us.
 */
WidgetClassRec widgetClassRec;
SimpleClassRec simpleClassRec;

WidgetClass widgetClass = (WidgetClass)&widgetClassRec;
WidgetClass coreWidgetClass = (WidgetClass)&widgetClassRec;
WidgetClass simpleWidgetClass = (WidgetClass)&simpleClassRec;

/*
 * The shell classes. xtlite has exactly one kind of top-level window, so these
 * exist to be NAMED - an application's XtOpenApplication() call takes the
 * address of one, and the loader will not start a binary with an unresolved
 * reference no matter how little the value is used.
 */
SessionShellClassRec sessionShellClassRec;
WidgetClass sessionShellWidgetClass = (WidgetClass)&sessionShellClassRec;
WidgetClass applicationShellWidgetClass = (WidgetClass)&sessionShellClassRec;
WidgetClass topLevelShellWidgetClass = (WidgetClass)&sessionShellClassRec;

/*
 * One libXmu converter, because xclock registers it by address. Converters are
 * never consulted here - see XtSetTypeConverter below - so this is reached
 * only if that ever changes, and it says so if it is.
 */
void XmuCvtStringToBackingStore(XrmValuePtr args, Cardinal *num_args,
				XrmValuePtr from, XrmValuePtr to)
{
	(void)args; (void)num_args; (void)from; (void)to;
	xt_missing("XmuCvtStringToBackingStore");
}

/*
 * Core's own resources.
 *
 * Without these, CorePart is whatever calloc() left behind, and the field that
 * bites first is background_pixel: xclock erases the previous second hand by
 * redrawing it in the background colour, so a zeroed background_pixel painted
 * every old hand in BLACK and the face filled up with a fan of them. A widget
 * that draws itself reads these fields; they are not decoration.
 */
static XtResource core_resources[] = {
	{ XtNx, XtCPosition, XtRPosition, sizeof(Position),
	  XtOffsetOf(WidgetRec, core.x), XtRImmediate, (XtPointer)0 },
	{ XtNy, XtCPosition, XtRPosition, sizeof(Position),
	  XtOffsetOf(WidgetRec, core.y), XtRImmediate, (XtPointer)0 },
	{ XtNwidth, XtCWidth, XtRDimension, sizeof(Dimension),
	  XtOffsetOf(WidgetRec, core.width), XtRImmediate, (XtPointer)0 },
	{ XtNheight, XtCHeight, XtRDimension, sizeof(Dimension),
	  XtOffsetOf(WidgetRec, core.height), XtRImmediate, (XtPointer)0 },
	{ XtNborderWidth, XtCBorderWidth, XtRDimension, sizeof(Dimension),
	  XtOffsetOf(WidgetRec, core.border_width), XtRImmediate,
	  (XtPointer)1 },
	{ XtNborderColor, XtCBorderColor, XtRPixel, sizeof(Pixel),
	  XtOffsetOf(WidgetRec, core.border_pixel), XtRString, "black" },
	{ XtNbackground, XtCBackground, XtRPixel, sizeof(Pixel),
	  XtOffsetOf(WidgetRec, core.background_pixel), XtRString, "white" },
	{ XtNsensitive, XtCSensitive, XtRBoolean, sizeof(Boolean),
	  XtOffsetOf(WidgetRec, core.sensitive), XtRImmediate,
	  (XtPointer)True },
	{ XtNmappedWhenManaged, XtCMappedWhenManaged, XtRBoolean,
	  sizeof(Boolean), XtOffsetOf(WidgetRec, core.mapped_when_managed),
	  XtRImmediate, (XtPointer)True },
};

static void core_realize(Widget w, XtValueMask *mask,
			 XSetWindowAttributes *attr)
{
	(void)w; (void)mask; (void)attr;
	/*
	 * A marker, not an implementation. xtlite's realize() creates every
	 * window itself; this exists so that a class inheriting realize can be
	 * told apart from one that overrides it.
	 */
}

__attribute__((constructor))
static void class_records_init(void)
{
	widgetClassRec.core_class.superclass = NULL;
	widgetClassRec.core_class.class_name = "Core";
	widgetClassRec.core_class.widget_size = sizeof(WidgetRec);
	widgetClassRec.core_class.realize = core_realize;
	widgetClassRec.core_class.resources = core_resources;
	widgetClassRec.core_class.num_resources =
		sizeof(core_resources) / sizeof(core_resources[0]);
	widgetClassRec.core_class.class_inited = 1;

	simpleClassRec.core_class.superclass = (WidgetClass)&widgetClassRec;
	simpleClassRec.core_class.class_name = "Simple";
	simpleClassRec.core_class.widget_size = sizeof(SimpleRec);
	simpleClassRec.core_class.realize = core_realize;
	simpleClassRec.core_class.class_inited = 1;

	sessionShellClassRec.core_class.superclass =
		(WidgetClass)&widgetClassRec;
	sessionShellClassRec.core_class.class_name = "SessionShell";
	sessionShellClassRec.core_class.widget_size =
		sizeof(SessionShellRec);
	sessionShellClassRec.core_class.realize = core_realize;
	sessionShellClassRec.core_class.class_inited = 1;
}

int xt_is_intrinsics_class(WidgetClass wc)
{
	return wc == (WidgetClass)&widgetClassRec ||
	       wc == (WidgetClass)&simpleClassRec;
}

/* ------------------------------------------------------ class initialisation */

/* Replace one _XtInherit slot with the superclass's value. */
#define INHERIT(field)							\
	do {								\
		if ((void *)wc->core_class.field == (void *)_XtInherit)	\
			wc->core_class.field = sc->core_class.field;	\
	} while (0)

static void class_init(WidgetClass wc)
{
	WidgetClass sc;

	if (!wc || wc->core_class.class_inited)
		return;
	sc = wc->core_class.superclass;
	class_init(sc);

	if (wc->core_class.class_initialize)
		wc->core_class.class_initialize();

	if (sc) {
		INHERIT(realize);
		INHERIT(resize);
		INHERIT(expose);
		INHERIT(set_values_almost);
		INHERIT(accept_focus);
		INHERIT(query_geometry);
		INHERIT(display_accelerator);
		if ((void *)wc->core_class.tm_table == (void *)_XtInherit)
			wc->core_class.tm_table = sc->core_class.tm_table;
	}
	if (wc->core_class.class_part_initialize)
		wc->core_class.class_part_initialize(wc);
	wc->core_class.class_inited = 1;
	xt_note("class %s initialised, %u byte records",
		wc->core_class.class_name,
		(unsigned)wc->core_class.widget_size);
}

const char *xt_class_name(WidgetClass wc)
{
	return wc ? wc->core_class.class_name : "?";
}

size_t xt_class_size(WidgetClass wc)
{
	return wc ? wc->core_class.widget_size : sizeof(CorePart);
}

/* ------------------------------------------------------------- resources */

/*
 * Apply one class's resource list to an instance record, superclass first so
 * a subclass's entry for the same field wins.
 *
 * A resource the database does not set falls back to the class's default,
 * which is where XtRImmediate matters: default_addr is then the VALUE, not a
 * pointer to it. Getting that backwards writes a pointer into an int field and
 * the widget comes up with, for instance, an update interval of 0x4a3c0 -
 * which looks like a hang and is a misread default.
 */
static void apply_resources(Widget w, WidgetClass wc, ArgList args,
			    Cardinal nargs)
{
	Cardinal i, a;

	if (!wc)
		return;
	apply_resources(w, wc->core_class.superclass, args, nargs);

	for (i = 0; i < wc->core_class.num_resources; i++) {
		XtResource *r = &wc->core_class.resources[i];
		char *slot = (char *)w + r->resource_offset;
		const char *v = NULL;
		int from_args = 0;

		for (a = 0; a < nargs; a++)
			if (!strcmp(args[a].name, r->resource_name)) {
				xt_set_typed(slot, r->resource_type,
					     r->resource_size,
					     (long)args[a].value);
				from_args = 1;
				break;
			}
		if (from_args) {
			xt_note("  res %-18s from Arg", r->resource_name);
			continue;
		}

		v = xt_res_lookup(w, r->resource_name, r->resource_class);
		xt_note("  res %-18s %-12s size %u off %-4u = %s "
			"[default %s \"%s\"]",
			r->resource_name, r->resource_type,
			(unsigned)r->resource_size,
			(unsigned)r->resource_offset,
			v ? v : "(default)",
			r->default_type ? r->default_type : "-",
			r->default_type && !strcmp(r->default_type, XtRString)
			&& r->default_addr ? (const char *)r->default_addr
			: "");
		if (v) {
			xt_set_from_string(w, slot, r->resource_type,
					   r->resource_size, v);
			continue;
		}
		if (!r->default_addr)
			continue;
		if (r->default_type && !strcmp(r->default_type, XtRImmediate))
			xt_set_typed(slot, r->resource_type, r->resource_size,
				     (long)r->default_addr);
		else if (r->default_type && r->resource_type &&
			 !strcmp(r->default_type, r->resource_type))
			memcpy(slot, r->default_addr, r->resource_size);
		else if (r->default_type &&
			 !strcmp(r->default_type, XtRString))
			xt_set_from_string(w, slot, r->resource_type,
					   r->resource_size,
					   (const char *)r->default_addr);
		else
			xt_set_typed(slot, r->resource_type, r->resource_size,
				     (long)r->default_addr);
	}
}

/* ------------------------------------------------------------- instances */

/* Run the initialize procs of the chain, superclass first, as Xt does. */
static void run_initialize(Widget w, WidgetClass wc, ArgList args,
			   Cardinal *nargs)
{
	if (!wc)
		return;
	run_initialize(w, wc->core_class.superclass, args, nargs);
	if (wc->core_class.initialize)
		wc->core_class.initialize(w, w, args, nargs);
}

Widget xt_custom_create(const char *name, WidgetClass wc, struct wid *parent,
			ArgList args, Cardinal nargs)
{
	struct wid *p;
	Widget w;
	Cardinal n = nargs;

	class_init(wc);
	p = xt_wid_new(name, W_CUSTOM, parent, xt_class_size(wc));
	if (!p)
		return NULL;
	p->wclass = wc;
	w = WIDGET(p);

	w->core.self = w;
	w->core.widget_class = wc;
	w->core.parent = parent ? WIDGET(parent) : NULL;
	w->core.name = p->name;
	w->core.screen = DefaultScreenOfDisplay(xt_dpy);
	w->core.colormap = DefaultColormap(xt_dpy, 0);
	w->core.depth = DefaultDepth(xt_dpy, 0);
	w->core.window = None;
	w->core.sensitive = True;
	w->core.ancestor_sensitive = True;
	w->core.mapped_when_managed = True;

	/*
	 * A custom widget filling a shell is the whole window, not a Form
	 * child inset by the default distance.
	 */
	if (parent && parent->cls == W_SHELL)
		p->horiz_dist = p->vert_dist = 0;

	apply_resources(w, wc, args, nargs);
	run_initialize(w, wc, args, &n);

	/*
	 * The class's initialize proc is what decides a custom widget's size,
	 * so xtlite's own geometry has to be taken from the record AFTER it
	 * runs, not before.
	 */
	p->w = w->core.width;
	p->h = w->core.height;
	/*
	 * pref_w/pref_h, not just w/h: layout() recomputes every child's size
	 * from its label unless an explicit one is set, and a custom widget
	 * has no label - so without this the 164x164 clock was laid out as an
	 * 8x17 box and the shell came up 12x21.
	 */
	p->pref_w = w->core.width;
	p->pref_h = w->core.height;
	p->x = w->core.x;
	p->y = w->core.y;
	p->bw = w->core.border_width;
	p->bg = w->core.background_pixel;
	p->border = w->core.border_pixel;
	xt_note("custom widget %s (%s) %dx%d", p->name,
		xt_class_name(wc), p->w, p->h);
	return w;
}

/* Push xtlite's geometry into the record and tell the widget it changed. */
void xt_custom_resized(struct wid *p)
{
	Widget w = WIDGET(p);

	if (!p->wclass)
		return;
	w->core.x = p->x;
	w->core.y = p->y;
	w->core.width = p->w;
	w->core.height = p->h;
	w->core.window = p->win;
	if (p->wclass->core_class.resize)
		p->wclass->core_class.resize(w);
}

void xt_custom_expose(struct wid *p, XEvent *ev)
{
	Widget w = WIDGET(p);
	XEvent synth;
	Region reg = NULL;

	if (!p->wclass || !p->wclass->core_class.expose)
		return;
	/*
	 * A redraw we initiate has no event, and an expose proc is entitled to
	 * read one - xclock's tests event->type. Synthesise a full-window
	 * Expose rather than passing NULL.
	 */
	if (!ev) {
		memset(&synth, 0, sizeof(synth));
		synth.type = Expose;
		synth.xexpose.window = p->win;
		synth.xexpose.width = p->w;
		synth.xexpose.height = p->h;
		ev = &synth;
	}
	/*
	 * Build the Region the expose proc is entitled to.
	 *
	 * Xt's XtExposeProc takes (widget, event, REGION), and passing NULL
	 * is not "the whole widget" - a widget that clips its drawing to the
	 * region simply draws nothing. xclock sets its RENDER clip from it, so
	 * with NULL its full-face repaint never happened and only the
	 * timer-driven hand updates ever painted: the dial appeared to sweep
	 * into existence behind the hands, one clipped region at a time.
	 */
	{
		XRectangle rc;

		rc.x = (short)ev->xexpose.x;
		rc.y = (short)ev->xexpose.y;
		rc.width = (unsigned short)(ev->xexpose.width ?
					    ev->xexpose.width : p->w);
		rc.height = (unsigned short)(ev->xexpose.height ?
					     ev->xexpose.height : p->h);
		reg = XCreateRegion();
		if (reg)
			XUnionRectWithRegion(&rc, reg, reg);
	}
	w->core.window = p->win;
	w->core.width = p->w;
	w->core.height = p->h;
	w->core.visible = True;
	p->wclass->core_class.expose(w, ev, reg);
	if (reg)
		XDestroyRegion(reg);
}

/* ------------------------------------------------------------ the GC cache */

/*
 * XtGetGC hands out SHARED GCs, and that sharing is the point: a widget set
 * asks for the same foreground/font combination over and over. xclock alone
 * asks four times. Without the cache each is a server resource and a round
 * trip; with it there is one of each.
 */
#define NGC	16

static struct gcent {
	XtGCMask mask;
	XGCValues v;
	GC gc;
	int refs;
} gcs[NGC];
static int ngcs;

XTLITE_IMPL(XtGetGC)
GC XtGetGC(Widget w, XtGCMask mask, XGCValues *values)
{
	int i;
	GC gc;

	for (i = 0; i < ngcs; i++)
		if (gcs[i].mask == mask &&
		    !memcmp(&gcs[i].v, values, sizeof(*values))) {
			gcs[i].refs++;
			return gcs[i].gc;
		}
	gc = XCreateGC(xt_dpy, DefaultRootWindow(xt_dpy), mask, values);
	if (ngcs < NGC) {
		gcs[ngcs].mask = mask;
		gcs[ngcs].v = *values;
		gcs[ngcs].gc = gc;
		gcs[ngcs].refs = 1;
		ngcs++;
	} else {
		xt_note("GC cache full (%d), %p is not shared", NGC,
			(void *)gc);
	}
	(void)w;
	return gc;
}

XTLITE_IMPL(XtReleaseGC)
void XtReleaseGC(Widget w, GC gc)
{
	int i;

	(void)w;
	for (i = 0; i < ngcs; i++)
		if (gcs[i].gc == gc) {
			if (--gcs[i].refs > 0)
				return;
			break;
		}
	/*
	 * Deliberately not freed. A released GC is very often re-acquired a
	 * moment later with the same values, and a client's GC count here is
	 * in single figures, so keeping it costs less than the round trip to
	 * destroy and recreate it.
	 */
}

/* --------------------------------------------------------------- timeouts */

/*
 * xclock does not poll; it asks for one timeout, redraws, and asks for the
 * next. So a client with no input pending must block in poll() with a
 * deadline rather than in read() - which is why the main loop needs these and
 * not merely a list.
 */
#define NTIMER	16

static struct timer {
	int used;
	unsigned long due_ms;
	XtTimerCallbackProc proc;
	XtPointer data;
	XtIntervalId id;
} timers[NTIMER];
static XtIntervalId next_timer_id = 1;

unsigned long xt_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (unsigned long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

XTLITE_IMPL(XtAppAddTimeOut)
XtIntervalId XtAppAddTimeOut(XtAppContext app, unsigned long interval,
			     XtTimerCallbackProc proc, XtPointer data)
{
	int i;

	(void)app;
	if ((long)interval < 0) {
		xt_note("timeout of %lu ms is a wrapped negative delay, "
			"clamped to 0", interval);
		interval = 0;
	}
	for (i = 0; i < NTIMER; i++)
		if (!timers[i].used) {
			timers[i].used = 1;
			timers[i].due_ms = xt_now_ms() + interval;
			timers[i].proc = proc;
			timers[i].data = data;
			timers[i].id = next_timer_id++;
			xt_note("timeout %lu in %lu ms",
				(unsigned long)timers[i].id, interval);
			return timers[i].id;
		}
	xt_note("no free timer slot for a %lu ms timeout", interval);
	return 0;
}

XTLITE_IMPL(XtRemoveTimeOut)
void XtRemoveTimeOut(XtIntervalId id)
{
	int i;

	for (i = 0; i < NTIMER; i++)
		if (timers[i].used && timers[i].id == id)
			timers[i].used = 0;
}

/*
 * Milliseconds until the next timeout, or -1 if there is none - the value
 * poll() wants. A timer already due returns 0 rather than a negative number,
 * which would mean "block for ever" and stop the clock.
 */
int xt_timer_wait_ms(void)
{
	unsigned long now = xt_now_ms();
	long best = -1;
	int i;

	xt_note("timer_wait: now=%lu", now);

	for (i = 0; i < NTIMER; i++) {
		long d;

		if (!timers[i].used)
			continue;
		d = (long)(timers[i].due_ms - now);
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}
	return (int)best;
}

void xt_timer_fire_due(void)
{
	unsigned long now = xt_now_ms();
	int i;

	for (i = 0; i < NTIMER; i++)
		if (timers[i].used && (long)(timers[i].due_ms - now) <= 0) {
			XtTimerCallbackProc proc = timers[i].proc;
			XtPointer data = timers[i].data;
			XtIntervalId id = timers[i].id;

			timers[i].used = 0;	/* before, so it may re-add */
			xt_note("timeout %lu fires", (unsigned long)id);
			proc(data, &id);
		}
}

/* ------------------------------------------------------------- odds and ends */

XTLITE_IMPL(XawInitializeWidgetSet)
void XawInitializeWidgetSet(void) { }

XTLITE_IMPL(XtDisplayOfObject)
Display *XtDisplayOfObject(Widget w) { (void)w; return xt_dpy; }

XTLITE_IMPL(XtWindowOfObject)
Window XtWindowOfObject(Widget w) { return w ? WID(w)->win : None; }

XTLITE_IMPL(XtAppErrorMsg)
void XtAppErrorMsg(XtAppContext app, const char *name, const char *type,
		   const char *cls, const char *def, String *params,
		   Cardinal *num)
{
	(void)app; (void)type; (void)cls; (void)params; (void)num;
	fprintf(stderr, "xtlite: fatal Xt error %s: %s\n", name,
		def ? def : "");
	exit(1);
}

XTLITE_IMPL(XtDisplayStringConversionWarning)
void XtDisplayStringConversionWarning(Display *dpy, const char *from,
				      const char *to)
{
	(void)dpy;
	fprintf(stderr, "xtlite: cannot convert \"%s\" to %s\n", from, to);
}

/* --------------------------------------------------------- type converters */

/*
 * Applications register converters for the resource types xtlite has never
 * heard of, and then depend on them having RUN. xclock's `face` is an
 * XftFont* and its colours are XftColors; noting the registration and
 * carrying on left the font pointer NULL, and `xclock -digital`
 * dereferenced it before drawing a single character.
 *
 * So they are stored and called. The awkward part is XtConvertArgList: a
 * converter for a font or a colour needs the screen and the colormap, and
 * asks for them as offsets into the widget record - which is exactly why the
 * class mechanism had to come first.
 */
#define NCONV	16

static struct conv {
	const char *from, *to;
	XtTypeConverter newp;
	XtConverter oldp;
	XtConvertArgList args;
	Cardinal nargs;
} convs[NCONV];
static int nconv;

static void conv_add(const char *from, const char *to, XtTypeConverter newp,
		     XtConverter oldp, XtConvertArgList args, Cardinal n)
{
	int i;

	for (i = 0; i < nconv; i++)
		if (!strcmp(convs[i].from, from) && !strcmp(convs[i].to, to))
			break;
	if (i == NCONV) {
		xt_note("converter table full, %s -> %s dropped", from, to);
		return;
	}
	if (i == nconv)
		nconv++;
	convs[i].from = from;
	convs[i].to = to;
	convs[i].newp = newp;
	convs[i].oldp = oldp;
	convs[i].args = args;
	convs[i].nargs = n;
	xt_note("converter %s -> %s registered, %u args", from, to,
		(unsigned)n);
}

/* Build the XrmValue list a converter's XtConvertArgList asks for. */
static Cardinal eval_args(Widget w, struct conv *c, XrmValue *out, int max)
{
	Cardinal i, n = 0;

	for (i = 0; i < c->nargs && (int)n < max; i++) {
		XtConvertArgRec *a = &c->args[i];

		out[n].size = a->size;
		switch (a->address_mode) {
		case XtAddress:
			out[n].addr = (XPointer)a->address_id;
			break;
		case XtImmediate:
			out[n].addr = (XPointer)&a->address_id;
			break;
		case XtBaseOffset:
		case XtWidgetBaseOffset:
			/*
			 * Every widget here is a real widget, so there is no
			 * object-to-parent walk to do: the offset applies
			 * straight to the record.
			 */
			out[n].addr = (XPointer)((char *)w +
						 (long)a->address_id);
			break;
		default:
			xt_ignored("converter argument mode", c->to);
			return 0;
		}
		n++;
	}
	return n;
}

int xt_convert(Widget w, const char *type, const char *v, void *slot,
	       unsigned size)
{
	XrmValue args[8], from, to;
	Cardinal n;
	int i;

	for (i = 0; i < nconv; i++) {
		struct conv *c = &convs[i];
		XtPointer cdata = NULL;

		if (strcmp(c->to, type) || strcmp(c->from, XtRString))
			continue;
		n = eval_args(w, c, args, 8);
		from.size = (unsigned)strlen(v) + 1;
		from.addr = (XPointer)v;
		to.size = size;
		to.addr = (XPointer)slot;
		if (c->newp) {
			if (!c->newp(xt_dpy, args, &n, &from, &to, &cdata))
				return 0;
		} else if (c->oldp) {
			to.addr = NULL;		/* old style allocates */
			c->oldp(args, &n, &from, &to);
			if (!to.addr)
				return 0;
		} else {
			return 0;
		}
		/*
		 * An old-style converter, and some new ones, answer by
		 * pointing to their own storage instead of writing where they
		 * were told. Copying is the difference between a converted
		 * value and an untouched slot.
		 */
		if (to.addr && to.addr != (XPointer)slot)
			memcpy(slot, to.addr, to.size < size ? to.size : size);
		xt_note("converted \"%s\" to %s", v, type);
		return 1;
	}
	return 0;
}

XTLITE_IMPL(XtSetTypeConverter)
void XtSetTypeConverter(const char *from, const char *to,
			XtTypeConverter conv, XtConvertArgList args,
			Cardinal n, XtCacheType cache, XtDestructor destroy)
{
	(void)cache; (void)destroy;
	conv_add(from, to, conv, NULL, args, n);
}

XTLITE_IMPL(XtAppSetTypeConverter)
void XtAppSetTypeConverter(XtAppContext app, const char *from, const char *to,
			   XtTypeConverter conv, XtConvertArgList args,
			   Cardinal n, XtCacheType cache, XtDestructor destroy)
{
	(void)app; (void)cache; (void)destroy;
	conv_add(from, to, conv, NULL, args, n);
}

XTLITE_IMPL(XtAddConverter)
void XtAddConverter(const char *from, const char *to, XtConverter conv,
		    XtConvertArgList args, Cardinal n)
{
	conv_add(from, to, NULL, conv, args, n);
}

XTLITE_IMPL(XtAppAddConverter)
void XtAppAddConverter(XtAppContext app, const char *from, const char *to,
		       XtConverter conv, XtConvertArgList args, Cardinal n)
{
	(void)app;
	conv_add(from, to, NULL, conv, args, n);
}
