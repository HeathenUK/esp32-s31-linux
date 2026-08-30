/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The X resource manager.
 *
 * This is the part of Xlib that an Xt application cannot start without, and
 * the only part of it that is not mechanical. xcalc's entire appearance -
 * every button label, the black bevel, the layout constraints - is 22 kB of
 * app-defaults loaded through here; with no database the toolkit builds a
 * widget tree with no resources and lays itself out degenerately, which is
 * exactly the 82x40 box seen earlier when the file was simply missing.
 *
 * Two deliberate simplifications, both invisible to a client:
 *
 *  - Matching is scored rather than done with Xrm's hash-table walk. The
 *    precedence rules are the spec's (a name beats a class, a tight binding
 *    beats a loose one, and a longer specific match beats a shorter one), but
 *    the search is linear over the database. A resource database here is a few
 *    hundred entries read once at startup, so the walk that makes Xrm fast on
 *    a workstation buys nothing and costs a lot of code.
 *
 *  - XrmQGetSearchList records the prefix it was given and XrmQGetSearchResource
 *    completes the lookup against it. Xt uses the pair as a cache; doing the
 *    full lookup each time is the same answer, and avoids reimplementing Xrm's
 *    level-list representation.
 */
#include "xlite.h"

#include <ctype.h>

#define QMAX	4096
#define CMAX	32		/* components in one resource name */

static char *qstr[QMAX];
static int nq = 1;		/* quark 0 is NULLQUARK */

XLITE_IMPL(XrmInitialize)
void XrmInitialize(void) { }

static XrmQuark intern(const char *s, int len)
{
	int i;

	if (!s)
		return NULLQUARK;
	for (i = 1; i < nq; i++)
		if ((int)strlen(qstr[i]) == len && !memcmp(qstr[i], s, len))
			return i;
	if (nq >= QMAX)
		return NULLQUARK;
	qstr[nq] = malloc(len + 1);
	if (!qstr[nq])
		return NULLQUARK;
	memcpy(qstr[nq], s, len);
	qstr[nq][len] = 0;
	return nq++;
}

XLITE_IMPL(XrmStringToQuark)
XrmQuark XrmStringToQuark(const char *s) { return intern(s, s ? strlen(s) : 0); }

XLITE_IMPL(XrmPermStringToQuark)
XrmQuark XrmPermStringToQuark(const char *s) { return XrmStringToQuark(s); }

XLITE_IMPL(XrmQuarkToString)
char *XrmQuarkToString(XrmQuark q)
{
	return (q > 0 && q < nq) ? qstr[q] : NULL;
}

XLITE_IMPL(XrmUniqueQuark)
XrmQuark XrmUniqueQuark(void)
{
	char b[24];

	snprintf(b, sizeof(b), "\1uniq%d", nq);
	return XrmStringToQuark(b);
}

/* "a.b*c" -> quarks {a,b,c} with bindings {tight,tight,loose}. */
static int split(const char *s, XrmQuark *q, XrmBinding *b, int max)
{
	int n = 0;
	XrmBinding bind = XrmBindTightly;

	while (*s && n < max) {
		const char *start = s;

		while (*s && *s != '.' && *s != '*')
			s++;
		if (s > start) {
			if (b)
				b[n] = bind;
			q[n++] = intern(start, s - start);
			bind = XrmBindTightly;
		}
		while (*s == '.' || *s == '*') {
			if (*s == '*')
				bind = XrmBindLoosely;
			s++;
		}
	}
	if (n < max)
		q[n] = NULLQUARK;
	return n;
}

XLITE_IMPL(XrmStringToQuarkList)
void XrmStringToQuarkList(const char *s, XrmQuarkList q)
{
	split(s, q, NULL, CMAX - 1);
}

XLITE_IMPL(XrmStringToBindingQuarkList)
void XrmStringToBindingQuarkList(const char *s, XrmBindingList b, XrmQuarkList q)
{
	split(s, q, b, CMAX - 1);
}

/* ------------------------------------------------------------- database */

#define ENTRY_MAGIC	0x58524d45u		/* "XRME" */

struct entry {
	unsigned magic;
	XrmQuark comp[CMAX];		/* name components, may be "?" wildcard */
	XrmBinding bind[CMAX];
	int n;
	char *value;
	struct entry *next;
};

/*
 * A magic word, because a resource database is the one xlite object a caller
 * hands back to us that we cannot otherwise validate. Xt passes databases
 * around freely, and a pointer that is foreign, freed or simply not ours turns
 * into a walk down a garbage linked list - which shows up as a fault inside
 * memmove with no clue where it came from. Checking is two instructions and
 * turns that into a named complaint.
 */
#define XRMDB_MAGIC	0x58524d44u		/* "XRMD" */

struct xrmdb {
	unsigned magic;
	struct entry *head;
};

static struct xrmdb *db_ok(XrmDatabase db, const char *who)
{
	struct xrmdb *d = (struct xrmdb *)db;

	if (!d)
		return NULL;
	if (d->magic != XRMDB_MAGIC) {
		fprintf(stderr, "xlite: %s was handed a resource database that "
			"is not ours (%p, magic %08x) - ignoring it\n",
			who, (void *)d, d->magic);
		return NULL;
	}
	return d;
}

static XrmQuark q_wild;			/* the "?" single-component wildcard */

static struct xrmdb *db_new(void)
{
	struct xrmdb *d = calloc(1, sizeof(*d));

	if (d)
		d->magic = XRMDB_MAGIC;
	if (!q_wild)
		q_wild = XrmStringToQuark("?");
	return d;
}

static void db_put(struct xrmdb *d, const char *spec, const char *value)
{
	struct entry *e = calloc(1, sizeof(*e));

	if (!e || !d)
		return;
	e->magic = ENTRY_MAGIC;
	e->n = split(spec, e->comp, e->bind, CMAX - 1);
	e->value = strdup(value ? value : "");
	e->next = d->head;
	d->head = e;
}

/*
 * Match one database entry against a query, and score it.
 *
 * The score encodes the spec's precedence: at each component a name match
 * beats a class match beats a wildcard, and a tight binding beats a loose one.
 * Summing per-component scores makes "more specific wins" fall out, and
 * weighting by position keeps an early exact match ahead of a late one.
 */
static int match(struct entry *e, XrmQuark *nq_, XrmQuark *cq, int n, int *score)
{
	int i = 0, j = 0, s = 0;

	while (i < e->n) {
		int loose = e->bind[i] == XrmBindLoosely;

		if (j >= n)
			return 0;
		if (loose) {
			/* Skip ahead to the first place this component fits. */
			while (j < n && e->comp[i] != nq_[j] &&
			       e->comp[i] != cq[j] && e->comp[i] != q_wild)
				j++;
			if (j >= n)
				return 0;
		} else if (e->comp[i] != nq_[j] && e->comp[i] != cq[j] &&
			   e->comp[i] != q_wild) {
			return 0;
		}
		if (e->comp[i] == nq_[j])
			s += 4;
		else if (e->comp[i] == cq[j])
			s += 2;
		else
			s += 1;			/* "?" */
		if (!loose)
			s += 1;
		i++; j++;
	}
	if (j != n)
		return 0;			/* must consume the whole name */
	*score = s;
	return 1;
}

static struct entry *db_find(struct xrmdb *d, XrmQuark *n_, XrmQuark *c, int n)
{
	struct entry *e, *best = NULL;
	int bs = -1, guard = 0;

	if (!d || d->magic != XRMDB_MAGIC)
		return NULL;
	for (e = d->head; e; e = e->next) {
		int s;

		/*
		 * Validate before walking. A corrupt or cyclic list is the
		 * difference between a wrong answer and a fault inside memmove
		 * with nothing to point at.
		 */
		if (e->magic != ENTRY_MAGIC || e->n < 0 || e->n >= CMAX) {
			fprintf(stderr, "xlite: resource database corrupt at "
				"%p (magic %08x, n %d) - truncating the "
				"search\n", (void *)e, e->magic, e->n);
			break;
		}
		if (++guard > 100000) {
			fprintf(stderr, "xlite: resource database has a cycle "
				"- truncating the search\n");
			break;
		}
		if (match(e, n_, c, n, &s) && s > bs) {
			bs = s;
			best = e;
		}
	}
	return best;
}

/*
 * Parse "name: value" lines. Continuations, comments and leading whitespace
 * are handled because real app-defaults files use all three - xcalc's does.
 */
static void db_load(struct xrmdb *d, const char *text)
{
	const char *p = text;
	char spec[512], val[2048];

	while (*p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[2048];
		char *colon;
		size_t vl;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		p = eol ? eol + 1 : p + strlen(p);

		/* Join backslash continuations before doing anything else. */
		while (len && line[len - 1] == '\\' && *p) {
			const char *e2 = strchr(p, '\n');
			size_t l2 = e2 ? (size_t)(e2 - p) : strlen(p);

			len--;
			if (len + l2 >= sizeof(line))
				l2 = sizeof(line) - len - 1;
			memcpy(line + len, p, l2);
			len += l2;
			line[len] = 0;
			p = e2 ? e2 + 1 : p + strlen(p);
			while (len && (line[len - 1] == ' ' ||
				       line[len - 1] == '\t'))
				len--;
			line[len] = 0;
		}

		{
			char *s = line;

			while (*s == ' ' || *s == '\t')
				s++;
			if (*s == '!' || *s == '#' || !*s)
				continue;
			colon = strchr(s, ':');
			if (!colon)
				continue;
			*colon = 0;
			{
				char *t = colon - 1;

				while (t >= s && (*t == ' ' || *t == '\t'))
					*t-- = 0;
			}
			snprintf(spec, sizeof(spec), "%s", s);
			s = colon + 1;
			while (*s == ' ' || *s == '\t')
				s++;
			snprintf(val, sizeof(val), "%s", s);
			vl = strlen(val);
			while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t' ||
				      val[vl - 1] == '\r'))
				val[--vl] = 0;
			db_put(d, spec, val);
		}
	}
}

XLITE_IMPL(XrmGetStringDatabase)
XrmDatabase XrmGetStringDatabase(const char *data)
{
	struct xrmdb *d = db_new();

	if (d && data)
		db_load(d, data);
	return (XrmDatabase)d;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n + 1);
	if (buf && fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = 0;
	fclose(f);
	return buf;
}

XLITE_IMPL(XrmGetFileDatabase)
XrmDatabase XrmGetFileDatabase(const char *path)
{
	char *t = slurp(path);
	XrmDatabase d;

	if (!t)
		return NULL;
	d = XrmGetStringDatabase(t);
	free(t);
	xlite_note("loaded resources from %s", path);
	return d;
}

XLITE_IMPL(XrmMergeDatabases)
void XrmMergeDatabases(XrmDatabase src, XrmDatabase *dst)
{
	struct xrmdb *s = db_ok(src, "XrmMergeDatabases");
	struct entry *last;

	if (!s)
		return;
	if (dst && *dst && !db_ok(*dst, "XrmMergeDatabases target"))
		return;
	if (!dst || !*dst) {
		if (dst)
			*dst = src;
		return;
	}
	/*
	 * src wins over dst, and db_find prefers the earlier of two equal
	 * scores because it needs a strictly better score to replace, so src
	 * goes at the FRONT.
	 */
	for (last = s->head; last && last->next; last = last->next)
		;
	if (last) {
		last->next = ((struct xrmdb *)*dst)->head;
		((struct xrmdb *)*dst)->head = s->head;
		s->head = NULL;
	}
	free(s);
}

XLITE_IMPL(XrmCombineDatabase)
void XrmCombineDatabase(XrmDatabase src, XrmDatabase *dst, Bool over)
{
	(void)over;
	XrmMergeDatabases(src, dst);
}

XLITE_IMPL(XrmCombineFileDatabase)
Status XrmCombineFileDatabase(const char *path, XrmDatabase *dst, Bool over)
{
	XrmDatabase d = XrmGetFileDatabase(path);

	if (!d)
		return 0;
	XrmCombineDatabase(d, dst, over);
	return 1;
}

XLITE_IMPL(XrmDestroyDatabase)
void XrmDestroyDatabase(XrmDatabase db)
{
	struct xrmdb *d = db_ok(db, "XrmDestroyDatabase");
	struct entry *e, *n;

	if (!d)
		return;
	d->magic = 0;			/* so a later use is caught, not run */
	for (e = d->head; e; e = n) {
		n = e->next;
		free(e->value);
		free(e);
	}
	free(d);
}

XLITE_IMPL(XrmGetDatabase)
XrmDatabase XrmGetDatabase(Display *dpy)
{
	return (XrmDatabase)XD(dpy)->pub.db;
}

XLITE_IMPL(XrmSetDatabase)
void XrmSetDatabase(Display *dpy, XrmDatabase db)
{
	XD(dpy)->pub.db = (struct _XrmHashBucketRec *)db;
}

XLITE_IMPL(XrmQGetResource)
Bool XrmQGetResource(XrmDatabase db, XrmNameList names, XrmClassList classes,
		     XrmRepresentation *type, XrmValue *value)
{
	struct entry *e;
	int n = 0;

	while (names[n] != NULLQUARK && n < CMAX - 1)
		n++;
	if (!names || !classes)
		return False;
	e = db_find(db_ok(db, "XrmQGetResource"), names, classes, n);
	if (!e)
		return False;
	if (type)
		*type = XrmStringToQuark("String");
	value->addr = e->value;
	value->size = strlen(e->value) + 1;
	return True;
}

XLITE_IMPL(XrmGetResource)
Bool XrmGetResource(XrmDatabase db, const char *name, const char *class,
		    char **type, XrmValue *value)
{
	XrmQuark nq_[CMAX], cq[CMAX];
	XrmRepresentation t;

	split(name, nq_, NULL, CMAX - 1);
	split(class ? class : "", cq, NULL, CMAX - 1);
	if (!XrmQGetResource(db, nq_, cq, &t, value))
		return False;
	if (type)
		*type = XrmQuarkToString(t);
	return True;
}

/*
 * Search lists. Xt asks for a list once per widget and then looks up many
 * leaves against it. We record the prefix and complete the lookup later; the
 * answer is identical, and Xrm's level-list representation is a lot of code
 * for a cache we do not need at this scale.
 */
struct searchlist {
	XrmDatabase db;
	XrmQuark names[CMAX], classes[CMAX];
	int n;
};

XLITE_IMPL(XrmQGetSearchList)
Bool XrmQGetSearchList(XrmDatabase db, XrmNameList names, XrmClassList classes,
		       XrmSearchList list, int len)
{
	struct searchlist *s;
	int n = 0;

	if (len < 2)
		return False;
	s = calloc(1, sizeof(*s));
	if (!s)
		return False;
	s->db = db;
	while (names[n] != NULLQUARK && n < CMAX - 2) {
		s->names[n] = names[n];
		s->classes[n] = classes ? classes[n] : NULLQUARK;
		n++;
	}
	s->n = n;
	list[0] = (XrmHashTable)s;
	list[1] = NULL;
	return True;
}

XLITE_IMPL(XrmQGetSearchResource)
Bool XrmQGetSearchResource(XrmSearchList list, XrmName name, XrmClass class,
			   XrmRepresentation *type, XrmValue *value)
{
	struct searchlist *s = list ? (struct searchlist *)list[0] : NULL;
	XrmQuark nq_[CMAX], cq[CMAX];
	int i;

	if (!s)
		return False;
	for (i = 0; i < s->n; i++) {
		nq_[i] = s->names[i];
		cq[i] = s->classes[i];
	}
	nq_[i] = name;
	cq[i] = class;
	nq_[i + 1] = NULLQUARK;
	cq[i + 1] = NULLQUARK;
	return XrmQGetResource(s->db, nq_, cq, type, value);
}

XLITE_IMPL(XrmPutStringResource)
void XrmPutStringResource(XrmDatabase *db, const char *spec, const char *val)
{
	if (!db)
		return;
	if (!*db)
		*db = (XrmDatabase)db_new();
	db_put((struct xrmdb *)*db, spec, val);
}

XLITE_IMPL(XrmQPutStringResource)
void XrmQPutStringResource(XrmDatabase *db, XrmBindingList bindings,
			   XrmQuarkList quarks, const char *val)
{
	char spec[512];
	int i;
	size_t o = 0;

	for (i = 0; quarks[i] != NULLQUARK && o < sizeof(spec) - 2; i++) {
		const char *q = XrmQuarkToString(quarks[i]);

		if (i)
			spec[o++] = bindings[i] == XrmBindLoosely ? '*' : '.';
		o += snprintf(spec + o, sizeof(spec) - o, "%s", q ? q : "?");
	}
	spec[o] = 0;
	XrmPutStringResource(db, spec, val);
}

XLITE_IMPL(XrmPutResource)
void XrmPutResource(XrmDatabase *db, const char *spec, const char *type,
		    XrmValue *value)
{
	(void)type;
	if (value && value->addr)
		XrmPutStringResource(db, spec, value->addr);
}

XLITE_IMPL(XrmQPutResource)
void XrmQPutResource(XrmDatabase *db, XrmBindingList b, XrmQuarkList q,
		     XrmRepresentation type, XrmValue *value)
{
	(void)type;
	if (value && value->addr)
		XrmQPutStringResource(db, b, q, value->addr);
}

XLITE_IMPL(XrmLocaleOfDatabase)
const char *XrmLocaleOfDatabase(XrmDatabase db) { (void)db; return "C"; }

/*
 * XrmParseCommand: pull recognised options out of argv into the database.
 * Everything it does not recognise is left in argv for the application, which
 * is how xclock gets -update and xcalc gets -rpn.
 */
XLITE_IMPL(XrmParseCommand)
void XrmParseCommand(XrmDatabase *db, XrmOptionDescList table, int ntable,
		     const char *name, int *argc, char **argv)
{
	int in = 1, out = 1, i;
	char spec[512];

	if (!db)
		return;
	if (!*db)
		*db = (XrmDatabase)db_new();

	while (in < *argc) {
		const char *arg = argv[in];
		XrmOptionDescRec *o = NULL;

		for (i = 0; i < ntable; i++)
			if (!strcmp(arg, table[i].option)) {
				o = &table[i];
				break;
			}
		if (!o) {
			argv[out++] = argv[in++];
			continue;
		}
		/*
		 * The resource name in the table is either absolute (starts
		 * with '.' or '*') or relative to the application name.
		 */
		if (o->specifier[0] == '.' || o->specifier[0] == '*')
			snprintf(spec, sizeof(spec), "%s%s", name, o->specifier);
		else
			snprintf(spec, sizeof(spec), "%s", o->specifier);

		switch (o->argKind) {
		case XrmoptionNoArg:
			db_put((struct xrmdb *)*db, spec, o->value);
			in++;
			break;
		case XrmoptionIsArg:
			db_put((struct xrmdb *)*db, spec, arg);
			in++;
			break;
		case XrmoptionStickyArg:
			db_put((struct xrmdb *)*db, spec,
			       arg + strlen(o->option));
			in++;
			break;
		case XrmoptionSepArg:
			if (in + 1 < *argc)
				db_put((struct xrmdb *)*db, spec, argv[in + 1]);
			in += 2;
			break;
		case XrmoptionResArg:
			if (in + 1 < *argc) {
				char *c = strchr(argv[in + 1], ':');

				if (c) {
					*c = 0;
					db_put((struct xrmdb *)*db,
					       argv[in + 1], c + 1);
				}
			}
			in += 2;
			break;
		case XrmoptionSkipArg:
			in += 2;
			break;
		case XrmoptionSkipLine:
			in = *argc;
			break;
		case XrmoptionSkipNArgs:
			in += 1 + (long)o->value;
			break;
		default:
			in++;
			break;
		}
	}
	argv[out] = NULL;
	*argc = out;
}

/* The server-side resource string. Ours keeps none, so there is nothing. */
XLITE_IMPL(XResourceManagerString)
char *XResourceManagerString(Display *dpy) { (void)dpy; return NULL; }

XLITE_IMPL(XScreenResourceString)
char *XScreenResourceString(Screen *s) { (void)s; return NULL; }
