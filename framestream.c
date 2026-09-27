/*
 * framestream - serve the Steam Frame eye cameras as MJPEG over HTTP.
 *
 * EyeTrackVR passes whatever you type in its camera address field straight to
 * cv2.VideoCapture(addr, CAP_ANY) unless it starts with COM/ /dev/tty, so an
 * "http://host:port/0" MJPEG stream drops straight in.
 *
 * The eye cameras are not V4L2 devices.  Their frames only appear in the
 * udmabuf XRService shares with the `eyetracking` helper on the way to the
 * Hexagon DSP: 8-bit greyscale, 512 bytes per row, 400 rows, one frame per
 * 256 KiB slot.  Those offsets are heap sub-allocations, so they move between
 * sessions and have to be found rather than assumed.  See framecap.c.
 *
 * This is built to come up at boot, long before any of that exists:
 *
 *   - The HTTP server binds and starts serving immediately, handing out a
 *     placeholder frame, so a client that connects early is never refused.
 *   - Discovery retries forever.  Missing SteamVR, missing XRService, an
 *     XRService restart, or an idle headset are all normal states it sits in
 *     and recovers from, not errors it exits on.
 *   - Frames only exist while the headset is being worn, so it locks on when
 *     they start flowing and keeps serving the last good frame when they stop.
 *
 * Build:  make   (or: gcc -O2 -Wall -o framestream framestream.c -ljpeg -lpthread -lm)
 * Needs root: pidfd_getfd() is blocked by kernel.yama.ptrace_scope=1.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <math.h>

#include <jpeglib.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

/* Eye frame geometry inside the shared arena. */
#define EYE_STRIDE   512
#define EYE_HEIGHT   400
#define EYE_SLOT     262144
#define SCAN_BLK     65536

#define MAX_ARENAS   8
#define MAX_STREAMS  8
#define MAX_BLOCKS   1024
#define MAX_RING     32
#define BOUNDARY     "framestreamboundary"

/* ------------------------------------------------------------------ types */

typedef struct {
    unsigned long   ino;
    size_t          size;
    unsigned char  *map;
    int             fd;
    uint64_t        blockfp[MAX_BLOCKS];
    unsigned char   blockchanged[MAX_BLOCKS];
} arena_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;

    unsigned char  *jpg;            /* latest encoded frame                  */
    size_t          jpglen;
    size_t          jpgcap;
    uint64_t        seq;

    double          last_src;       /* when the source last changed          */
    uint64_t        frames;         /* source frames seen, encoded or not    */

    atomic_int      viewers;        /* clients that want this one encoded    */

    /* capture thread only - encoding happens outside the lock */
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr       jerr;
    unsigned char  *spare;          /* encode target, swapped with jpg       */
    size_t          sparecap;
    double          next_due;       /* output rate limiter schedule          */
} stream_t;

/* the eye-frame ring: one origin, one pitch, N slots derived from it */
typedef struct {
    int             arena;
    int             slots;
    long            pitch;
    size_t          off[MAX_RING];
    uint64_t        fp[MAX_RING];
    int             eye[MAX_RING];      /* which physical eye each slot holds */
    bool            both_eyes;
} ring_t;

/* --------------------------------------------------------------- globals */

static arena_t  arenas[MAX_ARENAS];
static int      narenas;

static stream_t streams[MAX_STREAMS];

static pid_t    xr_pid;
static int      xr_pidfd = -1;

/* the ring being streamed, and the capture thread's state around it */
static ring_t   ring;
static bool     locked;                 /* ring is valid and being streamed */
static double   last_any;               /* when a ring slot last changed    */
static int      refreshes[MAX_RING];    /* --verbose statistics             */

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static char     status_line[256] = "starting";

static int      opt_port     = 8090;   /* 8080 is steamwebhelper */
static int      opt_quality  = 80;
static int      opt_fps      = 60;
static int      opt_scale    = 1;
static bool     opt_verbose  = false;
static bool     opt_swap     = false;
static int      opt_cx = 0, opt_cy = 0, opt_cw = 0, opt_ch = 0;

/* output geometry, settled once from --crop and --scale */
static unsigned out_x, out_y, out_w, out_h, out_scale;

/*
 * Orientation fix per physical eye camera.  Eye 1 is mounted upside down
 * relative to eye 0 and needs flipping vertically; eye 0 already arrives the
 * way EyeTrackVR wants it.  Indexed by physical eye rather than by stream
 * number, so a correction stays with the camera it belongs to under --swap.
 * Bit 0 = flip vertically, bit 1 = flip horizontally.
 */
#define FLIP_V 1
#define FLIP_H 2
static int      opt_flip[2] = { 0, FLIP_V };

static volatile sig_atomic_t running = 1;

#define MAX_CLIENTS 24
static int      nclients;
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------- utilities */

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char    buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t    t  = time(NULL);
    struct tm tm;
    char      ts[32];

    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    fprintf(stderr, "[%s] %s\n", ts, buf);
    fflush(stderr);
}

#define vlog(...) do { if (opt_verbose) logmsg(__VA_ARGS__); } while (0)

static void set_status(const char *fmt, ...)
{
    va_list ap;
    char    buf[256];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&state_lock);

    bool changed = strcmp(status_line, buf) != 0;

    snprintf(status_line, sizeof(status_line), "%s", buf);
    pthread_mutex_unlock(&state_lock);

    if (changed)
        logmsg("%s", buf);
}

static int pidfd_open_sys(pid_t pid)
{
    return (int)syscall(SYS_pidfd_open, pid, 0u);
}

static int pidfd_getfd_sys(int pidfd, int targetfd)
{
    return (int)syscall(SYS_pidfd_getfd, pidfd, targetfd, 0u);
}

/*
 * The frames are written by hardware that does not snoop the CPU caches, and
 * nothing here tells the kernel when we read them.  So once a line has been
 * read, the cached copy can keep returning an old frame long after the device
 * has rewritten memory - the stream freezes, or never starts.
 *
 * DMA_BUF_IOCTL_SYNC is the official fix, but on a udmabuf it syncs the whole
 * 16-32 MB heap every call.  Instead, clean+invalidate exactly the lines about
 * to be read, so they come from memory.  DC CIVAC is allowed from user space
 * on arm64 Linux, and because it cleans first it can never discard somebody
 * else's write.
 *
 * Earlier versions got away without this by accident: a scan of the whole
 * arena on every pass of the capture loop pushed everything out of cache.
 */
static size_t cache_line = 64;

static void cache_setup(void)
{
#if defined(__aarch64__)
    uint64_t ctr;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    cache_line = (size_t)4 << ((ctr >> 16) & 0xf);   /* DminLine, in bytes */
#endif
}

static inline void cache_drop_line(const void *p)
{
#if defined(__aarch64__)
    __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
#else
    (void)p;
#endif
}

static inline void cache_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb ish" ::: "memory");
#endif
}

static void cache_drop_range(const unsigned char *p, size_t len)
{
    uintptr_t a   = (uintptr_t)p & ~(uintptr_t)(cache_line - 1);
    uintptr_t end = (uintptr_t)p + len;

    for (; a < end; a += cache_line)
        cache_drop_line((const void *)a);

    cache_barrier();
}

static uint64_t fingerprint(const unsigned char *p, size_t len)
{
    uint64_t h    = 1469598103934665603ULL;
    size_t   step = len / 512;

    if (step == 0)
        step = 1;

    /* make sure the samples come from memory, not a stale cache line */
    for (size_t i = 0; i < len; i += step)
        cache_drop_line(p + i);

    cache_barrier();

    for (size_t i = 0; i < len; i += step) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }

    return h;
}

/* --------------------------------------------------------- process lookup */

static pid_t find_process(const char *needle)
{
    DIR *d = opendir("/proc");

    if (!d)
        return 0;

    struct dirent *e;
    pid_t found = 0;

    while ((e = readdir(d))) {

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;

        char path[288];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);

        FILE *f = fopen(path, "rb");

        if (!f)
            continue;

        char buf[512];
        memset(buf, 0, sizeof(buf));

        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);

        if (!got)
            continue;

        const char *base = strrchr(buf, '/');
        base = base ? base + 1 : buf;

        if (strstr(base, needle)) {
            found = (pid_t)atoi(e->d_name);
            break;
        }
    }

    closedir(d);

    return found;
}

/*
 * A pidfd polls readable once its process has exited.  This runs on every
 * pass of the capture loop, so it replaces re-reading /proc/<pid>/cmdline a
 * few hundred times a second.
 */
static bool process_exited(int pidfd)
{
    struct pollfd pfd = { .fd = pidfd, .events = POLLIN };

    return poll(&pfd, 1, 0) > 0;
}

static bool read_dmabuf_info(pid_t pid, int fd, size_t *size, unsigned long *ino)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", pid, fd);

    FILE *f = fopen(path, "r");

    if (!f)
        return false;

    bool have = false;
    char line[256];

    *ino = 0;

    while (fgets(line, sizeof(line), f)) {

        unsigned long long v;

        if (sscanf(line, "size: %llu", &v) == 1) {
            *size = (size_t)v;
            have  = true;
        } else if (sscanf(line, "ino: %llu", &v) == 1) {
            *ino = (unsigned long)v;
        }
    }

    fclose(f);

    return have;
}

/* ------------------------------------------------------ arena attach/drop */

static void drop_arenas(void)
{
    for (int i = 0; i < narenas; i++) {
        if (arenas[i].map)
            munmap(arenas[i].map, arenas[i].size);
        if (arenas[i].fd >= 0)
            close(arenas[i].fd);
    }

    narenas = 0;

    if (xr_pidfd >= 0) {
        close(xr_pidfd);
        xr_pidfd = -1;
    }

    xr_pid = 0;
}

/*
 * Map the buffers XRService shares with the eyetracking helper.  When that
 * helper is not up yet, fall back to any large udmabuf so a later scan can
 * still find frames.
 */
static bool attach_arenas(void)
{
    unsigned long shared[64];
    int           nshared = 0;

    pid_t et = find_process("eyetracking");

    if (et) {

        char dirpath[64];
        snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", et);

        DIR *d = opendir(dirpath);
        struct dirent *e;

        while (d && (e = readdir(d)) && nshared < 64) {

            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;

            char link[320], target[256];
            snprintf(link, sizeof(link), "/proc/%d/fd/%s", et, e->d_name);

            ssize_t n = readlink(link, target, sizeof(target) - 1);

            if (n < 0)
                continue;

            target[n] = 0;

            if (!strstr(target, "dmabuf"))
                continue;

            size_t        sz;
            unsigned long ino;

            if (!read_dmabuf_info(et, atoi(e->d_name), &sz, &ino))
                continue;

            bool dup = false;

            for (int k = 0; k < nshared; k++)
                if (shared[k] == ino)
                    dup = true;

            if (!dup)
                shared[nshared++] = ino;
        }

        if (d)
            closedir(d);
    }

    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", xr_pid);

    DIR *d = opendir(dirpath);

    if (!d)
        return false;

    struct dirent *e;

    while ((e = readdir(d)) && narenas < MAX_ARENAS) {

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;

        int  xfd = atoi(e->d_name);
        char link[320], target[256];

        snprintf(link, sizeof(link), "/proc/%d/fd/%d", xr_pid, xfd);

        ssize_t n = readlink(link, target, sizeof(target) - 1);

        if (n < 0)
            continue;

        target[n] = 0;

        if (!strstr(target, "dmabuf"))
            continue;

        size_t        sz;
        unsigned long ino;

        if (!read_dmabuf_info(xr_pid, xfd, &sz, &ino))
            continue;

        bool want = false;

        for (int k = 0; k < nshared; k++)
            if (shared[k] == ino)
                want = true;

        if (!nshared && sz >= 8u * 1024 * 1024)
            want = true;

        if (!want)
            continue;

        bool dup = false;

        for (int k = 0; k < narenas; k++)
            if (arenas[k].ino == ino)
                dup = true;

        if (dup)
            continue;

        int fd = pidfd_getfd_sys(xr_pidfd, xfd);

        if (fd < 0)
            continue;

        void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);

        if (m == MAP_FAILED) {
            close(fd);
            continue;
        }

        memset(&arenas[narenas], 0, sizeof(arenas[narenas]));
        arenas[narenas].ino  = ino;
        arenas[narenas].size = sz;
        arenas[narenas].map  = m;
        arenas[narenas].fd   = fd;

        for (size_t o = 0, b = 0; o < sz && b < MAX_BLOCKS; o += SCAN_BLK, b++)
            arenas[narenas].blockfp[b] = fingerprint((unsigned char *)m + o,
                                                     SCAN_BLK);
        narenas++;
    }

    closedir(d);

    return narenas > 0;
}

/* ------------------------------------------------------------- discovery */

/*
 * What the buffer actually looks like, measured rather than assumed:
 *
 *   - Eight frames sit in one region, 512x400 8-bit grey each.
 *   - The spacing is 262208 bytes, NOT the 256 KiB it looks like.  Those extra
 *     64 bytes are why reading on a 256 KiB grid made the picture appear to
 *     scan sideways: each slot came out rotated 64 more pixels than the last,
 *     wrapping exactly one row over the eight.
 *   - Slots 0-3 hold one eye and 4-7 the other, so which eye a frame shows is
 *     fixed by its slot.  Deciding it per frame from the picture was the wrong
 *     approach and is gone.
 *
 * The spacing is still measured at runtime instead of hardcoded, by asking how
 * far one slot has to be rotated to line up with the one before it.
 */

/*
 * Does this look like an eye frame?
 *
 * Measured on real frames versus the memory this used to lock onto by mistake:
 *
 *                       real eye frames     recycled heap
 *   lit fraction        0.27 - 0.34         0.09 - 0.15
 *   local roughness     6.3 - 7.2           3.5 - 3.8 (and 38 for noise)
 *
 * The lit fraction is what separates them.  Roughness does not: the junk is a
 * mostly-black frame with a thin band of noise, so it is *smoother* on average
 * than a real eye - which is exactly why the previous test, an average
 * row-to-row difference, waved it through.  Roughness is only useful as an
 * upper bound to reject pure noise.
 */
static void frame_stats(const unsigned char *p, double *litfrac, double *smooth)
{
    long   lit = 0, n = 0, nr = 0;
    double rough = 0;

    for (unsigned y = 0; y + 1 < EYE_HEIGHT; y += 4)
        for (unsigned x = 0; x + 1 < EYE_STRIDE; x += 4) {

            int v = p[(size_t)y * EYE_STRIDE + x];

            n++;

            if (v > 16) {
                lit++;
                rough += abs(v - p[(size_t)(y + 1) * EYE_STRIDE + x]);
                rough += abs(v - p[(size_t)y * EYE_STRIDE + x + 1]);
                nr += 2;
            }
        }

    *litfrac = n  ? (double)lit / n : 0;
    *smooth  = nr ? rough / nr      : 1e9;
}

static bool looks_like_eye_frame(const unsigned char *p)
{
    double litfrac, smooth;

    frame_stats(p, &litfrac, &smooth);

    return litfrac >= 0.20 && litfrac <= 0.96 && smooth <= 15.0;
}

/*
 * Best-effort "is the headset being worn".  The eye cameras only run while it
 * is, so this keeps discovery from even trying against idle memory.  If the
 * sensor cannot be found the check simply passes and the content test above is
 * left to do the work on its own.
 */
static bool headset_worn(void)
{
    static char path[256];
    static int  state;          /* 0 = unknown, 1 = found, 2 = unavailable */

    if (state == 0) {

        state = 2;

        for (int i = 0; i < 8; i++) {

            char np[128];
            snprintf(np, sizeof(np),
                     "/sys/bus/iio/devices/iio:device%d/name", i);

            FILE *f = fopen(np, "r");

            if (!f)
                continue;

            char nm[64] = "";

            if (fgets(nm, sizeof(nm), f) && strstr(nm, "vcnl")) {
                snprintf(path, sizeof(path),
                         "/sys/bus/iio/devices/iio:device%d/in_proximity_raw", i);
                state = 1;
            }

            fclose(f);

            if (state == 1)
                break;
        }

        if (state == 1)
            logmsg("proximity sensor: %s", path);
    }

    if (state != 1)
        return true;

    FILE *f = fopen(path, "r");

    if (!f)
        return true;

    double v = 0;
    int    got = fscanf(f, "%lf", &v);

    fclose(f);

    if (got != 1)
        return true;

    /* idle reads about 2.7, worn reads 24 and up */
    return v > 8.0;
}

/* average of each column - the shape used for all the alignment maths below */
static void column_profile(const unsigned char *p, float *out)
{
    for (unsigned x = 0; x < EYE_STRIDE; x++) {

        unsigned acc = 0;

        for (unsigned y = 0; y < EYE_HEIGHT; y += 8)
            acc += p[(size_t)y * EYE_STRIDE + x];

        out[x] = (float)acc / (EYE_HEIGHT / 8);
    }
}

static double profile_distance(const float *a, const float *b, unsigned rot)
{
    double d = 0;

    for (unsigned x = 0; x < EYE_STRIDE; x += 2)
        d += fabs(a[(x + rot) % EYE_STRIDE] - b[x]);

    return d / (EYE_STRIDE / 2);
}

/* how far a has to be rotated to line up with b */
static unsigned best_rotation(const float *a, const float *b, double *dist)
{
    unsigned best = 0;
    double   bestd = 1e18;

    for (unsigned r = 0; r < EYE_STRIDE; r++) {

        double d = profile_distance(a, b, r);

        if (d < bestd) {
            bestd = d;
            best  = r;
        }
    }

    if (dist)
        *dist = bestd;

    return best;
}

/* mean brightness of the columns at the two frame edges */
static double edge_darkness(const unsigned char *p)
{
    double acc = 0;
    long   n   = 0;

    for (unsigned y = 0; y < EYE_HEIGHT; y += 4) {

        const unsigned char *r = p + (size_t)y * EYE_STRIDE;

        for (unsigned x = 0; x < 24; x++) {
            acc += r[x] + r[EYE_STRIDE - 1 - x];
            n += 2;
        }
    }

    return n ? acc / n : 1e9;
}

/*
 * Find where the picture starts, in rows: a frame is 400 lit rows followed by
 * padding, so it begins on the first lit row after a run of dark ones.  A
 * candidate from the 4 KiB scan grid can sit anywhere inside a frame, so this
 * searches a wide window.
 */
static bool find_frame_start(const unsigned char *base, size_t limit,
                             size_t hint, size_t *out)
{
    long hint_row = (long)(hint / EYE_STRIDE);
    long maxrow   = (long)(limit / EYE_STRIDE) - 1;

    long lo = hint_row - 560;
    long hi = hint_row + 560;

    if (lo < 0)
        lo = 0;

    if (hi > maxrow)
        hi = maxrow;

    if (hi - lo < 200)
        return false;

    int nrows = (int)(hi - lo + 1);

    if (nrows > 1200)
        nrows = 1200;

    static double mean[1200];

    double total = 0;

    for (int k = 0; k < nrows; k++) {

        const unsigned char *r = base + (size_t)(lo + k) * EYE_STRIDE;

        double acc = 0;

        for (unsigned x = 0; x < EYE_STRIDE; x += 8)
            acc += r[x];

        mean[k] = acc / (EYE_STRIDE / 8);
        total  += mean[k];
    }

    double avg  = total / nrows;
    double dark = avg * 0.18;

    if (dark < 1.5)
        dark = 1.5;

    long best  = -1;
    long bestd = 1L << 30;
    int  run   = 0;

    for (int k = 0; k < nrows; k++) {

        if (mean[k] <= dark) {
            run++;
            continue;
        }

        if (run >= 8) {

            long row = lo + k;
            long d   = labs(row - hint_row);

            if (d < bestd) {
                bestd = d;
                best  = row;
            }
        }

        run = 0;
    }

    if (best < 0)
        return false;

    *out = (size_t)best * EYE_STRIDE;

    return true;
}

static void note_block_changes(void)
{
    for (int a = 0; a < narenas; a++)
        for (size_t o = 0, b = 0; o + SCAN_BLK <= arenas[a].size && b < MAX_BLOCKS;
             o += SCAN_BLK, b++) {

            uint64_t h = fingerprint(arenas[a].map + o, SCAN_BLK);

            if (h != arenas[a].blockfp[b]) {
                arenas[a].blockfp[b]      = h;
                arenas[a].blockchanged[b] = 1;
            }
        }
}

static void clear_block_changes(void)
{
    for (int a = 0; a < narenas; a++)
        memset(arenas[a].blockchanged, 0, sizeof(arenas[a].blockchanged));
}

/*
 * Block tracking only runs while searching - fingerprinting every 64 KiB of
 * both arenas on each 3 ms pass of the locked loop was the single biggest
 * cost in the process.  So on leaving the lock the stored fingerprints are
 * stale; re-take them so only changes from here on count.
 */
static void reset_block_changes(void)
{
    note_block_changes();
    clear_block_changes();
}

static void discovery_wait(void);

static void log_ring(const char *what, const ring_t *r)
{
    char map[MAX_RING * 2 + 1];
    int  mp = 0;

    for (int k = 0; k < r->slots && mp < (int)sizeof(map) - 2; k++) {
        map[mp++] = (char)('0' + r->eye[k]);
        map[mp++] = ' ';
    }

    map[mp] = 0;

    logmsg("%s %d slots at 0x%zx, pitch %ld bytes; eye map: %s",
           what, r->slots, r->off[0], r->pitch, map);
}

static bool discover_ring(ring_t *r)
{
    size_t need = (size_t)EYE_STRIDE * EYE_HEIGHT;

    /* ---- 1. rough candidates: regions being written that look like pictures */
    struct { int arena; size_t off; } cand[32];
    int ncand = 0;

    for (int a = 0; a < narenas && ncand < 32; a++) {

        for (size_t off = 0; off + need <= arenas[a].size; off += 4096) {

            size_t blk = off / SCAN_BLK;

            if (blk >= MAX_BLOCKS || !arenas[a].blockchanged[blk])
                continue;

            if (!looks_like_eye_frame(arenas[a].map + off))
                continue;

            size_t start;

            if (!find_frame_start(arenas[a].map, arenas[a].size, off, &start))
                continue;

            if (start + need > arenas[a].size)
                continue;

            bool dup = false;

            for (int k = 0; k < ncand; k++)
                if (cand[k].arena == a &&
                    (start > cand[k].off ? start - cand[k].off
                                         : cand[k].off - start) < need / 2)
                    dup = true;

            if (!dup && ncand < 32) {
                cand[ncand].arena = a;
                cand[ncand].off   = start;
                ncand++;
            }

            off += need - 4096;
        }
    }

    vlog("search: %d candidate frame(s) in changed memory", ncand);

    if (ncand < 2)
        return false;

    /* keep them in memory order */
    for (int x = 0; x < ncand; x++)
        for (int y = x + 1; y < ncand; y++)
            if (cand[y].arena < cand[x].arena ||
                (cand[y].arena == cand[x].arena && cand[y].off < cand[x].off)) {
                typeof(cand[0]) t = cand[x]; cand[x] = cand[y]; cand[y] = t;
            }

    /*
     * Keep only the arena holding the most candidates, and work entirely
     * within it from here on.
     *
     * Both shared buffers see writes - the second one has unrelated traffic
     * about 22 MB in - and they are different sizes.  An earlier version fixed
     * the arena from cand[0] before filtering, so an offset belonging to the
     * 32 MB buffer could end up being read against the 16 MB one.  That runs
     * off the end of the mapping and segfaults.
     */
    int a = cand[0].arena;

    {
        int count[MAX_ARENAS];

        memset(count, 0, sizeof(count));

        for (int k = 0; k < ncand; k++)
            count[cand[k].arena]++;

        for (int k = 0; k < narenas; k++)
            if (count[k] > count[a])
                a = k;

        int keep = 0;

        for (int k = 0; k < ncand; k++)
            if (cand[k].arena == a)
                cand[keep++] = cand[k];

        ncand = keep;
    }

    if (ncand < 2)
        return false;

    /* ---- 2. verify they are actually being refilled, not just stale pictures */
    {
        uint64_t fp[32];
        int      hits[32];

        for (int k = 0; k < ncand; k++) {
            fp[k]   = fingerprint(arenas[cand[k].arena].map + cand[k].off, need);
            hits[k] = 0;
        }

        double t0 = now_sec();

        while (now_sec() - t0 < 1.0 && running) {

            for (int k = 0; k < ncand; k++) {

                uint64_t f = fingerprint(arenas[cand[k].arena].map + cand[k].off, need);

                if (f != fp[k]) {
                    fp[k] = f;
                    hits[k]++;
                }
            }

            discovery_wait();
        }

        for (int k = 0; k < ncand; k++)
            vlog("  candidate 0x%zx (arena %d): %d refreshes in 1 s%s",
                 cand[k].off, a, hits[k], hits[k] >= 5 ? "" : "  -> dropped");

        int keep = 0;

        for (int k = 0; k < ncand; k++)
            if (hits[k] >= 5)
                cand[keep++] = cand[k];

        ncand = keep;
    }

    if (ncand < 2)
        return false;

    /*
     * ---- 3. pick the reference.
     *
     * Row alignment leaves a sub-row remainder, and that remainder differs per
     * slot, so the reference decides the horizontal framing for all of them.
     * Take the slot whose frame edges are darkest: that is the one whose
     * picture is least likely to be wrapped around the edge of the frame.
     */
    int    ref  = -1;
    double refd = 1e18;

    for (int k = 0; k < ncand; k++) {

        if (cand[k].off + need > arenas[a].size)
            continue;

        double d = edge_darkness(arenas[a].map + cand[k].off);

        if (d < refd) {
            refd = d;
            ref  = k;
        }
    }

    if (ref < 0)
        return false;

    size_t base = cand[ref].off;

    /*
     * ---- 4. measure the spacing.
     *
     * On a 256 KiB grid each successive slot comes out rotated a little
     * further; that rotation is exactly how much bigger than 256 KiB the real
     * spacing is.
     */
    static float pref[EYE_STRIDE], pnext[EYE_STRIDE];

    column_profile(arenas[a].map + base, pref);

    long pitch = EYE_SLOT;

    if (base + EYE_SLOT + need <= arenas[a].size) {

        column_profile(arenas[a].map + base + EYE_SLOT, pnext);

        double   d;
        unsigned rot = best_rotation(pnext, pref, &d);

        /* a rotation past halfway is really a small negative one */
        long adj = (long)rot;

        if (adj > EYE_STRIDE / 2)
            adj -= EYE_STRIDE;

        pitch = (long)EYE_SLOT + adj;
    }

    /* ---- 5. lay the ring out on that spacing */
    int n = 0;

    long first = (long)base;

    while (first - pitch >= 0 && n < MAX_RING &&
           (size_t)(first - pitch) + need <= arenas[a].size) {

        if (!looks_like_eye_frame(arenas[a].map + first - pitch))
            break;

        first -= pitch;
        n++;
    }

    n = 0;

    for (long o = first;
         o + (long)need + EYE_STRIDE <= (long)arenas[a].size && n < MAX_RING;
         o += pitch) {

        if (!looks_like_eye_frame(arenas[a].map + o))
            break;

        r->off[n] = (size_t)o;
        r->fp[n]  = fingerprint(arenas[a].map + o, need);
        n++;
    }

    if (opt_verbose) {

        vlog("  ring walk: %d slot(s) from 0x%lx, pitch %ld", n, first, pitch);

        /* the slots found, plus the two positions either side of the run */
        for (int k = -1; k <= n; k++) {

            long o = first + (long)k * pitch;

            if (o < 0 || (size_t)o + need > arenas[a].size)
                continue;

            double lit, sm;

            frame_stats(arenas[a].map + o, &lit, &sm);
            vlog("    %s 0x%lx  lit %.2f  rough %.1f",
                 k < 0 || k == n ? "past " : "slot ", o, lit, sm);
        }
    }

    if (n < 2)
        return false;

    /*
     * Settle the horizontal framing over the whole ring rather than letting
     * one reference slot decide it.  Shifting the base by d < 512 rotates the
     * columns without changing which rows we read, so try every d and keep the
     * one that leaves the frame edges darkest across all slots.  Picking a
     * reference slot instead made the framing depend on what was in view when
     * the lock happened, and it shifted between locks.
     */
    {
        long   bestd = 0;
        double bestv = 1e18;

        for (long d = 0; d < EYE_STRIDE; d++) {

            double v = 0;
            int    c = 0;

            for (int k = 0; k < n; k++) {

                if (r->off[k] + d + need > arenas[a].size)
                    continue;

                v += edge_darkness(arenas[a].map + r->off[k] + d);
                c++;
            }

            if (c && v / c < bestv) {
                bestv = v / c;
                bestd = d;
            }
        }

        if (bestd) {

            int keep = 0;

            for (int k = 0; k < n; k++)
                if (r->off[k] + bestd + need <= arenas[a].size)
                    r->off[keep++] = r->off[k] + bestd;

            n = keep;

            if (n < 2)
                return false;

            for (int k = 0; k < n; k++)
                r->fp[k] = fingerprint(arenas[a].map + r->off[k], need);
        }
    }

    /*
     * Drop anything that is not actually being refilled.  The forward walk
     * stops on the first position that does not look like a picture, so it can
     * run one past the end of the ring onto a stale frame; that slot would
     * never update but would still be counted and grouped.
     */
    {
        uint64_t before[MAX_RING];

        for (int k = 0; k < n; k++)
            before[k] = fingerprint(arenas[a].map + r->off[k], need);

        double t1 = now_sec();

        bool seen[MAX_RING];

        memset(seen, 0, sizeof(seen));

        while (now_sec() - t1 < 0.6 && running) {

            for (int k = 0; k < n; k++)
                if (fingerprint(arenas[a].map + r->off[k], need) != before[k])
                    seen[k] = true;

            discovery_wait();
        }

        int keep = 0;

        for (int k = 0; k < n; k++)
            if (!seen[k])
                vlog("  slot 0x%zx did not refresh in 0.6 s -> dropped", r->off[k]);

        for (int k = 0; k < n; k++)
            if (seen[k])
                r->off[keep++] = r->off[k];

        if (keep >= 2) {
            n = keep;
            for (int k = 0; k < n; k++)
                r->fp[k] = fingerprint(arenas[a].map + r->off[k], need);
        }
    }

    r->arena = a;
    r->slots = n;
    r->pitch = pitch;

    /*
     * ---- 6. work out which eye each slot belongs to.
     *
     * Slots holding the same eye match each other closely once aligned; the
     * other eye is an order of magnitude further away.  This is decided once,
     * per slot, so a stream can never flicker between eyes.
     */
    static float prof[MAX_RING][EYE_STRIDE];

    for (int k = 0; k < n; k++)
        column_profile(arenas[a].map + r->off[k], prof[k]);

    double dist[MAX_RING];
    double dmin = 1e18, dmax = -1e18;

    for (int k = 0; k < n; k++) {
        dist[k] = profile_distance(prof[k], prof[0], 0);
        if (dist[k] < dmin) dmin = dist[k];
        if (dist[k] > dmax) dmax = dist[k];
    }

    double split = (dmin + dmax) / 2;

    for (int k = 0; k < n; k++)
        vlog("  slot %d distance from slot 0: %.2f", k, dist[k]);

    vlog("  spread %.2f (under 0.8 means one eye only)", dmax - dmin);

    for (int k = 0; k < n; k++)
        r->eye[k] = dist[k] > split ? 1 : 0;

    /* stream 0 is the eye sitting further left, so the mapping is stable */
    double cx[2] = { 0, 0 };
    int    cn[2] = { 0, 0 };

    for (int k = 0; k < n; k++) {

        double num = 0, den = 0;

        for (unsigned x = 0; x < EYE_STRIDE; x++) {
            num += (double)prof[k][x] * x;
            den += prof[k][x];
        }

        if (den > 1) {
            cx[r->eye[k]] += num / den;
            cn[r->eye[k]]++;
        }
    }

    if (cn[0] && cn[1] && (cx[0] / cn[0]) > (cx[1] / cn[1]))
        for (int k = 0; k < n; k++)
            r->eye[k] ^= 1;

    r->both_eyes = dmax - dmin >= 0.8;

    if (!r->both_eyes) {
        /* only one eye visible in this ring - send everything to stream 0 */
        for (int k = 0; k < n; k++)
            r->eye[k] = 0;
    }

    return true;
}

/* ---------------------------------------------------------- JPEG encoding */

static void setup_encoder(stream_t *st)
{
    st->cinfo.err = jpeg_std_error(&st->jerr);
    jpeg_create_compress(&st->cinfo);

    st->cinfo.image_width      = out_w;
    st->cinfo.image_height     = out_h;
    st->cinfo.input_components = 1;
    st->cinfo.in_color_space   = JCS_GRAYSCALE;

    jpeg_set_defaults(&st->cinfo);
    jpeg_set_quality(&st->cinfo, opt_quality, TRUE);

    /* a grey frame this size practically never encodes larger than raw */
    st->sparecap = (size_t)EYE_STRIDE * EYE_HEIGHT;
    st->spare    = malloc(st->sparecap);

    if (!st->spare)
        st->sparecap = 0;
}

/*
 * Encode into st->spare and return the length.  The compressor, its buffer
 * and the row table are all reused, so a frame costs no allocations.  Rows
 * that need no rearranging are handed to libjpeg straight from the source.
 */
static size_t encode_jpeg(stream_t *st, const unsigned char *src, int flip)
{
    static unsigned char tmp[EYE_STRIDE * EYE_HEIGHT];

    JSAMPROW rows[EYE_HEIGHT];
    bool     copy = (flip & FLIP_H) || out_scale > 1;

    /* the whole frame has to come from memory, not just the sampled lines */
    cache_drop_range(src, (size_t)EYE_STRIDE * EYE_HEIGHT);

    for (unsigned y = 0; y < out_h; y++) {

        unsigned oy = (flip & FLIP_V) ? out_h - 1 - y : y;

        const unsigned char *s = src + (size_t)(out_y + oy * out_scale) * EYE_STRIDE
                                     + out_x;

        if (!copy) {
            rows[y] = (JSAMPROW)s;
            continue;
        }

        unsigned char *d = tmp + (size_t)y * EYE_STRIDE;

        if (flip & FLIP_H)
            for (unsigned x = 0; x < out_w; x++)
                d[x] = s[(out_w - 1 - x) * out_scale];
        else
            for (unsigned x = 0; x < out_w; x++)
                d[x] = s[x * out_scale];

        rows[y] = d;
    }

    unsigned char *buf  = st->spare;
    unsigned long  size = st->sparecap;

    jpeg_mem_dest(&st->cinfo, &buf, &size);
    jpeg_start_compress(&st->cinfo, TRUE);

    while (st->cinfo.next_scanline < out_h)
        jpeg_write_scanlines(&st->cinfo, rows + st->cinfo.next_scanline,
                             out_h - st->cinfo.next_scanline);

    jpeg_finish_compress(&st->cinfo);

    /* libjpeg outgrew our buffer and allocated its own - adopt that one */
    if (buf != st->spare) {
        free(st->spare);
        st->spare    = buf;
        st->sparecap = size;
    }

    return size;
}

/* Encode unconditionally and hand the result to the clients. */
static void publish_frame(stream_t *st, const unsigned char *src, int flip)
{
    size_t len = encode_jpeg(st, src, flip);

    pthread_mutex_lock(&st->lock);

    unsigned char *old    = st->jpg;
    size_t         oldcap = st->jpgcap;

    st->jpg      = st->spare;
    st->jpgcap   = st->sparecap;
    st->jpglen   = len;
    st->spare    = old;
    st->sparecap = oldcap;
    st->seq++;

    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->lock);
}

/*
 * Output rate limiter.  It keeps a schedule rather than timing from the last
 * frame, and allows a quarter-interval of slack: frames are only noticed on a
 * 3 ms poll, so a strict "at least 1/fps since the last one" test drops every
 * frame that happens to be seen a hair early and lands well under the target.
 */
static bool rate_ok(double *next_due, double t)
{
    if (opt_fps <= 0)
        return true;

    double iv = 1.0 / opt_fps;

    if (t < *next_due - iv / 4)
        return false;

    /* after a gap, restart the schedule instead of bursting to catch up */
    *next_due = (*next_due < t - iv ? t : *next_due) + iv;

    return true;
}

static void publish(stream_t *st, const unsigned char *src, int flip, double t)
{
    pthread_mutex_lock(&st->lock);
    st->frames++;
    st->last_src = t;
    pthread_mutex_unlock(&st->lock);

    /*
     * Encoding is the expensive part, so only do it for streams someone is
     * actually watching - /2 in particular is a debugging view that used to
     * be encoded at full rate all the time.  And there is no point encoding
     * faster than we will ever send: the ring delivers frames well above the
     * output rate.
     */
    if (!atomic_load(&st->viewers) || !rate_ok(&st->next_due, t))
        return;

    publish_frame(st, src, flip);
}

static void publish_placeholder(void)
{
    static unsigned char blank[EYE_STRIDE * EYE_HEIGHT];

    for (int i = 0; i < MAX_STREAMS; i++)
        publish_frame(&streams[i], blank, 0);
}

/* ------------------------------------------------------- capture pipeline */

/* Publish whatever the ring being streamed has new since the last look. */
static void poll_ring(void)
{
    if (!locked)
        return;

    bool   any = false;
    double t   = now_sec();

    /* whichever slot just changed is holding the newest frame */
    for (int i = 0; i < ring.slots; i++) {

        const unsigned char *src = arenas[ring.arena].map + ring.off[i];
        uint64_t fp = fingerprint(src, (size_t)EYE_STRIDE * EYE_HEIGHT);

        if (fp == ring.fp[i])
            continue;

        ring.fp[i] = fp;
        any = true;
        refreshes[i]++;

        int phys = ring.eye[i];
        int strm = opt_swap ? 1 - phys : phys;

        /* stream 2 is the raw frame, unflipped, for checking framing */
        publish(&streams[2], src, 0, t);
        publish(&streams[strm], src, opt_flip[phys], t);
    }

    if (any)
        last_any = t;
}

/*
 * Discovery spends over a second confirming that candidates keep refreshing.
 * When it runs while a ring is already being streamed, keep that stream
 * going in the meantime rather than freezing it.
 */
static void discovery_wait(void)
{
    poll_ring();
    usleep(4000);
}

/* A ring was found: start streaming it. */
static void adopt_ring(const ring_t *r, const char *what)
{
    ring   = *r;
    locked = true;

    memset(refreshes, 0, sizeof(refreshes));
    log_ring(what, &ring);

    if (ring.both_eyes)
        set_status("streaming: ring of %d slot(s)", ring.slots);
    else
        set_status("streaming one eye (%d slots) - still looking for the other",
                   ring.slots);
}

static void *capture_thread(void *arg)
{
    (void)arg;

    double last_scan   = 0;
    double last_check  = 0;
    double last_report = 0;
    double last_search = 0;
    double search_from = 0;     /* background search: when tracking began */
    int    misaligned  = 0;

    while (running) {

        if (xr_pid && process_exited(xr_pidfd)) {
            logmsg("XRService went away, releasing buffers");
            drop_arenas();
            locked = false;
        }

        if (!xr_pid) {

            pid_t p = find_process("XRService");

            if (!p) {
                set_status("waiting for XRService (is SteamVR running?)");
                for (int w = 0; w < 20 && running; w++)
                    usleep(100000);
                continue;
            }

            int pfd = pidfd_open_sys(p);

            if (pfd < 0) {
                set_status("pidfd_open(%d) failed: %s", p, strerror(errno));
                for (int w = 0; w < 20 && running; w++)
                    usleep(100000);
                continue;
            }

            xr_pid   = p;
            xr_pidfd = pfd;

            logmsg("XRService pid %d", xr_pid);
        }

        if (!narenas) {

            if (!attach_arenas()) {
                set_status("waiting for the shared eye-tracking buffer");
                for (int w = 0; w < 20 && running; w++)
                    usleep(100000);
                continue;
            }

            logmsg("mapped %d shared arena(s)", narenas);
            clear_block_changes();
            last_scan = now_sec();
        }

        if (!locked) {

            note_block_changes();

            set_status("waiting for eye frames - put the headset on "
                       "(or cover the proximity sensor)");

            if (now_sec() - last_scan > 1.0 && headset_worn()) {

                ring_t found;

                last_scan = now_sec();

                if (discover_ring(&found)) {
                    adopt_ring(&found, "locked");
                    last_any    = now_sec();
                    last_check  = now_sec();
                    last_report = now_sec();
                    last_search = now_sec();
                    search_from = 0;
                    misaligned  = 0;
                } else {
                    clear_block_changes();
                }
            }

            usleep(250000);
            continue;
        }

        poll_ring();

        double t = now_sec();

        if (opt_verbose && t - last_report >= 5.0) {

            char line[MAX_RING * 6 + 1];
            int  lp = 0;

            for (int i = 0; i < ring.slots && lp < (int)sizeof(line) - 6; i++)
                lp += snprintf(line + lp, sizeof(line) - lp, " %d", refreshes[i]);

            line[lp] = 0;
            logmsg("slot refreshes in the last %.0f s:%s", t - last_report, line);

            memset(refreshes, 0, sizeof(refreshes));
            last_report = t;
        }

        /*
         * Locked onto one eye only.  Eye tracking runs a reduced mode when the
         * proximity sensor is merely covered rather than the headset worn:
         * one eye's frames, in bursts.  Put the headset on afterwards and the
         * other eye's slots come to life - but the slots we have keep
         * refreshing, so nothing would ever go looking for them, and only a
         * restart found both eyes.  So while one-eyed, search again every few
         * seconds, streaming the current ring throughout, and switch over as
         * soon as a ring with both eyes turns up.
         *
         * Discovery works on memory that changed recently, so each attempt
         * starts change tracking and runs half a second later.  Tracking costs
         * a pass over the whole arena, which is why it is otherwise off while
         * locked.
         */
        if (!ring.both_eyes && !search_from && t - last_search > 5.0) {
            reset_block_changes();
            search_from = t;
        }

        if (search_from && t - search_from > 0.5) {

            ring_t found;

            note_block_changes();
            vlog("one eye only - searching for the other");

            if (discover_ring(&found) && found.both_eyes) {
                adopt_ring(&found, "found both eyes, switching to");
                last_check = now_sec();
                misaligned = 0;
            }

            search_from = 0;
            last_search = now_sec();
            continue;
        }

        /*
         * Re-check alignment periodically.  Each origin is pinned once at lock
         * time, which is right as long as the producer keeps reusing the same
         * slots; if anything ever moves underneath us the picture would quietly
         * start tearing again, so confirm the frame still sits where we think
         * and re-lock if it does not.  Several consecutive failures are needed
         * so one odd frame cannot trip it.
         */
        if (ring.slots && t - last_check > 20.0) {

            last_check = t;

            const unsigned char *p0 = arenas[ring.arena].map + ring.off[0];

            if (looks_like_eye_frame(p0)) {
                misaligned = 0;
            } else if (++misaligned >= 3) {
                logmsg("frame alignment drifted, re-locking");
                misaligned = 0;
                locked     = false;
                reset_block_changes();
                continue;
            }
        }

        if (t - last_any > 10.0) {
            logmsg("eye frames stopped, rescanning");
            locked = false;
            reset_block_changes();
            set_status("idle - headset not worn");
        }

        usleep(3000);
    }

    return NULL;
}

/* ------------------------------------------------------------ HTTP server */

static bool write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {

        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

        if (n <= 0) {
            if (n < 0 && (errno == EINTR))
                continue;
            return false;
        }

        p   += n;
        len -= (size_t)n;
    }

    return true;
}

static void send_simple(int fd, const char *code, const char *ctype,
                        const char *body)
{
    char hdr[512];

    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, ctype, strlen(body));

    write_all(fd, hdr, (size_t)n);
    write_all(fd, body, strlen(body));
}

static int parse_stream_index(const char *path)
{
    if (!strncmp(path, "/left", 5))
        return 0;

    if (!strncmp(path, "/right", 6))
        return 1;

    if (!strncmp(path, "/full", 5))
        return 2;

    if (path[0] == '/' && path[1] >= '0' && path[1] <= '9')
        return path[1] - '0';

    return -1;
}

/*
 * Returns true if the peer has gone away.  We never read from a streaming
 * client, so without this a disconnect is invisible: the thread would sit on
 * the condition variable forever.  Leaking one thread per connection is enough
 * to take the whole server down - EyeTrackVR reconnects freely, and the first
 * version wedged at 1019 threads with nothing listening.
 */
static bool peer_gone(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLRDHUP };

    if (poll(&pfd, 1, 0) <= 0)
        return false;

    return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

/* absolute CLOCK_REALTIME deadline for pthread_cond_timedwait */
static void deadline_in(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);

    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;

    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void serve_mjpeg(int fd, int idx)
{
    char hdr[512];

    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
                     "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                     "Pragma: no-cache\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n"
                     "\r\n");

    if (!write_all(fd, hdr, (size_t)n))
        return;

    stream_t      *st  = &streams[idx];
    uint64_t       got = 0;
    unsigned char *buf = NULL;
    size_t         cap = 0;

    atomic_fetch_add(&st->viewers, 1);

    /*
     * The output rate is already capped where frames are encoded, so every
     * new frame goes straight out.  Pacing here as well used to throw away a
     * frame it had already taken and then wait for the next one.
     */
    while (running && !peer_gone(fd)) {

        size_t len = 0;

        pthread_mutex_lock(&st->lock);

        if (st->seq == got) {

            struct timespec ts;

            deadline_in(&ts, 100);
            pthread_cond_timedwait(&st->cv, &st->lock, &ts);
        }

        /*
         * Send the newest frame, or re-send the last one if nothing arrived.
         * Repeating keeps the client connected while the headset is off, and
         * it is also what makes a dead socket show up as a failed write.
         */
        if (st->jpglen) {

            if (cap < st->jpglen) {

                unsigned char *nb = realloc(buf, st->jpglen);

                if (nb) {
                    buf = nb;
                    cap = st->jpglen;
                }
            }

            if (cap >= st->jpglen) {
                memcpy(buf, st->jpg, st->jpglen);
                len = st->jpglen;
            }

            got = st->seq;
        }

        pthread_mutex_unlock(&st->lock);

        if (!len) {
            usleep(50000);
            continue;
        }

        char part[256];

        int pn = snprintf(part, sizeof(part),
                          "--" BOUNDARY "\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n", len);

        if (!write_all(fd, part, (size_t)pn) ||
            !write_all(fd, buf, len) ||
            !write_all(fd, "\r\n", 2))
            break;
    }

    atomic_fetch_sub(&st->viewers, 1);
    free(buf);
}

static void serve_snapshot(int fd, int idx)
{
    stream_t *st = &streams[idx];

    /*
     * Nothing is encoded for a stream nobody is watching, so register as a
     * viewer and, if the source is live, give the capture thread a moment to
     * produce a fresh frame rather than handing out a stale one.
     */
    atomic_fetch_add(&st->viewers, 1);

    pthread_mutex_lock(&st->lock);

    if (now_sec() - st->last_src < 1.0) {

        struct timespec ts;
        uint64_t        want = st->seq;

        deadline_in(&ts, 250);

        while (st->seq == want &&
               pthread_cond_timedwait(&st->cv, &st->lock, &ts) != ETIMEDOUT)
            ;
    }

    size_t         len  = st->jpglen;
    unsigned char *copy = len ? malloc(len) : NULL;

    if (copy)
        memcpy(copy, st->jpg, len);

    pthread_mutex_unlock(&st->lock);

    atomic_fetch_sub(&st->viewers, 1);

    if (!copy) {
        send_simple(fd, "503 Service Unavailable", "text/plain",
                    "no frame yet\n");
        return;
    }

    char hdr[256];
    int  n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n"
                      "\r\n", len);

    write_all(fd, hdr, (size_t)n);
    write_all(fd, copy, len);

    free(copy);
}

static void serve_status(int fd)
{
    char body[2048];
    char st[256];

    pthread_mutex_lock(&state_lock);
    snprintf(st, sizeof(st), "%s", status_line);
    pthread_mutex_unlock(&state_lock);

    int n = snprintf(body, sizeof(body),
                     "{\n"
                     "  \"status\": \"%s\",\n"
                     "  \"xrservice_pid\": %d,\n"
                     "  \"arenas\": %d,\n"
                     "  \"ring_slots\": %d,\n"
                     "  \"streams\": [", st, xr_pid, narenas, ring.slots);

    for (int i = 0; i < 3 && n < (int)sizeof(body) - 200; i++) {

        pthread_mutex_lock(&streams[i].lock);

        char age[32] = "null";

        if (streams[i].frames)
            snprintf(age, sizeof(age), "%.1f", now_sec() - streams[i].last_src);

        n += snprintf(body + n, sizeof(body) - n,
                      "%s\n    {\"path\": \"/%d\", \"frames\": %llu, "
                      "\"age_s\": %s, \"viewers\": %d}",
                      i ? "," : "", i,
                      (unsigned long long)streams[i].frames,
                      age,
                      atomic_load(&streams[i].viewers));

        pthread_mutex_unlock(&streams[i].lock);
    }

    snprintf(body + n, sizeof(body) - n, "\n  ]\n}\n");

    send_simple(fd, "200 OK", "application/json", body);
}

static void serve_index(int fd)
{
    char body[1600];

    snprintf(body, sizeof(body),
             "<!doctype html><meta charset=utf-8>"
             "<title>Steam Frame eye cameras</title>"
             "<style>body{font:14px system-ui;margin:2rem;background:#111;color:#eee}"
             "img{border:1px solid #444;margin-right:1rem}code{color:#9cf}</style>"
             "<h2>Steam Frame eye cameras</h2>"
             "<p>Paste one of these into the EyeTrackVR camera address field:</p>"
             "<p><code>http://%%HOST%%:%d/0</code> &nbsp; "
             "<code>http://%%HOST%%:%d/1</code></p>"
             "<img src=\"/0\" width=384><img src=\"/1\" width=384>"
             "<p><a style=color:#9cf href=\"/status\">/status</a></p>",
             opt_port, opt_port);

    send_simple(fd, "200 OK", "text/html; charset=utf-8", body);
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    char    buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

    if (n <= 0) {
        close(fd);
        return NULL;
    }

    buf[n] = 0;

    char method[16], path[256];

    if (sscanf(buf, "%15s %255s", method, path) != 2) {
        send_simple(fd, "400 Bad Request", "text/plain", "bad request\n");
        close(fd);
        return NULL;
    }

    char *q = strchr(path, '?');

    if (q)
        *q = 0;

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {

        serve_index(fd);

    } else if (!strcmp(path, "/status")) {

        serve_status(fd);

    } else {

        bool snap = false;
        char *dot = strstr(path, ".jpg");

        if (dot) {
            *dot = 0;
            snap = true;
        }

        int idx = parse_stream_index(path);

        if (idx < 0 || idx >= MAX_STREAMS) {
            send_simple(fd, "404 Not Found", "text/plain",
                        "try /0 /1 /left /right /status\n");
        } else if (snap) {
            serve_snapshot(fd, idx);
        } else {
            serve_mjpeg(fd, idx);
        }
    }

    close(fd);

    pthread_mutex_lock(&client_lock);
    nclients--;
    pthread_mutex_unlock(&client_lock);

    return NULL;
}

/* ------------------------------------------------------------------ main */

static void on_signal(int s)
{
    (void)s;
    running = 0;
}

static void usage(const char *a0)
{
    printf(
"Usage: %s [options]\n"
"\n"
"  --port N          HTTP port (default 8090)\n"
"  --quality N       JPEG quality 1-100 (default 80)\n"
"  --fps N           max frames per second per client (default 60)\n"
"  --scale N         downscale by N (default 1)\n"
"  --crop X,Y,W,H    crop each frame before encoding\n"
"  --swap            swap streams 0 and 1\n"
"  --flip0 MODE      orientation fix for eye 0: none, v, h or vh\n"
"                    (default none)\n"
"  --flip1 MODE      same for eye 1 (default v - that camera is mounted\n"
"                    upside down)\n"
"  --verbose         explain slot discovery, and log how often each slot\n"
"                    refreshes\n"
"\n"
"Endpoints:  /0 /1 (MJPEG, one eye each)   /left /right (aliases)\n"
"            /2 or /full (newest frame, unclassified - use to tune crops)\n"
"            /0.jpg (single frame)   /status (JSON)   / (preview page)\n"
"\n"
"Put http://<headset-ip>:<port>/0 and .../1 into EyeTrackVR's camera\n"
"address fields.  Eye frames only exist while the headset is being worn.\n"
"\n"
"Needs root: pidfd_getfd() is blocked by kernel.yama.ptrace_scope=1.\n", a0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {

        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc)
            opt_quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            opt_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            opt_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--swap"))
            opt_swap = true;
        else if ((!strcmp(argv[i], "--flip0") || !strcmp(argv[i], "--flip1")) &&
                 i + 1 < argc) {

            int  which = argv[i][6] - '0';
            const char *m = argv[++i];
            int  f = 0;

            if (strchr(m, 'v')) f |= FLIP_V;
            if (strchr(m, 'h')) f |= FLIP_H;

            if (!f && strcmp(m, "none")) {
                fprintf(stderr, "--flip%d wants none, v, h or vh\n", which);
                return 1;
            }

            opt_flip[which] = f;
        }
        else if (!strcmp(argv[i], "--verbose"))
            opt_verbose = true;
        else if (!strcmp(argv[i], "--crop") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d",
                       &opt_cx, &opt_cy, &opt_cw, &opt_ch) != 4) {
                fprintf(stderr, "--crop wants X,Y,W,H\n");
                return 1;
            }
        } else {
            usage(argv[0]);
            return strcmp(argv[i], "--help") ? 1 : 0;
        }
    }

    /* settle the crop and scale once instead of on every frame */
    {
        unsigned cw = opt_cw > 0 ? (unsigned)opt_cw : EYE_STRIDE;
        unsigned ch = opt_ch > 0 ? (unsigned)opt_ch : EYE_HEIGHT;

        out_x = opt_cx > 0 && opt_cx < EYE_STRIDE ? (unsigned)opt_cx : 0;
        out_y = opt_cy > 0 && opt_cy < EYE_HEIGHT ? (unsigned)opt_cy : 0;

        if (out_x + cw > EYE_STRIDE) cw = EYE_STRIDE - out_x;
        if (out_y + ch > EYE_HEIGHT) ch = EYE_HEIGHT - out_y;

        out_scale = opt_scale < 1 ? 1 : (unsigned)opt_scale;
        out_w     = cw / out_scale;
        out_h     = ch / out_scale;

        if (!out_w || !out_h) {
            fprintf(stderr, "--crop / --scale leave nothing to encode\n");
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    cache_setup();

    for (int i = 0; i < MAX_STREAMS; i++) {
        pthread_mutex_init(&streams[i].lock, NULL);
        pthread_cond_init(&streams[i].cv, NULL);
        setup_encoder(&streams[i]);
    }

    /* Something to hand out before any real frame exists. */
    publish_placeholder();

    int srv = socket(AF_INET, SOCK_STREAM, 0);

    if (srv < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return 1;
    }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((uint16_t)opt_port);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "bind(%d): %s\n", opt_port, strerror(errno));
        return 1;
    }

    if (listen(srv, 16) < 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        return 1;
    }

    logmsg("listening on port %d  ->  http://<headset-ip>:%d/0 and /1",
           opt_port, opt_port);

    pthread_t cap;
    pthread_create(&cap, NULL, capture_thread, NULL);

    while (running) {

        /*
         * Poll rather than blocking in accept(), so SIGTERM is noticed straight
         * away instead of hanging until the next connection - systemd should
         * not have to SIGKILL this on shutdown.
         */
        struct pollfd pfd = { .fd = srv, .events = POLLIN };

        int pr = poll(&pfd, 1, 500);

        if (pr <= 0) {
            if (pr < 0 && errno != EINTR)
                break;
            continue;
        }

        struct sockaddr_in ca;
        socklen_t          cl = sizeof(ca);

        int fd = accept(srv, (struct sockaddr *)&ca, &cl);

        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        pthread_mutex_lock(&client_lock);
        bool room = nclients < MAX_CLIENTS;
        if (room)
            nclients++;
        pthread_mutex_unlock(&client_lock);

        if (!room) {
            logmsg("refusing connection: %d clients already", MAX_CLIENTS);
            send_simple(fd, "503 Service Unavailable", "text/plain",
                        "too many clients\n");
            close(fd);
            continue;
        }

        pthread_t t;

        if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)fd) != 0) {
            pthread_mutex_lock(&client_lock);
            nclients--;
            pthread_mutex_unlock(&client_lock);
            close(fd);
        } else {
            pthread_detach(t);
        }
    }

    logmsg("shutting down");

    close(srv);

    /*
     * Only the capture thread is joined; client threads are detached and die
     * with the process.  Waiting on them would stall shutdown behind whatever
     * a stalled peer is doing.
     */
    pthread_join(cap, NULL);
    drop_arenas();

    return 0;
}
