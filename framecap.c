/*
 * framecap - capture camera frames from the Steam Frame headset.
 *
 * XRService owns the V4L2 capture queues for the headset cameras and feeds
 * them buffers it allocated itself through /dev/udmabuf (V4L2_MEMORY_DMABUF).
 * The camera DMA writes straight into those pages, so another process that can
 * borrow the DMA-BUF descriptors sees the live frames.
 *
 * Nothing here is hardcoded to a particular device; it is all discovered:
 *
 *   - XRService is found by scanning /proc for its cmdline.
 *   - The V4L2 nodes and sensor subdevs it holds open come from /proc/<pid>/fd.
 *   - Each node's geometry comes from VIDIOC_G_FMT on our own handle.
 *   - Each node is traced back to its sensor through MEDIA_IOC_G_TOPOLOGY, so
 *     cameras are identified by sensor rather than by device number.
 *   - Buffers are split into queues using the order XRService allocates them:
 *     it opens a sensor subdev, then allocates that camera's buffers, so the
 *     nearest preceding subdev descriptor names the camera a run belongs to.
 *
 * pidfd_getfd() is gated by ptrace_may_access(), and SteamOS ships
 * kernel.yama.ptrace_scope=1, so this must run as root.
 *
 * Build:  gcc -O2 -Wall -o framecap framecap.c
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

#ifndef MEDIA_ENT_F_CAM_SENSOR
#define MEDIA_ENT_F_CAM_SENSOR 0x00020001
#endif

#define MAX_CAMERAS   16
#define MAX_FDENTS  4096
#define MAX_GROUPS    32
#define MAX_RUNBUFS  128
#define MAX_TOPOS      8

#define SENSOR_NAME_LEN 64

/* ------------------------------------------------------------------ types */

typedef struct {
    int      node;                      /* N from /dev/videoN                */
    char     path[64];
    unsigned width;
    unsigned height;
    unsigned bytesperline;
    unsigned nplanes;
    size_t   planesize[VIDEO_MAX_PLANES];
    uint32_t pixfmt;
    char     sensor[SENSOR_NAME_LEN];   /* media entity name of the sensor   */
    const char *role;                   /* eye / world / passthrough / ...   */
} camera_t;

enum fdkind {
    FD_DMABUF,
    FD_SUBDEV_SENSOR,
    FD_VIDEO
};

typedef struct {
    int         xfd;                    /* descriptor number in XRService    */
    enum fdkind kind;
    size_t      size;                   /* FD_DMABUF                         */
    unsigned long ino;                  /* FD_DMABUF                         */
    char        sensor[SENSOR_NAME_LEN];/* FD_SUBDEV_SENSOR                  */
    char        path[64];               /* FD_VIDEO                          */
} fdent_t;

typedef struct {
    int      xfd;
    size_t   size;
} bufref_t;

typedef struct {
    size_t    planesize[2];
    int       nbufs;
    bufref_t  buf[MAX_RUNBUFS];         /* plane 0 descriptors only          */
    char      sensor[SENSOR_NAME_LEN];  /* from the preceding subdev         */
    camera_t *cam;                      /* camera this run belongs to        */
} group_t;

typedef struct {
    struct media_v2_entity    *ents;
    struct media_v2_interface *intfs;
    struct media_v2_pad       *pads;
    struct media_v2_link      *links;
    __u32 nents, nintfs, npads, nlinks;
} topo_t;

/* --------------------------------------------------------------- globals */

static camera_t  cameras[MAX_CAMERAS];
static int       ncameras;

static fdent_t   fdents[MAX_FDENTS];
static int       nfdents;

static group_t   groups[MAX_GROUPS];
static int       ngroups;

static topo_t    topos[MAX_TOPOS];
static int       ntopos;

static pid_t     xr_pid;
static int       xr_pidfd = -1;

static const char *opt_process  = "XRService";
static const char *opt_out      = "frames";
static const char *opt_camera   = "all";
static int         opt_frames   = 1;
static double      opt_timeout  = 5.0;
static bool        opt_list     = false;
static bool        opt_snapshot = false;
static bool        opt_analyze  = false;
static bool        opt_eye      = false;
static int         opt_stride   = 0;
static int         opt_pollhz   = 500;

/* ------------------------------------------------------------- utilities */

static int pidfd_open_sys(pid_t pid)
{
    return (int)syscall(SYS_pidfd_open, pid, 0u);
}

static int pidfd_getfd_sys(int pidfd, int targetfd)
{
    return (int)syscall(SYS_pidfd_getfd, pidfd, targetfd, 0u);
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);

    exit(1);
}

static void fourcc_str(uint32_t f, char out[5])
{
    out[0] = (char)(f & 0xff);
    out[1] = (char)((f >> 8) & 0xff);
    out[2] = (char)((f >> 16) & 0xff);
    out[3] = (char)((f >> 24) & 0xff);
    out[4] = 0;
}

/*
 * Turn "og0ve10 5-0060" into "og0ve10_5-0060" so it can go in a filename.
 */
static void slugify(const char *in, char *out, size_t n)
{
    size_t i = 0;

    for (; in[i] && i + 1 < n; i++)
        out[i] = (in[i] == ' ' || in[i] == '/') ? '_' : in[i];

    out[i] = 0;
}

/* --------------------------------------------------- media graph handling */

static void topo_load_all(void)
{
    for (int mi = 0; mi < MAX_TOPOS; mi++) {

        char mpath[32];
        snprintf(mpath, sizeof(mpath), "/dev/media%d", mi);

        int mfd = open(mpath, O_RDWR);

        if (mfd < 0)
            continue;

        struct media_v2_topology t;
        memset(&t, 0, sizeof(t));

        if (ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &t) < 0) {
            close(mfd);
            continue;
        }

        topo_t *o = &topos[ntopos];
        memset(o, 0, sizeof(*o));

        o->nents  = t.num_entities;
        o->nintfs = t.num_interfaces;
        o->npads  = t.num_pads;
        o->nlinks = t.num_links;

        o->ents  = calloc(o->nents  ? o->nents  : 1, sizeof(*o->ents));
        o->intfs = calloc(o->nintfs ? o->nintfs : 1, sizeof(*o->intfs));
        o->pads  = calloc(o->npads  ? o->npads  : 1, sizeof(*o->pads));
        o->links = calloc(o->nlinks ? o->nlinks : 1, sizeof(*o->links));

        if (!o->ents || !o->intfs || !o->pads || !o->links) {
            free(o->ents); free(o->intfs); free(o->pads); free(o->links);
            close(mfd);
            continue;
        }

        t.ptr_entities   = (__u64)(uintptr_t)o->ents;
        t.ptr_interfaces = (__u64)(uintptr_t)o->intfs;
        t.ptr_pads       = (__u64)(uintptr_t)o->pads;
        t.ptr_links      = (__u64)(uintptr_t)o->links;

        bool ok = ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &t) == 0;
        close(mfd);

        if (!ok) {
            free(o->ents); free(o->intfs); free(o->pads); free(o->links);
            continue;
        }

        ntopos++;
    }
}

static struct media_v2_entity *topo_entity(topo_t *t, __u32 id)
{
    for (__u32 i = 0; i < t->nents; i++)
        if (t->ents[i].id == id)
            return &t->ents[i];

    return NULL;
}

static struct media_v2_pad *topo_pad(topo_t *t, __u32 id)
{
    for (__u32 i = 0; i < t->npads; i++)
        if (t->pads[i].id == id)
            return &t->pads[i];

    return NULL;
}

/*
 * Which entity owns the device node with this major:minor?
 */
static __u32 topo_entity_for_devnode(topo_t *t, dev_t rdev)
{
    __u32 intf_id = 0;

    for (__u32 i = 0; i < t->nintfs; i++)
        if (t->intfs[i].devnode.major == major(rdev) &&
            t->intfs[i].devnode.minor == minor(rdev)) {
            intf_id = t->intfs[i].id;
            break;
        }

    if (!intf_id)
        return 0;

    for (__u32 i = 0; i < t->nlinks; i++)
        if ((t->links[i].flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_INTERFACE_LINK &&
            t->links[i].source_id == intf_id)
            return t->links[i].sink_id;

    return 0;
}

/*
 * Walk upstream from an entity across enabled data links until a sensor is
 * reached.
 *
 * A CSIPHY carries two independent sensors on separate pad pairs, so following
 * "any enabled link into this entity" picks the wrong camera.  Track which
 * source pad we left the entity through and re-enter on its paired sink pad -
 * pads are laid out as (sink, source) couples, so the sink index is one below
 * the source index.  Entities with a single sink pad are unambiguous.
 */
static bool topo_walk_to_sensor(topo_t *t, __u32 ent_id, char *out, size_t outn)
{
    int exit_pad_index = -1;

    for (int hop = 0; hop < 32 && ent_id; hop++) {

        struct media_v2_entity *e = topo_entity(t, ent_id);

        if (!e)
            return false;

        if (e->function == MEDIA_ENT_F_CAM_SENSOR) {
            snprintf(out, outn, "%s", e->name);
            return true;
        }

        /* pick the sink pad that pairs with the source pad we came from */
        __u32 first_sink = 0, paired = 0;
        int   nsinks     = 0;

        for (__u32 p = 0; p < t->npads; p++) {

            if (t->pads[p].entity_id != ent_id)
                continue;

            if (!(t->pads[p].flags & MEDIA_PAD_FL_SINK))
                continue;

            nsinks++;

            if (!first_sink)
                first_sink = t->pads[p].id;

            if (exit_pad_index >= 1 &&
                (int)t->pads[p].index == exit_pad_index - 1)
                paired = t->pads[p].id;
        }

        __u32 sink_pad = (nsinks == 1) ? first_sink
                       : (paired ? paired : first_sink);

        if (!sink_pad)
            return false;

        __u32 src_pad = 0;

        for (__u32 i = 0; i < t->nlinks; i++) {

            if ((t->links[i].flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK)
                continue;

            if (!(t->links[i].flags & MEDIA_LNK_FL_ENABLED))
                continue;

            if (t->links[i].sink_id == sink_pad) {
                src_pad = t->links[i].source_id;
                break;
            }
        }

        if (!src_pad)
            return false;

        struct media_v2_pad *sp = topo_pad(t, src_pad);

        if (!sp)
            return false;

        ent_id         = sp->entity_id;
        exit_pad_index = (int)sp->index;
    }

    return false;
}

/*
 * Sensor feeding a /dev/videoN capture node.
 */
static bool sensor_for_video(dev_t rdev, char *out, size_t outn)
{
    for (int i = 0; i < ntopos; i++) {

        __u32 ent = topo_entity_for_devnode(&topos[i], rdev);

        if (ent && topo_walk_to_sensor(&topos[i], ent, out, outn))
            return true;
    }

    return false;
}

/*
 * If this device node is itself a sensor subdev, return its name.
 */
static bool sensor_for_subdev(dev_t rdev, char *out, size_t outn)
{
    for (int i = 0; i < ntopos; i++) {

        __u32 id = topo_entity_for_devnode(&topos[i], rdev);

        if (!id)
            continue;

        struct media_v2_entity *e = topo_entity(&topos[i], id);

        if (e && e->function == MEDIA_ENT_F_CAM_SENSOR) {
            snprintf(out, outn, "%s", e->name);
            return true;
        }
    }

    return false;
}

/*
 * Sensor model -> what the camera is for.  Only a naming convenience: every
 * camera is listed and selectable whether or not its model is recognised.
 */
static const char *role_for_sensor(const char *sensor)
{
    if (strstr(sensor, "og01a1b"))
        return "tracking";      /* 1056x1024 exterior fisheye  */

    if (strstr(sensor, "og0ve10"))
        return "tracking";      /* 640x480 exterior            */

    if (strstr(sensor, "imx616"))
        return "passthrough";   /* 2464x2464, idle by default  */

    return "unknown";
}

/* ------------------------------------------------- XRService / proc scan */

static pid_t find_process(const char *needle)
{
    DIR *d = opendir("/proc");

    if (!d)
        die("opendir(/proc): %s", strerror(errno));

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

        if (got == 0)
            continue;

        /* cmdline[0] is argv[0]; match on its basename */
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

static bool read_dmabuf_size(pid_t pid, int fd, size_t *size, unsigned long *ino)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", pid, fd);

    FILE *f = fopen(path, "r");

    if (!f)
        return false;

    bool have = false;
    char line[256];

    if (ino)
        *ino = 0;

    while (fgets(line, sizeof(line), f)) {

        unsigned long long v;

        if (sscanf(line, "size: %llu", &v) == 1) {
            *size = (size_t)v;
            have  = true;
        } else if (ino && sscanf(line, "ino: %llu", &v) == 1) {
            *ino = (unsigned long)v;
        }
    }

    fclose(f);

    return have;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/*
 * Walk XRService's descriptor table in numeric order, recording the DMA-BUFs,
 * the capture nodes and the sensor subdevs.  The ordering matters: XRService
 * opens a sensor's subdev and then allocates that camera's queue, so the
 * sequence is what lets a run of buffers be attributed to a camera.
 */
static void scan_xr_fds(void)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", xr_pid);

    DIR *d = opendir(dirpath);

    if (!d)
        die("opendir(%s): %s  (are you root?)", dirpath, strerror(errno));

    static int fds[8192];
    int        nfds = 0;
    struct dirent *e;

    while ((e = readdir(d)) && nfds < (int)(sizeof(fds) / sizeof(fds[0]))) {

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;

        fds[nfds++] = atoi(e->d_name);
    }

    closedir(d);

    qsort(fds, nfds, sizeof(int), cmp_int);

    for (int i = 0; i < nfds && nfdents < MAX_FDENTS; i++) {

        char link[64], target[256];
        snprintf(link, sizeof(link), "/proc/%d/fd/%d", xr_pid, fds[i]);

        ssize_t n = readlink(link, target, sizeof(target) - 1);

        if (n < 0)
            continue;

        target[n] = 0;

        fdent_t ent;
        memset(&ent, 0, sizeof(ent));
        ent.xfd = fds[i];

        if (strstr(target, "dmabuf")) {

            if (!read_dmabuf_size(xr_pid, fds[i], &ent.size, &ent.ino))
                continue;

            ent.kind = FD_DMABUF;

        } else if (strncmp(target, "/dev/video", 10) == 0) {

            ent.kind = FD_VIDEO;
            snprintf(ent.path, sizeof(ent.path), "%s", target);

        } else if (strncmp(target, "/dev/v4l-subdev", 15) == 0) {

            struct stat st;

            if (stat(target, &st) < 0)
                continue;

            if (!sensor_for_subdev(st.st_rdev, ent.sensor, sizeof(ent.sensor)))
                continue;   /* not a sensor subdev - not a useful marker */

            ent.kind = FD_SUBDEV_SENSOR;

        } else {
            continue;
        }

        fdents[nfdents++] = ent;
    }
}

/* ------------------------------------------------------ camera discovery */

static void probe_cameras(void)
{
    int seen[64];
    int nseen = 0;

    for (int i = 0; i < nfdents; i++) {

        if (fdents[i].kind != FD_VIDEO)
            continue;

        const char *path = fdents[i].path;
        int         node = atoi(path + 10);
        bool        dup  = false;

        for (int k = 0; k < nseen; k++)
            if (seen[k] == node)
                dup = true;

        if (dup || ncameras >= MAX_CAMERAS)
            continue;

        seen[nseen++] = node;

        int fd = open(path, O_RDWR);

        if (fd < 0)
            continue;

        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        camera_t *c = &cameras[ncameras];
        memset(c, 0, sizeof(*c));

        if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {

            c->width        = fmt.fmt.pix_mp.width;
            c->height       = fmt.fmt.pix_mp.height;
            c->pixfmt       = fmt.fmt.pix_mp.pixelformat;
            c->nplanes      = fmt.fmt.pix_mp.num_planes;
            c->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

            for (unsigned p = 0; p < c->nplanes && p < VIDEO_MAX_PLANES; p++)
                c->planesize[p] = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;

        } else {

            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
                close(fd);
                continue;
            }

            c->width        = fmt.fmt.pix.width;
            c->height       = fmt.fmt.pix.height;
            c->pixfmt       = fmt.fmt.pix.pixelformat;
            c->nplanes      = 1;
            c->bytesperline = fmt.fmt.pix.bytesperline;
            c->planesize[0] = fmt.fmt.pix.sizeimage;
        }

        struct stat st;

        if (fstat(fd, &st) == 0)
            sensor_for_video(st.st_rdev, c->sensor, sizeof(c->sensor));

        close(fd);

        if (!c->sensor[0])
            snprintf(c->sensor, sizeof(c->sensor), "unknown");

        c->node = node;
        snprintf(c->path, sizeof(c->path), "%s", path);
        c->role = role_for_sensor(c->sensor);

        ncameras++;
    }
}

/*
 * qcom-camss reports bytesperline as the visible width, but the VFE writes with
 * a larger aligned pitch: the og01a1b nodes claim 1056 while plane 0 is really
 * 1152 bytes per row.  sizeimage is correct, so recover the pitch from it.
 */
static unsigned camera_stride(const camera_t *c)
{
    if (!c->height || !c->planesize[0])
        return c->bytesperline ? c->bytesperline : c->width;

    double bpp = 1.0;

    if (c->pixfmt == V4L2_PIX_FMT_NV12 || c->pixfmt == V4L2_PIX_FMT_NV21)
        bpp = 1.5;

    unsigned s = (unsigned)((double)c->planesize[0] / ((double)c->height * bpp));

    if (s >= c->width && s <= c->width * 4)
        return s;

    return c->bytesperline ? c->bytesperline : c->width;
}

/* ------------------------------------------------------- buffer grouping */

/*
 * XRService allocates one udmabuf per plane, plane 0 immediately followed by
 * plane 1, and allocates a whole queue in one go right after opening the
 * sensor's subdev.  So: sweep the descriptor table in order, remember the last
 * sensor subdev seen, and pair up consecutive DMA-BUFs whose sizes match a
 * camera's plane sizes.  A run ends when the sensor changes or the plane sizes
 * change.
 *
 * Plane 1's size matches VIDIOC_G_FMT exactly; plane 0 is allocated with slack
 * (the eye cameras report 307200 but get a 462848-byte buffer), so plane 0 is
 * matched with >= rather than ==.
 */
static void build_groups(void)
{
    char current_sensor[SENSOR_NAME_LEN] = "";

    for (int i = 0; i < nfdents; i++) {

        if (fdents[i].kind == FD_SUBDEV_SENSOR) {
            snprintf(current_sensor, sizeof(current_sensor), "%s",
                     fdents[i].sensor);
            continue;
        }

        if (fdents[i].kind != FD_DMABUF)
            continue;

        /* need the next descriptor to also be a DMA-BUF: the plane 1 buffer */
        if (i + 1 >= nfdents || fdents[i + 1].kind != FD_DMABUF)
            continue;

        size_t s0 = fdents[i].size;
        size_t s1 = fdents[i + 1].size;

        bool match = false;

        for (int c = 0; c < ncameras; c++) {

            camera_t *cam = &cameras[c];

            if (cam->nplanes >= 2 &&
                s1 == cam->planesize[1] &&
                s0 >= cam->planesize[0]) {
                match = true;
                break;
            }
        }

        if (!match)
            continue;

        group_t *g = NULL;

        if (ngroups > 0) {

            group_t *last = &groups[ngroups - 1];

            if (last->planesize[0] == s0 &&
                last->planesize[1] == s1 &&
                !strcmp(last->sensor, current_sensor))
                g = last;
        }

        if (!g) {

            if (ngroups >= MAX_GROUPS)
                break;

            g = &groups[ngroups++];
            memset(g, 0, sizeof(*g));
            g->planesize[0] = s0;
            g->planesize[1] = s1;
            snprintf(g->sensor, sizeof(g->sensor), "%s", current_sensor);
        }

        if (g->nbufs < MAX_RUNBUFS) {
            g->buf[g->nbufs].xfd  = fdents[i].xfd;
            g->buf[g->nbufs].size = s0;
            g->nbufs++;
        }

        i++;    /* consume the plane 1 descriptor */
    }

    /* Drop runs too short to be a real V4L2 queue. */
    int keep = 0;

    for (int i = 0; i < ngroups; i++)
        if (groups[i].nbufs >= 4)
            groups[keep++] = groups[i];

    ngroups = keep;

    /*
     * Bind each run to the camera fed by the same sensor.  Falling back to a
     * format match keeps things working if the subdev marker is missing.
     */
    for (int i = 0; i < ngroups; i++) {

        group_t *g = &groups[i];

        for (int c = 0; c < ncameras && !g->cam; c++)
            if (g->sensor[0] && !strcmp(cameras[c].sensor, g->sensor))
                g->cam = &cameras[c];

        for (int c = 0; c < ncameras && !g->cam; c++) {

            camera_t *cam = &cameras[c];

            if (cam->nplanes < 2)
                continue;

            if (g->planesize[1] != cam->planesize[1] ||
                g->planesize[0] <  cam->planesize[0])
                continue;

            bool taken = false;

            for (int k = 0; k < ngroups; k++)
                if (k != i && groups[k].cam == cam)
                    taken = true;

            if (!taken)
                g->cam = cam;
        }
    }
}

/* ---------------------------------------------------------- frame output */

static int write_pgm(const char *path,
                     const unsigned char *data,
                     unsigned w,
                     unsigned h,
                     unsigned stride)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "  fopen(%s): %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(f, "P5\n%u %u\n255\n", w, h);

    for (unsigned y = 0; y < h; y++)
        if (fwrite(data + (size_t)y * stride, 1, w, f) != w) {
            fclose(f);
            return -1;
        }

    fclose(f);

    return 0;
}

/*
 * Cheap content fingerprint, used to notice when the camera has refilled a
 * buffer.  Sampling beats hashing whole frames at 500 Hz.
 */
static uint64_t fingerprint(const unsigned char *p, size_t len)
{
    uint64_t h    = 1469598103934665603ULL;
    size_t   step = len / 512;

    if (step == 0)
        step = 1;

    for (size_t i = 0; i < len; i += step) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }

    return h;
}

/* ------------------------------------------------------------- reporting */

static void print_discovery(void)
{
    int ndmabuf = 0;

    for (int i = 0; i < nfdents; i++)
        if (fdents[i].kind == FD_DMABUF)
            ndmabuf++;

    printf("XRService pid %d\n\n", xr_pid);

    printf("Cameras held open by XRService:\n");

    for (int i = 0; i < ncameras; i++) {

        camera_t *c = &cameras[i];
        char fcc[5];

        fourcc_str(c->pixfmt, fcc);

        printf("  [%d] %-12s %-16s %4ux%-4u %s  pitch %4u  planes",
               i, c->path, c->sensor, c->width, c->height, fcc,
               camera_stride(c));

        for (unsigned p = 0; p < c->nplanes; p++)
            printf(" %zu", c->planesize[p]);

        printf("   role=%s\n", c->role);
    }

    printf("\nDMA-BUF queues in XRService (%d DMA-BUFs total):\n", ndmabuf);

    for (int i = 0; i < ngroups; i++) {

        group_t *g = &groups[i];

        printf("  [%d] %2d buffers  plane0=%-8zu plane1=%-7zu fds %d..%d",
               i, g->nbufs, g->planesize[0], g->planesize[1],
               g->buf[0].xfd, g->buf[g->nbufs - 1].xfd);

        if (g->cam)
            printf("  -> %-11s %-16s %s (%ux%u)",
                   g->cam->role, g->sensor, g->cam->path,
                   g->cam->width, g->cam->height);
        else
            printf("  -> unmatched (sensor '%s')", g->sensor);

        printf("\n");
    }

    printf("\n");
}

/* ---------------------------------------------------------- stride finder */

/*
 * If a buffer's real pitch differs from bytesperline the image comes out
 * sheared.  The true pitch minimises the difference between vertically
 * adjacent pixels, so score them all and report the best few.
 */
static void analyze_stride(const unsigned char *p, size_t len)
{
    struct { unsigned stride; double score; } top[6];
    int ntop = 0;

    printf("  stride analysis over %zu bytes:\n", len);

    for (unsigned stride = 256; stride <= 4096; stride += 2) {

        unsigned rows = (unsigned)(len / stride);

        if (rows < 64)
            break;

        double sum = 0;
        int    n   = 0;

        for (unsigned y = 0; y + 1 < rows && y < 512; y += 3) {

            const unsigned char *a = p + (size_t)y * stride;
            const unsigned char *b = a + stride;

            for (unsigned x = 0; x < 256 && x < stride; x += 4) {
                sum += abs((int)a[x] - (int)b[x]);
                n++;
            }
        }

        if (n == 0)
            continue;

        double score = sum / n;

        if (ntop < 6) {
            top[ntop].stride = stride;
            top[ntop].score  = score;
            ntop++;
        } else {
            int worst = 0;
            for (int k = 1; k < 6; k++)
                if (top[k].score > top[worst].score)
                    worst = k;
            if (score < top[worst].score) {
                top[worst].stride = stride;
                top[worst].score  = score;
            }
        }
    }

    for (int i = 0; i < ntop; i++)
        for (int j = i + 1; j < ntop; j++)
            if (top[j].score < top[i].score) {
                unsigned s = top[i].stride;
                double   v = top[i].score;
                top[i].stride = top[j].stride;
                top[i].score  = top[j].score;
                top[j].stride = s;
                top[j].score  = v;
            }

    for (int i = 0; i < ntop; i++)
        printf("    stride %4u  score %6.2f  (%u rows)\n",
               top[i].stride, top[i].score,
               (unsigned)(len / top[i].stride));
}

/* ------------------------------------------------------------- capturing */

static bool group_selected(const group_t *g, int index)
{
    char idx[16];

    if (!strcmp(opt_camera, "all"))
        return true;

    snprintf(idx, sizeof(idx), "%d", index);

    /*
     * A bare number selects a queue by index and nothing else - every sensor
     * name contains digits, so falling through to the substring match below
     * would make "--camera 0" select all of them.
     */
    if (strspn(opt_camera, "0123456789") == strlen(opt_camera))
        return !strcmp(opt_camera, idx);

    if (g->sensor[0] && strstr(g->sensor, opt_camera))
        return true;

    if (!g->cam)
        return false;

    if (!strcmp(opt_camera, g->cam->role))
        return true;

    if (!strcmp(opt_camera, g->cam->path))
        return true;

    return false;
}

static void capture_group(group_t *g, int index)
{
    camera_t *cam = g->cam;

    if (!cam) {
        fprintf(stderr, "queue %d has no camera binding, skipping\n", index);
        return;
    }

    unsigned stride = opt_stride ? (unsigned)opt_stride : camera_stride(cam);
    unsigned w      = cam->width;
    unsigned h      = cam->height;

    if (stride == 0)
        stride = w;

    if (w > stride)
        w = stride;

    size_t need = (size_t)stride * h;

    char slug[SENSOR_NAME_LEN];
    slugify(g->sensor[0] ? g->sensor : cam->sensor, slug, sizeof(slug));

    printf("Queue %d: %s %s via %s  %ux%u stride %u  (%d buffers)\n",
           index, cam->role, slug, cam->path, w, h, stride, g->nbufs);

    unsigned char *maps[MAX_RUNBUFS];
    int            fds [MAX_RUNBUFS];
    uint64_t       fp  [MAX_RUNBUFS];
    int            mapped = 0;

    for (int i = 0; i < MAX_RUNBUFS; i++) {
        maps[i] = NULL;
        fds[i]  = -1;
        fp[i]   = 0;
    }

    for (int i = 0; i < g->nbufs; i++) {

        if (g->buf[i].size < need) {
            fprintf(stderr, "  buffer %d is %zu bytes, need %zu - skipping\n",
                    i, g->buf[i].size, need);
            continue;
        }

        fds[i] = pidfd_getfd_sys(xr_pidfd, g->buf[i].xfd);

        if (fds[i] < 0) {
            fprintf(stderr, "  pidfd_getfd(%d): %s\n",
                    g->buf[i].xfd, strerror(errno));
            continue;
        }

        void *m = mmap(NULL, g->buf[i].size, PROT_READ, MAP_SHARED, fds[i], 0);

        if (m == MAP_FAILED) {
            fprintf(stderr, "  mmap(fd %d): %s\n",
                    g->buf[i].xfd, strerror(errno));
            close(fds[i]);
            fds[i] = -1;
            continue;
        }

        maps[i] = m;
        fp[i]   = fingerprint(m, need);
        mapped++;
    }

    printf("  mapped %d/%d buffers\n", mapped, g->nbufs);

    if (mapped == 0)
        return;

    if (opt_analyze)
        for (int i = 0; i < g->nbufs; i++)
            if (maps[i]) {
                analyze_stride(maps[i], g->buf[i].size);
                break;
            }

    int written = 0;

    if (opt_snapshot) {

        /*
         * Dump the queue as it stands.  While the cameras are paused this is
         * the last N frames the sensor captured, still sitting in memory.
         */
        for (int i = 0; i < g->nbufs; i++) {

            if (!maps[i])
                continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s_%s_buf%02d.pgm",
                     opt_out, cam->role, slug, i);

            if (write_pgm(path, maps[i], w, h, stride) == 0) {
                printf("  %s\n", path);
                written++;
            }
        }

    } else {

        /*
         * Live mode: the camera cycles through the queue, so a buffer whose
         * contents changed since the last poll has just been filled.
         */
        double deadline = now_sec() + opt_timeout;
        long   interval = 1000000L / (opt_pollhz > 0 ? opt_pollhz : 500);

        while (written < opt_frames && now_sec() < deadline) {

            for (int i = 0; i < g->nbufs && written < opt_frames; i++) {

                if (!maps[i])
                    continue;

                uint64_t cur = fingerprint(maps[i], need);

                if (cur == fp[i])
                    continue;

                fp[i] = cur;

                char path[512];
                snprintf(path, sizeof(path), "%s/%s_%s_%04d.pgm",
                         opt_out, cam->role, slug, written);

                if (write_pgm(path, maps[i], w, h, stride) == 0) {
                    printf("  %s   (buffer %d)\n", path, i);
                    written++;
                    deadline = now_sec() + opt_timeout;
                }
            }

            usleep(interval);
        }

        if (written == 0)
            printf("  no new frames in %.1fs - this camera is not streaming.\n"
                   "  Use --snapshot to dump the frames still left in the queue.\n",
                   opt_timeout);
    }

    for (int i = 0; i < g->nbufs; i++) {
        if (maps[i])
            munmap(maps[i], g->buf[i].size);
        if (fds[i] >= 0)
            close(fds[i]);
    }

    printf("  wrote %d frame(s)\n\n", written);
}

/* ------------------------------------------------------- eye camera path */

/*
 * The eye cameras are not exposed as V4L2 nodes.  Their frames are delivered
 * into a large udmabuf that XRService shares with the `eyetracking` helper,
 * which hands them to the Hexagon DSP for gaze estimation ("CGazeEstimatorCdsp"
 * in eyetracking.txt).  Frames appear there as 8-bit greyscale, 512 bytes per
 * row, 400 rows, one frame per 256 KiB slot, carrying both eyes side by side.
 *
 * The slot offsets are sub-allocations inside a shared heap, so they are not
 * fixed: they are located by looking for regions that both change over time and
 * score as image-like (low difference between vertically adjacent pixels).
 */

#define EYE_STRIDE  512
#define EYE_HEIGHT  400
#define EYE_SLOT    262144
#define EYE_BLK     65536
#define MAX_ARENAS  8
#define MAX_SLOTS   64

typedef struct {
    unsigned long   ino;
    size_t          size;
    unsigned char  *map;
    int             fd;
} arena_t;

/*
 * Mean absolute difference between vertically adjacent pixels.  Real imagery
 * scores low; random heap content and packed structures score high.
 */
static double image_score(const unsigned char *p, unsigned *mean_out)
{
    double sum  = 0;
    long   n    = 0;
    double bright = 0;

    for (unsigned y = 0; y + 1 < EYE_HEIGHT; y += 4) {

        const unsigned char *a = p + (size_t)y * EYE_STRIDE;
        const unsigned char *b = a + EYE_STRIDE;

        for (unsigned x = 0; x < EYE_STRIDE; x += 4) {
            sum    += abs((int)a[x] - (int)b[x]);
            bright += a[x];
            n++;
        }
    }

    if (n == 0)
        return 1e9;

    if (mean_out)
        *mean_out = (unsigned)(bright / n);

    return sum / n;
}

static void capture_eye(void)
{
    /* Inodes of DMA-BUFs the eyetracking helper also holds, if it is running. */
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

            char link[80], target[256];
            snprintf(link, sizeof(link), "/proc/%d/fd/%s", et, e->d_name);

            ssize_t n = readlink(link, target, sizeof(target) - 1);

            if (n < 0)
                continue;

            target[n] = 0;

            if (!strstr(target, "dmabuf"))
                continue;

            size_t        sz;
            unsigned long ino;

            if (!read_dmabuf_size(et, atoi(e->d_name), &sz, &ino))
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

    printf("eyetracking pid %d, %d shared DMA-BUF(s)\n\n",
           et, nshared);

    /* Map XRService's copies of those buffers (or any large udmabuf). */
    arena_t arenas[MAX_ARENAS];
    int     narenas = 0;

    for (int i = 0; i < nfdents && narenas < MAX_ARENAS; i++) {

        if (fdents[i].kind != FD_DMABUF)
            continue;

        bool want = false;

        for (int k = 0; k < nshared; k++)
            if (shared[k] == fdents[i].ino)
                want = true;

        if (!nshared && fdents[i].size >= 8u * 1024 * 1024)
            want = true;           /* fallback: any big heap */

        if (!want)
            continue;

        bool dup = false;

        for (int k = 0; k < narenas; k++)
            if (arenas[k].ino == fdents[i].ino)
                dup = true;

        if (dup)
            continue;

        int fd = pidfd_getfd_sys(xr_pidfd, fdents[i].xfd);

        if (fd < 0)
            continue;

        void *m = mmap(NULL, fdents[i].size, PROT_READ, MAP_SHARED, fd, 0);

        if (m == MAP_FAILED) {
            close(fd);
            continue;
        }

        arenas[narenas].ino  = fdents[i].ino;
        arenas[narenas].size = fdents[i].size;
        arenas[narenas].map  = m;
        arenas[narenas].fd   = fd;
        narenas++;
    }

    if (!narenas) {
        fprintf(stderr, "no shared arena found - is SteamVR running?\n");
        return;
    }

    for (int i = 0; i < narenas; i++)
        printf("arena inode %lu, %zu bytes\n", arenas[i].ino, arenas[i].size);

    /*
     * Phase 1: find 64 KiB blocks that change, so the (much more expensive)
     * image test only runs over live regions.
     */
    printf("\nscanning for active regions...\n");

    static unsigned char changed[MAX_ARENAS][1024];
    static uint64_t      prev[MAX_ARENAS][1024];

    for (int a = 0; a < narenas; a++)
        for (size_t o = 0, b = 0; o < arenas[a].size && b < 1024; o += EYE_BLK, b++) {
            prev[a][b]    = fingerprint(arenas[a].map + o, EYE_BLK);
            changed[a][b] = 0;
        }

    double t0 = now_sec();

    while (now_sec() - t0 < 2.0) {

        for (int a = 0; a < narenas; a++)
            for (size_t o = 0, b = 0; o < arenas[a].size && b < 1024; o += EYE_BLK, b++) {
                uint64_t h = fingerprint(arenas[a].map + o, EYE_BLK);
                if (h != prev[a][b]) {
                    changed[a][b] = 1;
                    prev[a][b]    = h;
                }
            }

        usleep(20000);
    }

    /*
     * Phase 2: inside the live regions, look for frame-shaped imagery.
     */
    struct { int arena; size_t off; double score; } slots[MAX_SLOTS];
    int nslots = 0;

    for (int a = 0; a < narenas && nslots < MAX_SLOTS; a++) {

        size_t need = (size_t)EYE_STRIDE * EYE_HEIGHT;

        for (size_t off = 0; off + need <= arenas[a].size; off += 4096) {

            size_t blk = off / EYE_BLK;

            if (blk >= 1024 || !changed[a][blk])
                continue;

            unsigned mean;
            double   sc = image_score(arenas[a].map + off, &mean);

            if (sc > 10.0 || mean < 8)
                continue;

            /* keep one entry per 256 KiB slot */
            bool dup = false;

            for (int k = 0; k < nslots; k++)
                if (slots[k].arena == a &&
                    (slots[k].off / EYE_SLOT) == (off / EYE_SLOT))
                    dup = true;

            if (dup)
                continue;

            if (nslots < MAX_SLOTS) {
                slots[nslots].arena = a;
                slots[nslots].off   = off;
                slots[nslots].score = sc;
                nslots++;
            }
        }
    }

    printf("found %d candidate eye-frame slot(s)\n\n", nslots);

    if (!nslots) {
        printf("No eye frames found.  Eye tracking only runs while the headset\n"
               "is being worn - cover the proximity sensor and retry.\n");
        goto out;
    }

    if (mkdir(opt_out, 0755) < 0 && errno != EEXIST)
        die("mkdir(%s): %s", opt_out, strerror(errno));

    /* Phase 3: emit frames as they are refilled. */
    uint64_t fp[MAX_SLOTS];

    for (int i = 0; i < nslots; i++)
        fp[i] = fingerprint(arenas[slots[i].arena].map + slots[i].off,
                            (size_t)EYE_STRIDE * EYE_HEIGHT);

    int    written  = 0;
    double deadline = now_sec() + opt_timeout;

    if (opt_snapshot) {

        for (int i = 0; i < nslots; i++) {

            char path[512];
            snprintf(path, sizeof(path), "%s/eye_slot%02d.pgm", opt_out, i);

            if (write_pgm(path, arenas[slots[i].arena].map + slots[i].off,
                          EYE_STRIDE, EYE_HEIGHT, EYE_STRIDE) == 0) {
                printf("  %s   (score %.1f)\n", path, slots[i].score);
                written++;
            }
        }

    } else {

        while (written < opt_frames && now_sec() < deadline) {

            for (int i = 0; i < nslots && written < opt_frames; i++) {

                const unsigned char *p = arenas[slots[i].arena].map + slots[i].off;
                uint64_t cur = fingerprint(p, (size_t)EYE_STRIDE * EYE_HEIGHT);

                if (cur == fp[i])
                    continue;

                fp[i] = cur;

                char path[512];
                snprintf(path, sizeof(path), "%s/eye_%04d.pgm", opt_out, written);

                if (write_pgm(path, p, EYE_STRIDE, EYE_HEIGHT, EYE_STRIDE) == 0) {
                    printf("  %s   (slot %d)\n", path, i);
                    written++;
                    deadline = now_sec() + opt_timeout;
                }
            }

            usleep(2000);
        }

        if (!written)
            printf("  slots found but nothing refreshed - the headset is\n"
                   "  probably no longer being worn.  Try --snapshot.\n");
    }

    printf("\nwrote %d eye frame(s)\n", written);

out:
    for (int i = 0; i < narenas; i++) {
        munmap(arenas[i].map, arenas[i].size);
        close(arenas[i].fd);
    }
}

/* ------------------------------------------------------------------ main */

static void usage(const char *argv0)
{
    printf(
"Usage: %s [options]\n"
"\n"
"  --list            discover cameras and buffer queues, then exit\n"
"  --camera SPEC     which camera(s): a role (eye, world, passthrough), a\n"
"                    sensor name substring such as 5-0060, /dev/videoN, a\n"
"                    queue index, or 'all'.  Default: eye\n"
"  --out DIR         output directory (default: frames)\n"
"  --frames N        frames to capture per camera (default: 1)\n"
"  --timeout SEC     how long to wait for a new frame (default: 5)\n"
"  --snapshot        dump every buffer in the queue instead of waiting for\n"
"                    fresh frames - useful while the cameras are paused\n"
"  --stride N        override bytes-per-line\n"
"  --analyze         estimate the real pitch from buffer content\n"
"  --poll-hz N       buffer polling rate (default: 500)\n"
"  --process NAME    process owning the queues (default: XRService)\n"
"\n"
"Needs root: pidfd_getfd() is blocked by kernel.yama.ptrace_scope=1.\n",
        argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {

        if (!strcmp(argv[i], "--list"))
            opt_list = true;
        else if (!strcmp(argv[i], "--snapshot"))
            opt_snapshot = true;
        else if (!strcmp(argv[i], "--analyze"))
            opt_analyze = true;
        else if (!strcmp(argv[i], "--eye"))
            opt_eye = true;
        else if (!strcmp(argv[i], "--camera") && i + 1 < argc)
            opt_camera = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            opt_out = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 1 < argc)
            opt_process = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            opt_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            opt_timeout = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
            opt_stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--poll-hz") && i + 1 < argc)
            opt_pollhz = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return strcmp(argv[i], "--help") ? 1 : 0;
        }
    }

    xr_pid = find_process(opt_process);

    if (!xr_pid)
        die("%s is not running - start SteamVR on the headset first.",
            opt_process);

    topo_load_all();
    scan_xr_fds();
    probe_cameras();
    build_groups();

    print_discovery();

    if (opt_list)
        return 0;

    xr_pidfd = pidfd_open_sys(xr_pid);

    if (xr_pidfd < 0)
        die("pidfd_open(%d): %s", xr_pid, strerror(errno));

    if (opt_eye) {
        capture_eye();
        close(xr_pidfd);
        return 0;
    }

    if (mkdir(opt_out, 0755) < 0 && errno != EEXIST)
        die("mkdir(%s): %s", opt_out, strerror(errno));

    int matched = 0;

    for (int i = 0; i < ngroups; i++)
        if (group_selected(&groups[i], i)) {
            capture_group(&groups[i], i);
            matched++;
        }

    if (!matched)
        fprintf(stderr, "no camera matched '%s' - run with --list\n", opt_camera);

    close(xr_pidfd);

    return matched ? 0 : 1;
}
