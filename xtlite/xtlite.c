/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xtlite: the Intrinsics and Athena behaviour xcalc actually uses.
 *
 * See xtlite.h for why this is only 24 symbols wide. The short version: the
 * application treats Widget and the widget classes as opaque tokens, so this
 * implements what the application OBSERVES - a laid-out tree of labelled
 * boxes that fire named actions when clicked - rather than Xt's class system.
 */
#include "xtlite.h"

#include <stdarg.h>
#include <ctype.h>

Display *xt_dpy;
struct wid *xt_root;

static XrmDatabase xt_db;
static char app_name[32] = "xcalc";
static char app_class[32] = "XCalc";
static struct action actions[MAXACT];
static int nactions;
static GC gc_fg, gc_inv;
static XFontStruct *font;
static int running = 1;

/* ------------------------------------------------------------ diagnostics */

int xt_tracing(void)
{
	static int v = -1;

	if (v < 0)
		v = getenv("XTLITE_TRACE") != NULL;
	return v;
}

void xt_note(const char *fmt, ...)
{
	va_list ap;

	if (!xt_tracing())
		return;
	fputs("xtlite: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Anything we are handed and do not act on gets said OUT LOUD, once, with a
 * summary at exit - never behind a trace flag.
 *
 * This exists because xcalc passes justify=2 (XtJustifyRight) to its display
 * labels and the first version of apply_arg() dropped it with a quiet
 * xt_note(). The result was a centred value where the application had
 * explicitly asked for a right-aligned one: the library silently overriding
 * the program. An off-the-shelf application only stays off-the-shelf if every
 * instruction it gives is either obeyed or announced.
 */
void xt_ignored(const char *kind, const char *name)
{
	static const char *seen[64];
	static int n, registered;
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(seen[i], name))
			return;
	if (n < 64)
		seen[n++] = name;
	fprintf(stderr, "xtlite: IGNORING %s '%s' - the application asked for "
		"something we do not implement\n", kind, name);
	(void)registered;
}

void xt_missing(const char *name)
{
	static const char *seen[32];
	static int n;
	int i;

	for (i = 0; i < n; i++)
		if (seen[i] == name)
			return;
	if (n < 32)
		seen[n++] = name;
	fprintf(stderr, "xtlite: UNIMPLEMENTED %s() - carrying on\n", name);
}

/* --------------------------------------------------------------- resources */

/*
 * A widget's resource is looked up by its full instance path and its class
 * path, exactly as Xrm defines - "xcalc.bevel.screen.LCD" against
 * "XCalc.Form.Form.Label". Building both walks up the parent chain.
 */
static const char *class_name(struct wid *w)
{
	switch (w->cls) {
	case W_SHELL:   return "XCalc";
	case W_FORM:    return "Form";
	case W_LABEL:   return "Label";
	case W_COMMAND: return "Command";
	case W_TOGGLE:  return "Toggle";
	case W_CUSTOM:  return xt_class_name(w->wclass);
	}
	return "?";
}

static void path_of(struct wid *w, const char *leaf, char *inst, char *cls,
		    size_t n)
{
	struct wid *chain[16];
	int d = 0, i;
	size_t oi = 0, oc = 0;

	while (w && d < 16) {
		chain[d++] = w;
		w = w->parent;
	}
	for (i = d - 1; i >= 0; i--) {
		oi += snprintf(inst + oi, n - oi, "%s%s", oi ? "." : "",
			       chain[i]->name);
		oc += snprintf(cls + oc, n - oc, "%s%s", oc ? "." : "",
			       class_name(chain[i]));
	}
	if (leaf) {
		snprintf(inst + oi, n - oi, ".%s", leaf);
		snprintf(cls + oc, n - oc, ".%s", leaf);
	}
}

/* Look up one resource for a widget. Returns NULL when unset. */
static const char *res_get(struct wid *w, const char *name, const char *class)
{
	char inst[256], cls[256];
	char *type = NULL;
	XrmValue v;

	path_of(w, name, inst, cls, sizeof(inst));
	/* The class path needs the resource's CLASS as its leaf, not its name. */
	{
		char *dot = strrchr(cls, '.');

		if (dot)
			snprintf(dot + 1, sizeof(cls) - (dot + 1 - cls), "%s",
				 class);
	}
	if (XrmGetResource(xt_db, inst, cls, &type, &v) && v.addr)
		return (const char *)v.addr;
	return NULL;
}

static int res_int(struct wid *w, const char *n, const char *c, int dflt)
{
	const char *s = res_get(w, n, c);

	return s ? atoi(s) : dflt;
}

static unsigned long res_pixel(struct wid *w, const char *n, const char *c,
			       unsigned long dflt)
{
	const char *s = res_get(w, n, c);
	XColor col;

	if (!s)
		return dflt;
	if (XParseColor(xt_dpy, DefaultColormap(xt_dpy, 0), s, &col) &&
	    XAllocColor(xt_dpy, DefaultColormap(xt_dpy, 0), &col))
		return col.pixel;
	return dflt;
}

const char *xt_res_lookup(Widget w, const char *name, const char *class)
{
	return res_get(WID(w), name, class);
}

/*
 * Store a value into a resource slot.
 *
 * xt_set_typed() takes the value already in its binary form, as an Arg or an
 * XtRImmediate default carries it; xt_set_from_string() takes the text form,
 * as the resource database carries it. Both have to respect resource_size,
 * because Dimension and Position are shorts and writing an int over one
 * corrupts the field next to it - which is how a widget ends up with a
 * plausible width and a garbage height.
 */
void xt_set_typed(void *slot, const char *type, unsigned size, long value)
{
	/*
	 * Float is not an integer of the same width. xclock's `update` is an
	 * XtRFloat, and storing the integer 1 in it gives 1.4e-45 - so
	 * update*1000 rounded to zero, the delay to the next tick came out
	 * negative, wrapped to 4,249,431,067 ms, and the clock sat at 12:00
	 * spinning through a timeout every few milliseconds. The board has a
	 * real single-precision FPU, so this is one instruction.
	 */
	if (type && !strcmp(type, XtRFloat)) {
		*(float *)slot = (float)value;
		return;
	}
	switch (size) {
	case 1: *(char *)slot = (char)value; break;
	case 2: *(short *)slot = (short)value; break;
	case 4: *(long *)slot = value; break;
	default: memcpy(slot, &value, size < sizeof(long) ? size
						          : sizeof(long));
	}
}

void xt_set_from_string(void *slot, const char *type, unsigned size,
			const char *v)
{
	if (!v)
		return;
	if (!strcmp(type, XtRString)) {
		*(const char **)slot = v;
	} else if (!strcmp(type, XtRBoolean) || !strcmp(type, XtRBool)) {
		*(char *)slot = (*v == 't' || *v == 'T' || *v == 'y' ||
				 *v == 'Y' || *v == '1');
	} else if (!strcmp(type, XtRPixel) || !strcmp(type, "Color")) {
		XColor c;

		if (XParseColor(xt_dpy, DefaultColormap(xt_dpy, 0), v, &c) &&
		    XAllocColor(xt_dpy, DefaultColormap(xt_dpy, 0), &c))
			xt_set_typed(slot, type, size, (long)c.pixel);
	} else if (!strcmp(type, XtRFontStruct)) {
		XFontStruct *f = XLoadQueryFont(xt_dpy, v);

		if (f)
			*(XFontStruct **)slot = f;
	} else if (!strcmp(type, XtRFont)) {
		XFontStruct *f = XLoadQueryFont(xt_dpy, v);

		if (f)
			xt_set_typed(slot, type, size, (long)f->fid);
	} else if (!strcmp(type, XtRFloat)) {
		*(float *)slot = strtof(v, NULL);
	} else if (!strcmp(type, XtRInt) || !strcmp(type, XtRDimension) ||
		   !strcmp(type, XtRPosition) || !strcmp(type, XtRShort) ||
		   !strcmp(type, XtRCardinal)) {
		xt_set_typed(slot, type, size, atol(v));
	} else if (!strcmp(type, XtRCursor) || !strcmp(type, XtRPixmap) ||
		   !strcmp(type, XtRCallback)) {
		/* Nothing here draws a cursor or a pixmap; leave the default. */
	} else {
		xt_ignored("resource type", type);
	}
}

/* --------------------------------------------------------------- widgets */

/*
 * Apply one widget argument. The application is off-the-shelf, so anything it
 * sets and we ignore is a decision being made by US instead of by it - which
 * is how xcalc's explicitly right-justified display ended up centred.
 */
static int apply_arg(struct wid *w, const char *name, long value)
{
	if (!strcmp(name, "label")) {
		snprintf(w->label, sizeof(w->label), "%s",
			 (const char *)value);
		return 1;
	}
	if (!strcmp(name, "justify")) {		/* XtJustifyLeft/Center/Right */
		w->justify = (int)value;
		return 1;
	}
	if (!strcmp(name, "borderWidth")) {
		w->bw = (int)value;
		return 1;
	}
	if (!strcmp(name, "width")) {
		w->pref_w = (int)value;
		return 1;
	}
	if (!strcmp(name, "height")) {
		w->pref_h = (int)value;
		return 1;
	}
	if (!strcmp(name, "state")) {
		w->set = (int)value;
		return 1;
	}
	xt_ignored("argument", name);
	return 0;
}

/*
 * Allocate a widget. `recsize` is the size of the APPLICATION-visible record
 * that follows ours; every widget has one, so WIDGET()/WID() is a single
 * constant offset whether the class is ours or the application's.
 */
struct wid *xt_wid_new(const char *name, enum wclass cls, struct wid *parent,
		       size_t recsize)
{
	struct wid *w = calloc(1, sizeof(*w) + recsize);

	if (!w)
		return NULL;
	snprintf(w->name, sizeof(w->name), "%s", name ? name : "?");
	w->cls = cls;
	w->parent = parent;
	if (parent) {
		if (parent->nkids == parent->kidcap) {
			int cap = parent->kidcap ? parent->kidcap * 2 : 8;
			struct wid **k = realloc(parent->kids,
						 cap * sizeof(*k));

			if (!k) {
				free(w);
				return NULL;
			}
			parent->kids = k;
			parent->kidcap = cap;
		}
		parent->kids[parent->nkids++] = w;
	}
	return w;
}

static struct wid *find_named(struct wid *root, const char *name)
{
	int i;

	if (!root)
		return NULL;
	if (!strcmp(root->name, name))
		return root;
	for (i = 0; i < root->nkids; i++) {
		struct wid *f = find_named(root->kids[i], name);

		if (f)
			return f;
	}
	return NULL;
}

/* Read every resource a widget cares about, once, at creation. */
static void wid_configure(struct wid *w)
{
	const char *s;

	w->bg = res_pixel(w, "background", "Background",
			  WhitePixel(xt_dpy, 0));
	w->fg = res_pixel(w, "foreground", "Foreground",
			  BlackPixel(xt_dpy, 0));
	w->border = res_pixel(w, "borderColor", "BorderColor",
			      BlackPixel(xt_dpy, 0));
	w->bw = res_int(w, "borderWidth", "BorderWidth",
			w->cls == W_SHELL || w->cls == W_FORM ? 0 : 1);
	s = res_get(w, "label", "Label");
	snprintf(w->label, sizeof(w->label), "%s", s ? s : w->name);

	/*
	 * Explicit geometry. xcalc sets "XCalc*Command.width: 40" and
	 * "height: 26", which is what makes its keypad uniform - sizing every
	 * button from its own label instead gives a ragged grid and pushes the
	 * later rows off the window entirely.
	 */
	{	/* "left" / "center" / "right", or Xaw's numeric values. */
		const char *j = res_get(w, "justify", "Justify");

		w->justify = 1;
		if (j) {
			if (!strncmp(j, "left", 4))       w->justify = 0;
			else if (!strncmp(j, "right", 5)) w->justify = 2;
			else if (j[0] >= '0' && j[0] <= '2') w->justify = *j - '0';
		}
	}
	w->pref_w = res_int(w, "width", "Width", 0);
	w->pref_h = res_int(w, "height", "Height", 0);
	w->horiz_dist = res_int(w, "horizDistance", "HorizDistance", -1);
	w->vert_dist = res_int(w, "vertDistance", "VertDistance", -1);
	s = res_get(w, "fromHoriz", "FromHoriz");
	if (s) w->from_horiz = find_named(xt_root, s);
	s = res_get(w, "fromVert", "FromVert");
	if (s) w->from_vert = find_named(xt_root, s);
	/*
	 * A per-widget font. xcalc asks for -adobe-symbol-* on three buttons
	 * and gets the radical, pi and the division sign from it; drawing them
	 * with the default face renders "O`" and "p" instead, which is what
	 * ignoring this resource looked like.
	 */
	s = res_get(w, "font", "Font");
	if (s) {
		w->fnt = XLoadQueryFont(xt_dpy, s);
		if (!w->fnt)
			fprintf(stderr, "xtlite: %s asked for font '%s' and "
				"the server had none\n", w->name, s);
	}
	s = res_get(w, "radioGroup", "RadioGroup");
	if (s) snprintf(w->radio_group, sizeof(w->radio_group), "%s", s);
}

/*
 * Form layout, which is the only geometry manager xcalc uses.
 *
 * A child sits `horizDistance` right of `fromHoriz` (or of the left edge) and
 * `vertDistance` below `fromVert`. That is the whole of the Form contract that
 * xcalc's resource file exercises, and it is why the app-defaults file is not
 * optional - without it every widget lands at the same place, which is exactly
 * what a missing file looked like earlier in this project.
 */
static void layout(struct wid *w, int defdist)
{
	int i, right = 0, bottom = 0;

	for (i = 0; i < w->nkids; i++) {
		struct wid *c = w->kids[i];
		int hd = c->horiz_dist >= 0 ? c->horiz_dist : defdist;
		int vd = c->vert_dist >= 0 ? c->vert_dist : defdist;

		if (!c->managed)
			continue;
		/* Explicit size wins; otherwise fit the label. */
		if (c->cls != W_FORM || c->pref_w || c->pref_h) {
			XFontStruct *cf = c->fnt ? c->fnt : font;
			int tw = cf ? XTextWidth(cf, c->label,
						 strlen(c->label)) : 8;
			int th = cf ? cf->ascent + cf->descent : 13;

			c->w = c->pref_w ? c->pref_w : tw + 8;
			c->h = c->pref_h ? c->pref_h : th + 4;
		}
		c->x = c->from_horiz ? c->from_horiz->x + c->from_horiz->w +
				       2 * c->from_horiz->bw + hd : hd;
		c->y = c->from_vert ? c->from_vert->y + c->from_vert->h +
				      2 * c->from_vert->bw + vd : vd;
		if (c->cls == W_FORM)
			layout(c, defdist);
		xt_note("  %-10s %3dx%-3d at %3d,%3d  fh=%s fv=%s", c->name,
			c->w, c->h, c->x, c->y,
			c->from_horiz ? c->from_horiz->name : "-",
			c->from_vert ? c->from_vert->name : "-");
		/*
		 * The trailing margin mirrors this child's own leading one, so
		 * a widget placed flush at 0,0 - a custom widget filling its
		 * shell - does not get a stray defdist strip on two sides.
		 */
		if (c->x + c->w + 2 * c->bw + hd > right)
			right = c->x + c->w + 2 * c->bw + hd;
		if (c->y + c->h + 2 * c->bw + vd > bottom)
			bottom = c->y + c->h + 2 * c->bw + vd;
	}
	if (w->nkids) {
		w->w = right;
		w->h = bottom;
	}
}

static void unset_group(struct wid *w, const char *group, struct wid *keep);

static XFontStruct *wfont(struct wid *w)
{
	return w->fnt ? w->fnt : font;
}

static void draw(struct wid *w)
{
	XFontStruct *f = wfont(w);
	int tw, tx, ty;
	unsigned long fg = w->fg, bg = w->bg;

	if (!w->realized || !w->win)
		return;
	if (w->cls == W_CUSTOM) {
		xt_custom_expose(w, NULL);
		return;
	}
	if (w->set) {			/* Toggle/Command "set" is reverse video */
		unsigned long t = fg; fg = bg; bg = t;
	}
	XSetForeground(xt_dpy, gc_fg, bg);
	XFillRectangle(xt_dpy, w->win, gc_fg, 0, 0, w->w, w->h);
	/*
	 * A shell's label is its window TITLE; painting it as content put the
	 * word "xclock" across the middle of the clock face, because the shell
	 * repaints after its child and the child only redraws its hands.
	 */
	if (w->cls == W_FORM || w->cls == W_SHELL || !w->label[0])
		return;
	if (f)
		XSetFont(xt_dpy, gc_fg, f->fid);
	tw = f ? XTextWidth(f, w->label, strlen(w->label)) : 0;
	switch (w->justify) {
	case 0:  tx = 2; break;			/* XtJustifyLeft */
	case 2:  tx = w->w - tw - 2; break;	/* XtJustifyRight */
	default: tx = (w->w - tw) / 2; break;	/* XtJustifyCenter */
	}
	ty = f ? (w->h + f->ascent - f->descent) / 2 : w->h / 2;
	XSetForeground(xt_dpy, gc_fg, fg);
	XDrawString(xt_dpy, w->win, gc_fg, tx, ty, w->label, strlen(w->label));
}

static void realize(struct wid *w)
{
	XSetWindowAttributes a;
	unsigned long mask = CWBackPixel | CWBorderPixel | CWEventMask;

	memset(&a, 0, sizeof(a));
	a.background_pixel = w->bg;
	a.border_pixel = w->border;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
		       KeyPressMask | EnterWindowMask | LeaveWindowMask;
	if (w->cls == W_SHELL) {
		w->win = XCreateWindow(xt_dpy, DefaultRootWindow(xt_dpy),
				       0, 0, w->w, w->h, 0, CopyFromParent,
				       InputOutput, CopyFromParent, mask, &a);
		XStoreName(xt_dpy, w->win, w->label[0] ? w->label : app_name);
	} else {
		w->win = XCreateWindow(xt_dpy, w->parent->win, w->x, w->y,
				       w->w, w->h, w->bw, CopyFromParent,
				       InputOutput, CopyFromParent, mask, &a);
	}
	w->realized = 1;
	if (w->cls == W_CUSTOM)
		xt_custom_resized(w);
	{
		int i;

		for (i = 0; i < w->nkids; i++)
			if (w->kids[i]->managed)
				realize(w->kids[i]);
	}
	XMapWindow(xt_dpy, w->win);
	w->mapped = 1;
}

static struct wid *by_window(struct wid *w, Window win)
{
	int i;

	if (!w)
		return NULL;
	if (w->win == win)
		return w;
	for (i = 0; i < w->nkids; i++) {
		struct wid *f = by_window(w->kids[i], win);

		if (f)
			return f;
	}
	return NULL;
}

/* ---------------------------------------------------------- translations */

/*
 * Parse one translation line, e.g.
 *
 *      <Btn1Down>,<Btn1Up>: toggle()selection()
 *      Ctrl<Key>c:          quit()
 *      :<Key>.:             decimal()
 *
 * The separator is the first ':' AFTER the last '>', which is the only rule
 * that survives both `Ctrl<Key>c:quit()` and the leading-':' form above. Only
 * the LAST event of a sequence is used as the trigger: xcalc's sequences are
 * all press-then-release on the same button, so firing on the release is what
 * the user sees either way.
 */
static int is_event_name(const char *e)
{
	return !strncmp(e, "Btn", 3) || !strcmp(e, "Key") ||
	       !strcmp(e, "KeyPress") || !strcmp(e, "KeyDown") ||
	       !strcmp(e, "KeyUp") || !strcmp(e, "EnterWindow") ||
	       !strcmp(e, "LeaveWindow") || !strcmp(e, "Message") ||
	       !strcmp(e, "Expose") || !strcmp(e, "Motion");
}

/*
 * The separator between the event's detail and its action list: the first ':'
 * whose tail looks like `name(`. Anything simpler gets one of these wrong -
 * `:<Key>>:shr()` binds the '>' KEY, so the last '>' in the line is data, not
 * the end of the event.
 */
static const char *find_sep(const char *p)
{
	for (; *p; p++) {
		const char *q;

		if (*p != ':')
			continue;
		q = p + 1;
		while (*q == ' ' || *q == '\t')
			q++;
		if (!isalpha((unsigned char)*q) && *q != '_')
			continue;
		while (isalnum((unsigned char)*q) || *q == '_')
			q++;
		if (*q == '(')
			return p;
	}
	return NULL;
}

static void parse_line(struct wid *w, const char *line)
{
	const char *lt, *gt, *colon;
	struct trans *t;
	char ev[24], detail[32], mods[32];
	size_t n;

	while (*line == ' ' || *line == '\t')
		line++;
	/*
	 * A directive may sit on the SAME line as the first binding:
	 * "#override<Btn1Down>,<Btn1Up>:reciprocal()". Skipping the whole line
	 * because it starts with '#' left almost every xcalc button with no
	 * translations at all, so clicking did nothing.
	 */
	if (*line == '#') {
		const char *sp = line;

		while (*sp && *sp != '<' && *sp != ' ' && *sp != '\t')
			sp++;
		line = sp;
		while (*line == ' ' || *line == '\t')
			line++;
	}
	if (!*line || *line == '!')
		return;

	/* Last valid <event> in the sequence; its detail runs to the separator. */
	lt = NULL;
	{
		const char *p;

		for (p = line; *p; p++) {
			const char *e;
			char buf[24];

			if (*p != '<')
				continue;
			e = strchr(p, '>');
			if (!e || (size_t)(e - p - 1) >= sizeof(buf))
				continue;
			memcpy(buf, p + 1, e - p - 1);
			buf[e - p - 1] = 0;
			if (is_event_name(buf))
				lt = p;
		}
	}
	if (!lt)
		return;
	gt = strchr(lt, '>');
	if (!gt)
		return;
	colon = find_sep(gt + 1);
	if (!colon)
		return;

	n = gt - lt - 1;
	if (n >= sizeof(ev))
		return;
	memcpy(ev, lt + 1, n); ev[n] = 0;

	n = colon - gt - 1;
	if (n >= sizeof(detail))
		n = sizeof(detail) - 1;
	memcpy(detail, gt + 1, n); detail[n] = 0;

	{	/* Modifiers: whatever precedes this event in its own term. */
		const char *ms = line, *comma = NULL, *p;

		for (p = line; p < lt; p++)
			if (*p == ',')
				comma = p;
		if (comma)
			ms = comma + 1;
		n = lt - ms;
		if (n >= sizeof(mods))
			n = sizeof(mods) - 1;
		memcpy(mods, ms, n); mods[n] = 0;
	}

	if (w->ntrans == w->transcap) {
		int cap = w->transcap ? w->transcap * 2 : 4;
		struct trans *nt = realloc(w->trans, cap * sizeof(*nt));

		if (!nt)
			return;
		w->trans = nt;
		w->transcap = cap;
	}
	t = &w->trans[w->ntrans];
	memset(t, 0, sizeof(*t));

	if (!strncmp(ev, "Btn", 3) && strstr(ev, "Down")) {
		t->type = ButtonPress;  t->detail = ev[3] - '0';
	} else if (!strncmp(ev, "Btn", 3) && strstr(ev, "Up")) {
		t->type = ButtonRelease; t->detail = ev[3] - '0';
	} else if (!strcmp(ev, "Key") || !strcmp(ev, "KeyPress") ||
		   !strcmp(ev, "KeyDown")) {
		t->type = KeyPress;
		t->detail = XStringToKeysym(detail);
		if (!t->detail && detail[0])
			t->detail = (unsigned char)detail[0];
	} else {
		xt_ignored("translation event", ev);
		return;
	}
	if (strstr(mods, "Ctrl"))  t->mods |= ControlMask;
	if (strstr(mods, "Shift")) t->mods |= ShiftMask;
	t->actions = strdup(colon + 1);
	if (!t->actions)
		return;
	w->ntrans++;
}

static void parse_translations(struct wid *w, const char *table)
{
	const char *p = table;

	while (p && *p) {
		const char *nl = strchr(p, '\n');
		char line[256];
		size_t n = nl ? (size_t)(nl - p) : strlen(p);

		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, p, n); line[n] = 0;
		parse_line(w, line);
		p = nl ? nl + 1 : NULL;
	}
	xt_note("%s: %d translations", w->name, w->ntrans);
}

/*
 * The actions a widget class provides itself, as opposed to those the
 * application registers with XtAppAddActions. xcalc's translations end with
 * unset() on every button and toggle() on the mode buttons, and those come
 * from Xaw's Command and Toggle, not from xcalc.
 */
static int builtin_action(struct wid *w, const char *name)
{
	if (!strcmp(name, "set"))        { w->set = 1; draw(w); return 1; }
	if (!strcmp(name, "unset"))      { w->set = 0; draw(w); return 1; }
	if (!strcmp(name, "highlight"))  { return 1; }
	if (!strcmp(name, "reset"))      { w->set = 0; draw(w); return 1; }
	if (!strcmp(name, "toggle")) {
		w->set = !w->set;
		if (w->set && w->radio_group[0])
			unset_group(xt_root, w->radio_group, w);
		draw(w);
		return 1;
	}
	if (!strcmp(name, "notify")) {
		if (w->callback)
			w->callback(WIDGET(w), w->closure, NULL);
		return 1;
	}
	return 0;
}

/* Run "digit(7)unset()" - a chain of name(arg) calls. */
static void run_actions(struct wid *w, const char *spec, XEvent *ev)
{
	const char *p = spec;

	while (*p) {
		char name[48], arg[48];
		const char *op, *cp;
		int i;

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		op = strchr(p, '(');
		if (!op)
			return;
		cp = strchr(op, ')');
		if (!cp)
			return;
		snprintf(name, sizeof(name), "%.*s", (int)(op - p), p);
		snprintf(arg, sizeof(arg), "%.*s", (int)(cp - op - 1), op + 1);
		for (i = 0; i < nactions; i++)
			if (!strcmp(actions[i].name, name)) {
				char *params[1] = { arg };
				unsigned np = arg[0] ? 1 : 0;

				xt_note("action %s(%s) on %s", name, arg,
					w->name);
				actions[i].proc(WIDGET(w), ev, params, &np);
				break;
			}
		if (i == nactions && builtin_action(w, name))
			i = -1;			/* handled by the widget class */
		if (i == nactions) {
			static char last[48];

			if (strcmp(last, name)) {
				snprintf(last, sizeof(last), "%s", name);
				fprintf(stderr, "xtlite: no action named "
					"'%s' - the application bound it in a "
					"translation table\n", name);
			}
		}
		p = cp + 1;
	}
}

static void dispatch(struct wid *w, XEvent *ev)
{
	int i, type = ev->type;
	unsigned detail = 0, mods = 0;

	if (type == ButtonPress || type == ButtonRelease) {
		detail = ev->xbutton.button;
		mods = ev->xbutton.state & (ControlMask | ShiftMask);
	} else if (type == KeyPress) {
		detail = ev->xkey.keycode;
		mods = ev->xkey.state & (ControlMask | ShiftMask);
	}
	xt_note("dispatch type=%d detail=%u mods=%u to %s (%d trans)", type,
		detail, mods, w->name, w->ntrans);
	for (i = 0; i < w->ntrans; i++) {
		struct trans *t = &w->trans[i];

		if (t->type != type)
			continue;
		if (t->detail && t->detail != detail)
			continue;
		if (t->mods != mods)
			continue;
		run_actions(w, t->actions, ev);
		return;
	}
	/* Unbound clicks still walk up to the parent, as Xt propagates them. */
	if (w->parent && (type == ButtonPress || type == ButtonRelease ||
			  type == KeyPress))
		dispatch(w->parent, ev);
}

/* ============================ the exported API ========================== */

#include <X11/Intrinsic.h>
#include "xtstrings.h"

#include <poll.h>

/*
 * Widget classes are opaque tokens. The application receives one of these
 * pointers from us and hands it straight back to XtCreateManagedWidget, so
 * what it points AT is nobody's business but ours - which is the single fact
 * that makes replacing the widget set tractable.
 */
static const enum wclass cls_form = W_FORM, cls_label = W_LABEL;
static const enum wclass cls_command = W_COMMAND, cls_toggle = W_TOGGLE;

/*
 * Telling our tokens apart from a real WidgetClassRec is by identity, not by
 * inspection: a token is four bytes and reading core_class.superclass out of
 * one would run off the end of it.
 */
int xt_is_builtin_class(WidgetClass c)
{
	return c == (WidgetClass)&cls_form || c == (WidgetClass)&cls_label ||
	       c == (WidgetClass)&cls_command ||
	       c == (WidgetClass)&cls_toggle;
}

WidgetClass formWidgetClass    = (WidgetClass)&cls_form;
WidgetClass labelWidgetClass   = (WidgetClass)&cls_label;
WidgetClass commandWidgetClass = (WidgetClass)&cls_command;
WidgetClass toggleWidgetClass  = (WidgetClass)&cls_toggle;

static XtAppContext the_app = (XtAppContext)&running;   /* one, opaque */

XTLITE_IMPL(XtSetLanguageProc)
XtLanguageProc XtSetLanguageProc(XtAppContext app, XtLanguageProc proc,
				 XtPointer data)
{
	(void)app; (void)proc; (void)data;
	xt_ignored("call", "XtSetLanguageProc");
	return NULL;		/* no locale support; xlite says so as well */
}

/* Find and load the application's resource file, as XtResolvePathname does. */
static void load_app_defaults(const char *class)
{
	const char *sp = getenv("XFILESEARCHPATH");
	char path[512];
	XrmDatabase d = NULL;

	if (sp) {
		const char *pct = strstr(sp, "%N");

		if (pct)
			snprintf(path, sizeof(path), "%.*s%s%s",
				 (int)(pct - sp), sp, class, pct + 2);
		else
			snprintf(path, sizeof(path), "%s", sp);
		d = XrmGetFileDatabase(path);
	}
	if (!d) {
		snprintf(path, sizeof(path), "/usr/share/X11/app-defaults/%s",
			 class);
		d = XrmGetFileDatabase(path);
	}
	if (d) {
		xt_note("loaded app-defaults from %s", path);
		XrmMergeDatabases(d, &xt_db);
	} else {
		fprintf(stderr, "xtlite: no app-defaults for %s - the widget "
			"tree will have no resources and will lay out "
			"degenerately\n", class);
	}
}

XTLITE_IMPL(XtAppInitialize)
Widget XtAppInitialize(XtAppContext *app_ret, const char *class,
		       XrmOptionDescRec *options, Cardinal num_options,
		       int *argc, String *argv, String *fallback,
		       ArgList args, Cardinal num_args)
{
	struct wid *shell;

	(void)fallback; (void)args; (void)num_args;
	if (app_ret)
		*app_ret = the_app;
	if (class)
		snprintf(app_class, sizeof(app_class), "%s", class);
	if (argv && argv[0]) {
		const char *b = strrchr(argv[0], '/');

		snprintf(app_name, sizeof(app_name), "%s", b ? b + 1 : argv[0]);
	}

	xt_dpy = XOpenDisplay(NULL);
	if (!xt_dpy) {
		fprintf(stderr, "xtlite: cannot open display\n");
		exit(1);
	}
	load_app_defaults(app_class);
	if (options && num_options && argc && argv)
		XrmParseCommand(&xt_db, options, num_options, app_name,
				argc, argv);
	XrmSetDatabase(xt_dpy, xt_db);

	font = XLoadQueryFont(xt_dpy, "8x13");
	gc_fg = XCreateGC(xt_dpy, DefaultRootWindow(xt_dpy), 0, NULL);
	gc_inv = XCreateGC(xt_dpy, DefaultRootWindow(xt_dpy), 0, NULL);
	if (font)
		XSetFont(xt_dpy, gc_fg, font->fid);

	shell = xt_wid_new(app_name, W_SHELL, NULL, xt_class_size(NULL));
	xt_root = shell;
	wid_configure(shell);
	shell->managed = 1;
	return WIDGET(shell);
}

XTLITE_IMPL(XtCreateManagedWidget)
Widget XtCreateManagedWidget(const char *name, WidgetClass cls, Widget parent,
			     ArgList args, Cardinal n)
{
	struct wid *p = WID(parent);
	struct wid *w;
	Cardinal i;

	/*
	 * Our own classes are opaque tokens; anything else is a class record
	 * the application built for itself, and its instance layout is not
	 * ours to choose. See xtclass.c.
	 */
	if (!xt_is_builtin_class(cls)) {
		Widget cw = xt_custom_create(name, cls, p, args, n);

		if (cw) {
			WID(cw)->managed = 1;
			return cw;
		}
		return NULL;
	}
	w = xt_wid_new(name, *(const enum wclass *)cls, p,
		       xt_class_size(NULL));
	if (!w)
		return NULL;
	w->managed = 1;
	wid_configure(w);
	for (i = 0; args && i < n; i++) {
		/*
		 * Log every argument, including the ones not handled. The
		 * application is off-the-shelf: anything it sets and we ignore
		 * is a rendering decision being made by US instead of by it,
		 * which is exactly how a centred value ends up where a
		 * right-aligned one belongs.
		 */
		apply_arg(w, args[i].name, (long)args[i].value);
	}
	{	/* Per-widget translations come from the resource file. */
		const char *t = res_get(w, "translations", "Translations");

		if (t)
			parse_translations(w, t);
	}
	return WIDGET(w);
}

XTLITE_IMPL(XtCreateWidget)
Widget XtCreateWidget(const char *name, WidgetClass cls, Widget parent,
		      ArgList args, Cardinal n)
{
	Widget w = XtCreateManagedWidget(name, cls, parent, args, n);

	/*
	 * Unmanaged means "do not lay me out yet". Everything here is laid out
	 * once, at realize, so the only difference that survives is the flag.
	 */
	if (w)
		WID(w)->managed = 0;
	return w;
}

XTLITE_IMPL(XtManageChild)
void XtManageChild(Widget w) { if (w) WID(w)->managed = 1; }

XTLITE_IMPL(XtUnmanageChild)
void XtUnmanageChild(Widget w) { if (w) WID(w)->managed = 0; }

/*
 * XtOpenApplication is XtAppInitialize plus the shell's class, which xtlite
 * has only one of. xclock asks for a sessionShellWidgetClass and gets the same
 * top-level window either way: there is no session manager on this board, so
 * the difference between a Session, Application and TopLevel shell is entirely
 * in machinery that would have nothing to talk to.
 */
XTLITE_IMPL(XtOpenApplication)
Widget XtOpenApplication(XtAppContext *app_ret, const char *class,
			 XrmOptionDescRec *options, Cardinal num_options,
			 int *argc, String *argv, String *fallback,
			 WidgetClass shell_class, ArgList args,
			 Cardinal num_args)
{
	(void)shell_class;
	return XtAppInitialize(app_ret, class, options, num_options, argc,
			       argv, fallback, args, num_args);
}

XTLITE_IMPL(XtDisplayToApplicationContext)
XtAppContext XtDisplayToApplicationContext(Display *d)
{
	(void)d;
	return the_app;
}

XTLITE_IMPL(XtWidgetToApplicationContext)
XtAppContext XtWidgetToApplicationContext(Widget w)
{
	(void)w;
	return the_app;
}

XTLITE_IMPL(XtAddCallback)
void XtAddCallback(Widget wi, const char *name, XtCallbackProc proc,
		   XtPointer data)
{
	struct wid *w = WID(wi);

	if (!strcmp(name, XtNcallback)) {
		w->callback = (void (*)(Widget, void *, void *))proc;
		w->closure = data;
		return;
	}
	xt_ignored("callback", name);
}

XTLITE_IMPL(XtRealizeWidget)
void XtRealizeWidget(Widget wi)
{
	struct wid *w = WID(wi);
	int def = res_int(w, "defaultDistance", "Thickness", 4);

	layout(w, def);
	xt_note("realize %s: %dx%d, %d children", w->name, w->w, w->h,
		w->nkids);
	realize(w);
	XFlush(xt_dpy);
}

XTLITE_IMPL(XtAppAddActions)
void XtAppAddActions(XtAppContext app, XtActionList list, Cardinal n)
{
	Cardinal i;

	(void)app;
	for (i = 0; i < n && nactions < MAXACT; i++) {
		actions[nactions].name = list[i].string;
		actions[nactions].proc =
			(void (*)(Widget, XEvent *, char **,
				  unsigned *))list[i].proc;
		nactions++;
	}
	xt_note("%d actions registered", nactions);
}

XTLITE_IMPL(XtAppMainLoop)
void XtAppMainLoop(XtAppContext app)
{
	(void)app;
	while (running) {
		XEvent ev;
		struct wid *w;

		/*
		 * Blocking in XNextEvent() would stop the clock: a widget with
		 * a timeout and no input pending must wait on a DEADLINE, not
		 * on the socket. So flush, then poll the connection with the
		 * time until the next timer, and only read when there is
		 * something there.
		 */
		while (!XPending(xt_dpy)) {
			struct pollfd pfd = { ConnectionNumber(xt_dpy),
					      POLLIN, 0 };
			int wait = xt_timer_wait_ms();

			XFlush(xt_dpy);
			poll(&pfd, 1, wait);
			xt_timer_fire_due();
			if (!running)
				return;
		}
		XNextEvent(xt_dpy, &ev);
		w = by_window(xt_root, ev.xany.window);
		xt_note("event type %d on window 0x%lx -> %s", ev.type,
			(unsigned long)ev.xany.window, w ? w->name : "(none)");
		if (!w)
			continue;
		switch (ev.type) {
		case Expose:
			if (w->cls == W_CUSTOM)
				xt_custom_expose(w, &ev);
			else
				draw(w);
			break;
		case ButtonPress:
		case ButtonRelease:
		case KeyPress:
			dispatch(w, &ev);
			break;
		}
	}
}

XTLITE_IMPL(XtSetValues)
void XtSetValues(Widget wi, ArgList args, Cardinal n)
{
	struct wid *w = WID(wi);
	Cardinal i;
	int redraw = 0;

	for (i = 0; args && i < n; i++)
		redraw |= apply_arg(w, args[i].name, (long)args[i].value);
	if (redraw) {
		draw(w);
		XFlush(xt_dpy);
	}
}

XTLITE_IMPL(XtGetValues)
void XtGetValues(Widget wi, ArgList args, Cardinal n)
{
	struct wid *w = WID(wi);
	Cardinal i;

	for (i = 0; args && i < n; i++) {
		if (!strcmp(args[i].name, "label"))
			*(char **)args[i].value = w->label;
		else if (!strcmp(args[i].name, "state"))
			*(int *)args[i].value = w->set;
	}
}

XTLITE_IMPL(XtParseTranslationTable)
XtTranslations XtParseTranslationTable(const char *table)
{
	return (XtTranslations)table;	/* parsed when it is applied */
}

XTLITE_IMPL(XtOverrideTranslations)
void XtOverrideTranslations(Widget wi, XtTranslations t)
{
	parse_translations(WID(wi), (const char *)t);
}

XTLITE_IMPL(XtDisplay)
Display *XtDisplay(Widget w) { (void)w; return xt_dpy; }
XTLITE_IMPL(XtScreen)
Screen *XtScreen(Widget w) { (void)w; return DefaultScreenOfDisplay(xt_dpy); }
XTLITE_IMPL(XtWindow)
Window XtWindow(Widget w) { return w ? WID(w)->win : None; }

XTLITE_IMPL(XtSetKeyboardFocus)
void XtSetKeyboardFocus(Widget sub, Widget descendant)
{
	(void)sub; (void)descendant;	/* one shell, one focus */
	xt_ignored("call", "XtSetKeyboardFocus");
}

XTLITE_IMPL(XtOwnSelection)
Boolean XtOwnSelection(Widget w, Atom sel, Time t, XtConvertSelectionProc conv,
		       XtLoseSelectionProc lose, XtSelectionDoneProc done)
{
	(void)w; (void)sel; (void)t; (void)conv; (void)lose; (void)done;
	xt_ignored("call", "XtOwnSelection");
	return False;		/* no selection transfer on this desktop */
}

XTLITE_IMPL(XtDestroyApplicationContext)
void XtDestroyApplicationContext(XtAppContext app)
{
	(void)app;
	running = 0;
	if (xt_dpy)
		XCloseDisplay(xt_dpy);
}

/*
 * Application resources: fill the caller's struct from the database, falling
 * back to the defaults it supplied. Only the types xcalc declares are
 * converted; anything else keeps its default and says so.
 */
XTLITE_IMPL(XtGetApplicationResources)
void XtGetApplicationResources(Widget wi, XtPointer base, XtResourceList res,
			       Cardinal n, ArgList args, Cardinal num_args)
{
	struct wid *w = WID(wi);
	Cardinal i;

	(void)args; (void)num_args;
	for (i = 0; i < n; i++) {
		XtResource *r = &res[i];
		char *slot = (char *)base + r->resource_offset;
		const char *v = res_get(w, r->resource_name, r->resource_class);
		const char *type = r->resource_type;

		if (!v) {			/* the caller's default */
			if (r->default_addr && r->default_type &&
			    !strcmp(r->default_type, type))
				memcpy(slot, r->default_addr, r->resource_size);
			else if (r->default_addr)
				memcpy(slot, &r->default_addr,
				       r->resource_size < sizeof(void *)
				       ? r->resource_size : sizeof(void *));
			continue;
		}
		if (!strcmp(type, "String")) {
			*(const char **)slot = v;
		} else if (!strcmp(type, "Boolean")) {
			*(char *)slot = (*v == 't' || *v == 'T' ||
					 *v == 'y' || *v == 'Y' || *v == '1');
		} else if (!strcmp(type, "Int") || !strcmp(type, "Dimension")) {
			if (r->resource_size == sizeof(int))
				*(int *)slot = atoi(v);
			else
				*(short *)slot = (short)atoi(v);
		} else {
			xt_ignored("resource type", type);
		}
	}
}

/* ------------------------------------------------------------ Athena bits */

/*
 * Toggles in a radio group: setting one clears the rest. xcalc uses this for
 * the base and angle-mode indicators.
 */
static void unset_group(struct wid *w, const char *group, struct wid *keep)
{
	int i;

	if (!w)
		return;
	if (w->cls == W_TOGGLE && w != keep && w->radio_group[0] &&
	    !strcmp(w->radio_group, group) && w->set) {
		w->set = 0;
		draw(w);
	}
	for (i = 0; i < w->nkids; i++)
		unset_group(w->kids[i], group, keep);
}

XTLITE_IMPL(XawToggleUnsetCurrent)
void XawToggleUnsetCurrent(Widget radio_group)
{
	struct wid *w = WID(radio_group);

	if (w && w->radio_group[0])
		unset_group(xt_root, w->radio_group, NULL);
}
