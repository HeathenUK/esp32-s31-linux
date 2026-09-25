/*
 * capture.c - LD_PRELOAD shim for the glref A/B harness (tools/glref).
 *
 * Runs a stock GL app unmodified and writes the app window's pixels after
 * exactly GLREF_FRAME glXSwapBuffers calls, then _exit()s. It is the same shim
 * for Mesa and for our libGL, and it never calls GL itself: the pixels come
 * from the X server (XGetImage on the root window, clipped to the GL window)
 * over a private X connection, so the capture is implementation-neutral.
 *
 * Deterministic time. Every clock the APP reads (gettimeofday, clock_gettime,
 * time, clock) returns
 *      fixed_base + swaps * GLREF_FRAME_NS + virtual_sleep
 * so an animation driven by elapsed time shows the same phase at swap N under
 * any GL. Sleeps and finite select/poll timeouts issued by the app advance the
 * virtual clock instead of waiting (after a short real poll so X events that
 * are genuinely in flight still arrive), so timer-driven apps still animate.
 * "The app" is any caller whose return address is NOT inside a GL/X/libc
 * library (see lib_is_real()): Mesa's worker threads, libxcb's blocking poll
 * and our own libGL keep real time.
 *
 * Deterministic X events. When the app polls Xlib (XPending, XEventsQueued)
 * or waits on its X connection with a timeout, the shim XSyncs that
 * connection first, so every event the server owes it is already queued: the
 * app sees an infinitely fast server, identically under Mesa and ours.
 *
 * Environment (all set by run.sh):
 *   GLREF_OUT       output path stem; writes <stem>.ppm and <stem>.meta
 *   GLREF_FRAME     capture after this many swaps (default 1; 0 = never, only
 *                   the stall watchdog captures)
 *   GLREF_FRAME_NS  virtual time per swap, ns (default 16666667 = 1/60 s)
 *   GLREF_CAPTURE   "window" (default: the swapped drawable) or "screen"
 *   GLREF_STALL_MS  if no swap happens for this long (real time) before frame
 *                   N, capture the last completed frame and exit 3
 *                   (default 4000; 0 disables)
 *   GLREF_TRACE     set: log every swap (drawable, virtual time) to stderr
 *   GLREF_REAL_LIBS extra ':'-separated substrings of library paths whose
 *                   time calls stay real
 *
 * Exit codes: 0 captured frame N; 3 captured after a stall (meta says at
 * which swap); 4 capture failed (meta says why).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

#pragma GCC diagnostic ignored "-Wnonnull-compare" /* glibc marks tv nonnull */

/* ---------------------------------------------------------------- real fns */

static int (*real_gettimeofday)(struct timeval *, void *);
static int (*real_clock_gettime)(clockid_t, struct timespec *);
static time_t (*real_time)(time_t *);
static clock_t (*real_clock)(void);
static int (*real_nanosleep)(const struct timespec *, struct timespec *);
static int (*real_clock_nanosleep)(clockid_t, int, const struct timespec *, struct timespec *);
static int (*real_usleep)(useconds_t);
static unsigned (*real_sleep)(unsigned);
static int (*real_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static int (*real_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);

static void (*real_glXSwapBuffers)(Display *, GLXDrawable);
static Bool (*real_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
static Bool (*real_glXMakeContextCurrent)(Display *, GLXDrawable, GLXDrawable, GLXContext);
static GLXWindow (*real_glXCreateWindow)(Display *, GLXFBConfig, Window, const int *);
typedef void (*glref_fp)(void);
static glref_fp (*real_glXGetProcAddress)(const GLubyte *);
static glref_fp (*real_glXGetProcAddressARB)(const GLubyte *);

#define RESOLVE(n) do { if (!real_##n) real_##n = dlsym(RTLD_NEXT, #n); } while (0)

/* ------------------------------------------------------------ virtual time */

#define BASE_REAL_S 1700000000LL /* fixed epoch: srand(time(NULL)) is stable too */
#define BASE_MONO_S 1000LL

static int64_t frame_ns = 16666667;
static int64_t swaps;       /* completed glXSwapBuffers calls */
static int64_t slept_ns;    /* virtual time the app spent "sleeping" */

static int64_t vnow(void)
{
    return __atomic_load_n(&swaps, __ATOMIC_RELAXED) * frame_ns +
           __atomic_load_n(&slept_ns, __ATOMIC_RELAXED);
}
static void vadvance(int64_t ns)
{
    if (ns > 0)
        __atomic_add_fetch(&slept_ns, ns, __ATOMIC_RELAXED);
}

/* Is the caller at return address ra library code that must keep real time? */
static const char *extra_real;
static struct { void *base; int real; } cache[128];
static int ncache;
static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static void *self_base;

static int lib_is_real(const char *path)
{
    static const char *const pats[] = {
        "libGL.so", "libGLX", "libGLdispatch", "libOpenGL", "libEGL", "libglapi",
        "libgallium", "libLLVM", "_dri.so", "libdrm", "libvulkan", "libxcb",
        "libX11", "libXext", "libc.so", "libpthread", "ld-linux", "libstdc++",
        "/gl/out-host/", NULL,
    };
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    for (int i = 0; pats[i]; i++) {
        const char *p = pats[i];
        if (p[0] == '/') { if (strstr(path, p)) return 1; }
        else if (strstr(b, p)) return 1;
    }
    if (extra_real) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", extra_real);
        for (char *s = strtok(buf, ":"); s; s = strtok(NULL, ":"))
            if (*s && strstr(path, s)) return 1;
    }
    return 0;
}

static int caller_is_app(void *ra)
{
    Dl_info di;
    if (!ra || !dladdr(ra, &di) || !di.dli_fbase)
        return 0; /* JIT code, unknown: real */
    if (di.dli_fbase == self_base)
        return 0;
    int r = -1;
    pthread_mutex_lock(&cache_mu);
    for (int i = 0; i < ncache; i++)
        if (cache[i].base == di.dli_fbase) { r = cache[i].real; break; }
    if (r < 0) {
        const char *fn = di.dli_fname ? di.dli_fname : "";
        r = lib_is_real(fn);
        if (ncache < (int)(sizeof cache / sizeof cache[0])) {
            cache[ncache].base = di.dli_fbase;
            cache[ncache].real = r;
            ncache++;
        }
    }
    pthread_mutex_unlock(&cache_mu);
    return !r;
}
#define APP() caller_is_app(__builtin_return_address(0))

static int virt_clock(clockid_t c)
{
    switch (c) {
    case CLOCK_REALTIME: case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC: case CLOCK_MONOTONIC_COARSE: case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME: case CLOCK_PROCESS_CPUTIME_ID: case CLOCK_THREAD_CPUTIME_ID:
        return 1;
    }
    return 0;
}

static void vts(clockid_t c, struct timespec *ts)
{
    int64_t n = vnow();
    int64_t base = (c == CLOCK_REALTIME || c == CLOCK_REALTIME_COARSE) ? BASE_REAL_S
                 : (c == CLOCK_PROCESS_CPUTIME_ID || c == CLOCK_THREAD_CPUTIME_ID) ? 0
                 : BASE_MONO_S;
    ts->tv_sec = base + n / 1000000000LL;
    ts->tv_nsec = n % 1000000000LL;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    RESOLVE(gettimeofday);
    if (!APP())
        return real_gettimeofday(tv, tz);
    if (tv) {
        struct timespec ts;
        vts(CLOCK_REALTIME, &ts);
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    if (tz)
        memset(tz, 0, sizeof(struct timezone));
    return 0;
}

int clock_gettime(clockid_t c, struct timespec *ts)
{
    RESOLVE(clock_gettime);
    if (!virt_clock(c) || !APP())
        return real_clock_gettime(c, ts);
    vts(c, ts);
    return 0;
}

time_t time(time_t *t)
{
    RESOLVE(time);
    if (!APP())
        return real_time(t);
    time_t v = (time_t)(BASE_REAL_S + vnow() / 1000000000LL);
    if (t)
        *t = v;
    return v;
}

clock_t clock(void)
{
    RESOLVE(clock);
    if (!APP())
        return real_clock();
    return (clock_t)(vnow() / (1000000000LL / CLOCKS_PER_SEC));
}

/* -------------------------------------------------------------- sleeping */

static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec; }

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    RESOLVE(nanosleep);
    if (!APP() || !req)
        return real_nanosleep(req, rem);
    vadvance(ts_ns(req));
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    sched_yield();
    return 0;
}

int clock_nanosleep(clockid_t c, int flags, const struct timespec *req, struct timespec *rem)
{
    RESOLVE(clock_nanosleep);
    if (!virt_clock(c) || !APP() || !req)
        return real_clock_nanosleep(c, flags, req, rem);
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        vts(c, &now);
        vadvance(ts_ns(req) - ts_ns(&now));
    } else {
        vadvance(ts_ns(req));
    }
    sched_yield();
    return 0;
}

int usleep(useconds_t us)
{
    RESOLVE(usleep);
    if (!APP())
        return real_usleep(us);
    vadvance((int64_t)us * 1000);
    sched_yield();
    return 0;
}

unsigned sleep(unsigned s)
{
    RESOLVE(sleep);
    if (!APP())
        return real_sleep(s);
    vadvance((int64_t)s * 1000000000LL);
    return 0;
}

/* ------------------------------------------------ deterministic X events
 *
 * Whether an Expose/ConfigureNotify has arrived when the app next looks is a
 * real-time race, and it decides which frame a resize lands in. Remove it:
 * whenever the app polls Xlib's queue (XPending, XEventsQueued) or waits on an
 * X connection with a timeout, XSync that connection first. Every event the
 * server generated for requests already sent is then in the queue, so the app
 * sees the event stream of an infinitely fast server - the same one under
 * Mesa and under ours. Calls made from inside a GL/X library are untouched.
 */
static Display *(*real_XOpenDisplay)(const char *);
static int (*real_XCloseDisplay)(Display *);
static int (*real_XPending)(Display *);
static int (*real_XEventsQueued)(Display *, int);

static Display *dpys[16];
static int ndpys;
static pthread_mutex_t dpy_mu = PTHREAD_MUTEX_INITIALIZER;

Display *XOpenDisplay(const char *name)
{
    RESOLVE(XOpenDisplay);
    int app = APP();
    Display *d = real_XOpenDisplay(name);
    if (d && app) {
        pthread_mutex_lock(&dpy_mu);
        if (ndpys < 16)
            dpys[ndpys++] = d;
        pthread_mutex_unlock(&dpy_mu);
    }
    return d;
}

int XCloseDisplay(Display *d)
{
    RESOLVE(XCloseDisplay);
    pthread_mutex_lock(&dpy_mu);
    for (int i = 0; i < ndpys; i++)
        if (dpys[i] == d) { dpys[i] = dpys[--ndpys]; break; }
    pthread_mutex_unlock(&dpy_mu);
    return real_XCloseDisplay(d);
}

int XPending(Display *d)
{
    RESOLVE(XPending);
    if (d && APP())
        XSync(d, False);
    return real_XPending(d);
}

int XEventsQueued(Display *d, int mode)
{
    RESOLVE(XEventsQueued);
    if (d && mode != QueuedAlready && APP())
        XSync(d, False);
    return real_XEventsQueued(d, mode);
}

/* Sync every app display whose fd is in the wait set; return the number of
 * such fds that now have events queued in Xlib (marking them in *ready). */
static int sync_x_fds(int (*in_set)(int fd, void *), void (*mark)(int fd, void *), void *arg)
{
    int hits = 0;
    pthread_mutex_lock(&dpy_mu);
    int n = ndpys;
    Display *ds[16];
    memcpy(ds, dpys, sizeof(Display *) * (size_t)n);
    pthread_mutex_unlock(&dpy_mu);
    RESOLVE(XEventsQueued);
    for (int i = 0; i < n; i++) {
        int fd = ConnectionNumber(ds[i]);
        if (!in_set(fd, arg))
            continue;
        XSync(ds[i], False);
        if (real_XEventsQueued(ds[i], QueuedAlready) > 0) {
            mark(fd, arg);
            hits++;
        }
    }
    return hits;
}

/* A finite wait from the app. After the X sync above there is nothing in
 * flight from the X server, so wait for other fds only briefly (real time);
 * if nothing is ready, pretend the timeout passed. One wait advances virtual
 * time by at most MAX_VWAIT_NS: freeglut with no timers waits "INT_MAX ms",
 * and 24 virtual days would overflow glutGet(GLUT_ELAPSED_TIME). */
#define REAL_WAIT_MS 1
#define MAX_VWAIT_NS (10LL * 1000000000LL)

static int64_t vwait(int64_t ns) { return ns < MAX_VWAIT_NS ? ns : MAX_VWAIT_NS; }

struct selarg { int n; fd_set *r, *w, *e; fd_set rr; int any; };
static int sel_in(int fd, void *a_) { struct selarg *a = a_; return a->r && fd < a->n && FD_ISSET(fd, a->r); }
static void sel_mark(int fd, void *a_) { struct selarg *a = a_; FD_SET(fd, &a->rr); a->any = 1; }

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    RESOLVE(select);
    if (!tv || !APP() || (tv->tv_sec == 0 && tv->tv_usec == 0))
        return real_select(n, r, w, e, tv);
    struct selarg a = { n, r, w, e, {{0}}, 0 };
    FD_ZERO(&a.rr);
    int hits = sync_x_fds(sel_in, sel_mark, &a);
    if (hits) {
        *r = a.rr;
        if (w) FD_ZERO(w);
        if (e) FD_ZERO(e);
        return hits;
    }
    int64_t want = (int64_t)tv->tv_sec * 1000000000LL + (int64_t)tv->tv_usec * 1000;
    struct timeval shortv = { 0, REAL_WAIT_MS * 1000 };
    if (want < REAL_WAIT_MS * 1000000LL)
        shortv.tv_usec = want / 1000;
    int ret = real_select(n, r, w, e, &shortv);
    if (ret != 0)
        return ret;
    vadvance(vwait(want));
    tv->tv_sec = tv->tv_usec = 0;
    return 0;
}

struct pollarg { struct pollfd *f; nfds_t n; };
static int poll_in(int fd, void *a_)
{
    struct pollarg *a = a_;
    for (nfds_t i = 0; i < a->n; i++)
        if (a->f[i].fd == fd && (a->f[i].events & POLLIN)) return 1;
    return 0;
}
static void poll_mark(int fd, void *a_)
{
    struct pollarg *a = a_;
    for (nfds_t i = 0; i < a->n; i++)
        if (a->f[i].fd == fd) a->f[i].revents |= POLLIN;
}
static int poll_pre(struct pollfd *fds, nfds_t n)
{
    struct pollarg a = { fds, n };
    for (nfds_t i = 0; i < n; i++)
        fds[i].revents = 0;
    return sync_x_fds(poll_in, poll_mark, &a);
}

int poll(struct pollfd *fds, nfds_t n, int ms)
{
    RESOLVE(poll);
    if (ms <= 0 || !APP())
        return real_poll(fds, n, ms);
    int hits = poll_pre(fds, n);
    if (hits)
        return hits;
    int ret = real_poll(fds, n, ms < REAL_WAIT_MS ? ms : REAL_WAIT_MS);
    if (ret != 0)
        return ret;
    vadvance(vwait((int64_t)ms * 1000000LL));
    return 0;
}

int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *t, const sigset_t *ss)
{
    RESOLVE(ppoll);
    if (!t || (t->tv_sec == 0 && t->tv_nsec == 0) || !APP())
        return real_ppoll(fds, n, t, ss);
    int hits = poll_pre(fds, n);
    if (hits)
        return hits;
    int64_t want = ts_ns(t);
    struct timespec s = { 0, REAL_WAIT_MS * 1000000L };
    if (want < REAL_WAIT_MS * 1000000LL)
        s.tv_nsec = want;
    int ret = real_ppoll(fds, n, &s, ss);
    if (ret != 0)
        return ret;
    vadvance(vwait(want));
    return 0;
}

/* --------------------------------------------------------------- capture */

static const char *out_stem;
static int64_t want_frame = 1;
static int cap_screen;
static int64_t stall_ms = 4000;
static int trace;

static pthread_mutex_t cap_mu = PTHREAD_MUTEX_INITIALIZER;
static Display *cdpy;              /* our private connection */
static Window last_win;            /* X window of the last swap / MakeCurrent */
static Display *last_dpy;
static int64_t last_swap_real_ms;
static int captured;

static struct { GLXWindow g; Window w; } gwin[64];
static int ngwin;

static Window to_xwin(GLXDrawable d)
{
    for (int i = 0; i < ngwin; i++)
        if (gwin[i].g == d)
            return gwin[i].w;
    return (Window)d;
}

static int64_t real_ms(void)
{
    struct timespec ts;
    RESOLVE(clock_gettime);
    real_clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_meta(const char *status, int64_t at_swap, int x, int y, int w, int h, const char *why)
{
    char path[1024];
    snprintf(path, sizeof path, "%s.meta", out_stem);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "status=%s\nswap=%lld\nwant_frame=%lld\nx=%d\ny=%d\nw=%d\nh=%d\nvirtual_ms=%lld\n",
            status, (long long)at_swap, (long long)want_frame, x, y, w, h,
            (long long)(vnow() / 1000000));
    if (why)
        fprintf(f, "why=%s\n", why);
    fclose(f);
}

static int ctz32(unsigned long m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int bits(unsigned long m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

/* Grab the window's on-screen rectangle from the ROOT window (no BadMatch for
 * child windows, and identical pixels for an unobscured window). */
static int do_capture(Window win, const char *status, int64_t at_swap)
{
    if (!out_stem)
        return -1;
    if (!cdpy) {
        RESOLVE(XOpenDisplay);
        cdpy = real_XOpenDisplay(NULL); /* ours: never recorded as an app display */
    }
    if (!cdpy) {
        write_meta("capture-failed", at_swap, 0, 0, 0, 0, "cannot open private display");
        return -1;
    }
    Window root = DefaultRootWindow(cdpy);
    XWindowAttributes ra;
    XGetWindowAttributes(cdpy, root, &ra);
    int x = 0, y = 0, w = ra.width, h = ra.height;
    if (!cap_screen) {
        XWindowAttributes wa;
        Window child;
        if (!win || !XGetWindowAttributes(cdpy, win, &wa)) {
            write_meta("capture-failed", at_swap, 0, 0, 0, 0, "no window / bad window");
            return -1;
        }
        if (wa.map_state != IsViewable) {
            write_meta("capture-failed", at_swap, 0, 0, 0, 0, "window not viewable");
            return -1;
        }
        XTranslateCoordinates(cdpy, win, root, 0, 0, &x, &y, &child);
        w = wa.width;
        h = wa.height;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > ra.width) w = ra.width - x;
        if (y + h > ra.height) h = ra.height - y;
        if (w <= 0 || h <= 0) {
            write_meta("capture-failed", at_swap, x, y, w, h, "window off screen");
            return -1;
        }
    }
    XImage *img = XGetImage(cdpy, root, x, y, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (!img) {
        write_meta("capture-failed", at_swap, x, y, w, h, "XGetImage failed");
        return -1;
    }
    unsigned long mask[3] = { img->red_mask, img->green_mask, img->blue_mask };
    if (!mask[0] && !mask[1] && !mask[2]) {
        Visual *v = DefaultVisual(cdpy, DefaultScreen(cdpy));
        mask[0] = v->red_mask; mask[1] = v->green_mask; mask[2] = v->blue_mask;
    }
    int sh[3], mx[3];
    for (int c = 0; c < 3; c++) {
        sh[c] = ctz32(mask[c]);
        mx[c] = (1 << bits(mask[c])) - 1;
        if (mx[c] <= 0) mx[c] = 1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s.ppm", out_stem);
    FILE *f = fopen(path, "wb");
    if (!f) {
        XDestroyImage(img);
        write_meta("capture-failed", at_swap, x, y, w, h, "cannot write ppm");
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    unsigned char *row = malloc((size_t)w * 3);
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            unsigned long p = XGetPixel(img, xx, yy);
            for (int c = 0; c < 3; c++) {
                unsigned long v = (p & mask[c]) >> sh[c];
                row[xx * 3 + c] = (unsigned char)((v * 255 + (unsigned long)mx[c] / 2) / (unsigned long)mx[c]);
            }
        }
        fwrite(row, 3, (size_t)w, f);
    }
    free(row);
    fclose(f);
    XDestroyImage(img);
    write_meta(status, at_swap, x, y, w, h, NULL);
    return 0;
}

static void finish(Window win, const char *status, int64_t at_swap, int code)
{
    pthread_mutex_lock(&cap_mu);
    if (captured) {
        pthread_mutex_unlock(&cap_mu);
        return;
    }
    captured = 1;
    int r = do_capture(win, status, at_swap);
    fflush(NULL);
    fprintf(stderr, "glref: %s at swap %lld -> %s\n", r ? "capture FAILED" : status,
            (long long)at_swap, out_stem ? out_stem : "(no GLREF_OUT)");
    _exit(r ? 4 : code);
}

static void *watchdog(void *arg)
{
    (void)arg;
    struct timespec t = { 0, 100 * 1000000L };
    for (;;) {
        RESOLVE(nanosleep);
        real_nanosleep(&t, NULL);
        int64_t last = __atomic_load_n(&last_swap_real_ms, __ATOMIC_RELAXED);
        if (last && real_ms() - last > stall_ms) {
            /* the app thread may be inside Xlib on its own connection; we only
             * use ours, and the last swap was already XSync'd */
            finish(last_win, "stalled", __atomic_load_n(&swaps, __ATOMIC_RELAXED), 3);
        }
    }
    return NULL;
}

static void note_drawable(Display *dpy, GLXDrawable d)
{
    static int started;
    if (d) {
        last_win = to_xwin(d);
        last_dpy = dpy;
    }
    if (!started && stall_ms > 0) {
        started = 1;
        /* a stall before the first swap (single-buffered app, or it blocks
         * forever before swapping) also counts: arm from MakeCurrent */
        __atomic_store_n(&last_swap_real_ms, real_ms(), __ATOMIC_RELAXED);
        pthread_t th;
        pthread_create(&th, NULL, watchdog, NULL);
        pthread_detach(th);
    }
}

void glXSwapBuffers(Display *dpy, GLXDrawable d)
{
    RESOLVE(glXSwapBuffers);
    real_glXSwapBuffers(dpy, d);
    XSync(dpy, False); /* the present has reached the server before we read it */
    note_drawable(dpy, d);
    int64_t n = __atomic_add_fetch(&swaps, 1, __ATOMIC_RELAXED);
    if (trace)
        fprintf(stderr, "glref: swap %lld drawable 0x%lx vt %lld us\n", (long long)n,
                (unsigned long)d, (long long)(vnow() / 1000));
    __atomic_store_n(&last_swap_real_ms, real_ms(), __ATOMIC_RELAXED);
    if (want_frame > 0 && n == want_frame)
        finish(to_xwin(d), "ok", n, 0);
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable d, GLXContext c)
{
    RESOLVE(glXMakeCurrent);
    Bool r = real_glXMakeCurrent(dpy, d, c);
    if (r && d)
        note_drawable(dpy, d);
    return r;
}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable d, GLXDrawable rd, GLXContext c)
{
    RESOLVE(glXMakeContextCurrent);
    Bool r = real_glXMakeContextCurrent ? real_glXMakeContextCurrent(dpy, d, rd, c) : False;
    if (r && d)
        note_drawable(dpy, d);
    return r;
}

GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig cfg, Window w, const int *attr)
{
    RESOLVE(glXCreateWindow);
    GLXWindow g = real_glXCreateWindow ? real_glXCreateWindow(dpy, cfg, w, attr) : 0;
    if (g && ngwin < 64) {
        gwin[ngwin].g = g;
        gwin[ngwin].w = w;
        ngwin++;
    }
    return g;
}

static glref_fp hooked_proc(const GLubyte *name)
{
    const char *n = (const char *)name;
    if (!n) return NULL;
    if (!strcmp(n, "glXSwapBuffers")) return (glref_fp)glXSwapBuffers;
    if (!strcmp(n, "glXMakeCurrent")) return (glref_fp)glXMakeCurrent;
    if (!strcmp(n, "glXMakeContextCurrent")) return (glref_fp)glXMakeContextCurrent;
    if (!strcmp(n, "glXCreateWindow")) return (glref_fp)glXCreateWindow;
    return NULL;
}

glref_fp glXGetProcAddressARB(const GLubyte *name)
{
    glref_fp h = hooked_proc(name);
    if (h) return h;
    RESOLVE(glXGetProcAddressARB);
    return real_glXGetProcAddressARB ? real_glXGetProcAddressARB(name) : NULL;
}

glref_fp glXGetProcAddress(const GLubyte *name)
{
    glref_fp h = hooked_proc(name);
    if (h) return h;
    RESOLVE(glXGetProcAddress);
    return real_glXGetProcAddress ? real_glXGetProcAddress(name) : NULL;
}

__attribute__((constructor)) static void glref_init(void)
{
    Dl_info di;
    if (dladdr((void *)glref_init, &di))
        self_base = di.dli_fbase;
    const char *s;
    out_stem = getenv("GLREF_OUT");
    if ((s = getenv("GLREF_FRAME"))) want_frame = atoll(s);
    if ((s = getenv("GLREF_FRAME_NS"))) frame_ns = atoll(s);
    if ((s = getenv("GLREF_CAPTURE"))) cap_screen = !strcmp(s, "screen");
    if ((s = getenv("GLREF_STALL_MS"))) stall_ms = atoll(s);
    extra_real = getenv("GLREF_REAL_LIBS");
    trace = getenv("GLREF_TRACE") != NULL;
}
