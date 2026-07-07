/*
 * vdif_recorder.c -- no-drop VDIF packet recorder for the mmBVEX RFSoC backend.
 *
 * Captures the single 100GbE UDP VDIF stream (128,000 frames/s, 8032-byte VDIF
 * frames, ~8.3 Gbps) off a Mellanox ConnectX-5 with an AF_PACKET v3 (TPACKET_V3)
 * mmap ring, strips the 42-byte Eth/IP/UDP wrapper, and writes the pure 8032-byte
 * VDIF frames back-to-back as flat .vdif files -- directly DiFX-ingestible
 * (format=VDIF/8032/2). Designed for an UNATTENDED BALLOON payload: robust,
 * simple, lightweight machine-readable telemetry.
 *
 * Architecture (see CURRENT_STATUS.md / the plan):
 *   capture thread  -- walks the kernel ring, validates+strips each frame, packs
 *                      frames into page-aligned "slabs", does frame#-gap accounting,
 *                      cuts files on VDIF-second boundaries. Never blocks on disk.
 *   writer thread   -- pulls full slabs, writes them O_DIRECT to the current NVMe,
 *                      rolls files every N seconds, rolls disk when one fills,
 *                      finalizes byte-exact files + JSON sidecars.
 *   main/status     -- ~1 Hz: snapshots atomics -> atomic-rewrites status.json +
 *                      prints one concise console line.
 *
 * THE ALIGNMENT IDENTITY that makes O_DIRECT trivial:
 *   8032 * 128 = 1,028,096 = 251 * 4096  ->  128 VDIF frames == exactly 251 pages.
 *   Keep the slab a multiple of 128 frames and every write is 4 KiB-aligned.
 *   Default slab = 1024 frames = 8,224,768 B (= 2008 * 4096).
 *
 * Build (ON the target, for -march=native):  make           (see Makefile)
 * Run  (root, for AF_PACKET + O_DIRECT):
 *   sudo ./vdif_recorder --iface enp1s0f0np0 --dport 4001 \
 *        --disks /mnt/vlbi0,/mnt/vlbi1,/mnt/vlbi2 --secs-per-file 10 \
 *        --status /tmp/vdif_recorder_status.json
 * Normally launched by record_supervisor.py (which also brings the board up).
 *
 * Linux-only (AF_PACKET / TPACKET_V3 / O_DIRECT). Will not compile on macOS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/filter.h>

/* ----------------------------- AF_XDP backend (optional) --------------------------- */
/* Built only when HAVE_XDP is defined (Makefile links libxdp + bundled libbpf). The
 * AF_XDP zero-copy multi-buffer path bypasses the kernel skb/socket RX entirely: the
 * mlx5 NIC DMAs frames straight into a userspace UMEM, so one pinned core sustains
 * 128k fps for our 8 KB jumbo frames (the standard kernel path tops out at ~half).
 * On x86-64 (4 KiB pages) a UMEM chunk is capped at PAGE_SIZE, so an 8074-B wire frame
 * arrives as 2-3 chained fragments (XDP_PKT_CONTD) that we reassemble.                */
#ifdef HAVE_XDP
#include <linux/if_link.h>           /* XDP_FLAGS_DRV_MODE / XDP_FLAGS_SKB_MODE       */
#include <linux/if_xdp.h>            /* libxdp ships a modern copy (has SG/CONTD)     */
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <bpf/libbpf.h>              /* bpf_object__*, bpf_xdp_attach/detach          */
#include <bpf/bpf.h>                 /* bpf_map_update_elem                           */
/* Defensive fallbacks if an older UAPI gets picked up (the libxdp-bundled header has
 * these; stable kernel 6.6+ bit values). */
#ifndef XDP_USE_SG
#define XDP_USE_SG (1 << 4)          /* bind flag: accept multi-buffer frames        */
#endif
#ifndef XDP_PKT_CONTD
#define XDP_PKT_CONTD (1 << 0)       /* xdp_desc.options: more fragments follow      */
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
#endif /* HAVE_XDP */

/* ----------------------------- VDIF / stream constants ----------------------------- */
#define VDIF_FRAME_BYTES   8032u     /* 32-B VDIF header + 8000-B data array            */
#define WIRE_FRAME_BYTES   8074u     /* Eth(14)+IP(20)+UDP(8)+VDIF(8032)                */
#define FRAMES_PER_SECOND  128000u   /* the framer is a fixed divide-by-2000            */
#define ALIGN_FRAMES       128u      /* 128 frames == 251 pages (O_DIRECT invariant)    */
#define PAGE               4096u

/* ----------------------------- configuration (argv) -------------------------------- */
#define MAX_DISKS 8
static char     g_iface[IFNAMSIZ] = "enp1s0f0np0";
static int      g_dport           = 4001;            /* VDIF UDP dest port              */
static char    *g_disks[MAX_DISKS];
static int      g_ndisks          = 0;
static int      g_secs_per_file   = 10;              /* file roll-over period (seconds) */
static char     g_status_path[1024] = "/tmp/vdif_recorder_status.json";
static double   g_min_free_gb     = 12.0;            /* switch disk when free < this    */
static int      g_use_odirect     = 1;               /* O_DIRECT default; --buffered off */
static int      g_frames_per_slab = 1024;            /* must be a multiple of 128       */
static int      g_nslabs          = 128;             /* slab pool depth (~1 s of slack) */
static int      g_block_mb        = 4;               /* TPACKET_V3 block size (MiB)      */
static int      g_nblocks         = 256;             /* TPACKET_V3 block count (->ring)  */
static int      g_cap_core        = -1;              /* pin capture thread (-1 = none)  */
static int      g_wr_core         = -1;              /* pin writer  thread (-1 = none)  */
static long     g_run_secs        = 0;               /* stop after N s (0 = forever)    */
static int      g_align_start     = 1;               /* wait for frame#==0 to start     */
static int      g_rx_mode         = 0;   /* 0=udp recvmmsg (default), 1=raw recvmmsg, 2=tpacket ring */
static int      g_capture_fd      = -1;  /* the capture socket (for PACKET_STATISTICS)      */
static int      g_rcvbuf_mb       = 32;  /* SO_RCVBUF: keep SMALL so held skbs don't starve  */
                                         /* the mlx5 RX page pool (-> rx_out_of_buffer)       */
static int      g_busy_poll_us    = 100; /* SO_BUSY_POLL: spin draining the NIC queue (no    */
                                         /* wakeup latency) -> breaks the per-flow rx ceiling */
/* --rx xdp (AF_XDP) options */
static int      g_xdp_queue       = 0;   /* NIC RX queue to bind the XSK to (combined 1 -> 0) */
static int      g_xdp_zerocopy    = 0;   /* COPY default (this CX-5 has no ZC multi-buffer);  */
                                         /* --xdp-zc tries ZEROCOPY first on capable NICs     */
static int      g_xdp_drv         = 1;   /* native (DRV) XDP mode; 0 -> generic/SKB (veth)    */
static int      g_xdp_busy        = 0;   /* 0=IRQ-driven (NAPI on IRQ core, processing on     */
                                         /* cap-core: zero-gap on this CX-5); 1=busy-poll     */

/* ----------------------------- global state / signals ------------------------------ */
static volatile sig_atomic_t g_stop = 0;             /* set by signal / disk-full / run */
static int      g_slab_bytes = 0;                    /* g_frames_per_slab * 8032        */
static uint8_t *g_arena = NULL;                      /* slab pool                       */

enum rec_state { ST_RUNNING = 0, ST_STOPPED_DISK_FULL, ST_STOPPED_SIGNAL, ST_ERROR };
static const char *state_name(int s) {
    switch (s) {
        case ST_RUNNING:             return "RUNNING";
        case ST_STOPPED_DISK_FULL:   return "STOPPED_DISK_FULL";
        case ST_STOPPED_SIGNAL:      return "STOPPED_SIGNAL";
        default:                     return "ERROR";
    }
}

/* Telemetry counters: atomics for the hot counters, a mutex for the slow string/array
 * fields. The status is advisory, so this is deliberately lightweight. */
static _Atomic uint64_t st_frames_written = 0;       /* VDIF frames committed to disk   */
static _Atomic uint64_t st_bytes_written  = 0;
static _Atomic uint64_t st_kernel_drops   = 0;       /* accumulated PACKET_STATISTICS    */
static _Atomic uint64_t st_ring_losing    = 0;       /* blocks flagged TP_STATUS_LOSING  */
static _Atomic uint64_t st_spsc_drops     = 0;       /* frames dropped: pool exhausted   */
static _Atomic uint64_t st_bad_frames     = 0;       /* wrong size/port/proto            */
static _Atomic uint64_t st_gap_frames     = 0;       /* missing frames (from frame#)     */
static _Atomic uint64_t st_gap_events     = 0;
static _Atomic uint64_t st_dup            = 0;
static _Atomic uint64_t st_ooo            = 0;
static _Atomic uint64_t st_frames_seen    = 0;       /* frames received off the wire     */
static _Atomic int      st_time_ok        = 0;       /* last completed second was clean  */
static _Atomic int      st_state          = ST_RUNNING;

static pthread_mutex_t  st_lock = PTHREAD_MUTEX_INITIALIZER;
static char     st_cur_file[1408] = "";
static int      st_cur_disk = 0;
static uint64_t st_free_bytes[MAX_DISKS];
static char     st_alarm[256] = "";

static void set_alarm(const char *msg) {
    pthread_mutex_lock(&st_lock);
    snprintf(st_alarm, sizeof(st_alarm), "%s", msg);
    pthread_mutex_unlock(&st_lock);
    fprintf(stderr, "!! ALARM: %s\n", msg);
}

/* ----------------------------- SPSC index queue ------------------------------------ */
/* Single-producer / single-consumer lock-free ring of slab indices. */
typedef struct {
    _Atomic uint32_t head;     /* consumer */
    _Atomic uint32_t tail;     /* producer */
    uint32_t  cap;             /* power of two */
    uint32_t  mask;
    int32_t  *buf;
} spsc_t;

static void spsc_init(spsc_t *q, uint32_t cap_pow2) {
    q->cap = cap_pow2; q->mask = cap_pow2 - 1;
    q->buf = calloc(cap_pow2, sizeof(int32_t));
    atomic_store(&q->head, 0); atomic_store(&q->tail, 0);
}
static bool spsc_push(spsc_t *q, int32_t v) {       /* producer */
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t - h >= q->cap) return false;              /* full */
    q->buf[t & q->mask] = v;
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return true;
}
static bool spsc_pop(spsc_t *q, int32_t *out) {     /* consumer */
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h == t) return false;                       /* empty */
    *out = q->buf[h & q->mask];
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return true;
}

static spsc_t q_free;   /* writer -> capture : empty slabs available to fill */
static spsc_t q_full;   /* capture -> writer : full slabs ready to write     */

/* Per-slab descriptor (parallel to the arena, indexed by slab id). */
typedef struct {
    uint32_t frames;     /* VDIF frames packed into this slab (<= frames_per_slab) */
    uint32_t file_id;    /* which output file this slab belongs to                 */
    int      eof;        /* 1 => last slab of its file (writer finalizes after it)  */
} slabdesc_t;
static slabdesc_t *g_desc = NULL;

static atomic_int g_capture_running = 1;   /* writer drains until this clears + q empty */

/* ----------------------------- small helpers --------------------------------------- */
static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* VDIF (ref_epoch, sec_from_ep) -> Unix time_t (UTC). ref_epoch: units of 6 months
 * since 2000-01-01. */
static time_t vdif_to_unix(int ref_epoch, uint32_t sec) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    tm.tm_year = (2000 + ref_epoch / 2) - 1900;
    tm.tm_mon  = (ref_epoch % 2) ? 6 : 0;   /* Jan or Jul */
    tm.tm_mday = 1;
    time_t base = timegm(&tm);
    return base + (time_t)sec;
}

static int pin_thread(int core) {
    if (core < 0) return 0;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* ----------------------------- VDIF header parse ----------------------------------- */
typedef struct { uint32_t sec; uint32_t frame; int ref_epoch; int thread; int station;
                 uint32_t frame_len; int bps_m1; int data_type; } vhdr_t;
static inline void parse_vhdr(const uint8_t *v, vhdr_t *h) {
    uint32_t w0 = le32(v), w1 = le32(v+4), w2 = le32(v+8), w3 = le32(v+12);
    h->sec       = w0 & 0x3FFFFFFFu;
    h->frame     = w1 & 0x00FFFFFFu;
    h->ref_epoch = (w1 >> 24) & 0x3F;
    h->frame_len = (w2 & 0x00FFFFFFu) * 8u;
    h->thread    = (w3 >> 16) & 0x3FF;
    h->station   = w3 & 0xFFFF;
    h->bps_m1    = (w3 >> 26) & 0x1F;
    h->data_type = (w3 >> 31) & 1;
}

/* ============================================================================
 *  WRITER THREAD
 * ============================================================================ */
typedef struct {
    int      fd;
    char     path[1280];        /* final ".vdif" path                       */
    char     tmp[1296];         /* in-progress ".vdif.partial" path         */
    int      disk;              /* disk index this file lives on            */
    uint64_t frames;            /* frames written to this file so far       */
    /* provenance, parsed from the file's first frame */
    int      ref_epoch, thread, station;
    uint32_t first_sec, first_frame, last_sec, last_frame;
} ofile_t;

/* pick the disk index to use next: keep the current one until it falls below the
 * free-space threshold, then advance. Returns -1 when all remaining disks are full. */
static int choose_disk(int from) {
    for (int d = from; d < g_ndisks; d++) {
        struct statvfs vfs;
        if (statvfs(g_disks[d], &vfs) != 0) continue;
        uint64_t freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        if (freeb >= (uint64_t)(g_min_free_gb * 1e9)) return d;
    }
    return -1;
}

static void refresh_free(void) {
    pthread_mutex_lock(&st_lock);
    for (int d = 0; d < g_ndisks; d++) {
        struct statvfs vfs;
        st_free_bytes[d] = (statvfs(g_disks[d], &vfs) == 0)
                           ? (uint64_t)vfs.f_bavail * vfs.f_frsize : 0;
    }
    pthread_mutex_unlock(&st_lock);
}

/* open a new output file for the given first-frame header; returns 0 / -1 (disk full). */
static int file_open(ofile_t *f, const uint8_t *first_frame, int start_disk) {
    vhdr_t h; parse_vhdr(first_frame, &h);
    int disk = choose_disk(start_disk);
    if (disk < 0) return -1;

    time_t ut = vdif_to_unix(h.ref_epoch, h.sec);
    struct tm tm; gmtime_r(&ut, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y%m%dt%H%M%SZ", &tm);

    f->disk = disk; f->frames = 0;
    f->ref_epoch = h.ref_epoch; f->thread = h.thread; f->station = h.station;
    f->first_sec = h.sec; f->first_frame = h.frame;
    f->last_sec = h.sec; f->last_frame = h.frame;
    snprintf(f->path, sizeof f->path, "%s/bvex_%s_th%d.vdif", g_disks[disk], ts, h.thread);
    snprintf(f->tmp,  sizeof f->tmp,  "%s.partial", f->path);

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (g_use_odirect) flags |= O_DIRECT;
    f->fd = open(f->tmp, flags, 0644);
    if (f->fd < 0) {
        if (errno == ENOSPC) return -1;
        fprintf(stderr, "open(%s) failed: %s\n", f->tmp, strerror(errno));
        return -1;
    }
    /* preallocate the nominal extent so steady writes never stall on metadata.
     * fallocate is O(1) extent reservation on ext4/xfs (no zeroing). */
    off_t nominal = (off_t)g_secs_per_file * FRAMES_PER_SECOND * VDIF_FRAME_BYTES;
    if (fallocate(f->fd, 0, 0, nominal) != 0 && errno == ENOSPC) {
        close(f->fd); unlink(f->tmp); return -1;
    }
    pthread_mutex_lock(&st_lock);
    snprintf(st_cur_file, sizeof st_cur_file, "%s", f->path);
    st_cur_disk = disk;
    pthread_mutex_unlock(&st_lock);
    return 0;
}

static void write_sidecar(const ofile_t *f, uint64_t gap_frames_at_close) {
    char sc[1300]; snprintf(sc, sizeof sc, "%s.json", f->path);
    FILE *j = fopen(sc, "w");
    if (!j) return;
    time_t ut = vdif_to_unix(f->ref_epoch, f->first_sec);
    struct tm tm; gmtime_r(&ut, &tm);
    char iso[40]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);
    uint64_t span_secs = (f->last_sec >= f->first_sec) ? (f->last_sec - f->first_sec + 1) : 1;
    fprintf(j,
        "{\n"
        "  \"file\": \"%s\",\n"
        "  \"format\": \"VDIF/8032/2\",\n"
        "  \"thread_id\": %d,\n"
        "  \"station_id\": %d,\n"
        "  \"ref_epoch\": %d,\n"
        "  \"start_utc\": \"%s\",\n"
        "  \"first_sec\": %u,\n  \"first_frame\": %u,\n"
        "  \"last_sec\": %u,\n  \"last_frame\": %u,\n"
        "  \"frame_count\": %llu,\n"
        "  \"expected_count\": %llu,\n"
        "  \"gap_frames_total_at_close\": %llu,\n"
        "  \"bytes\": %llu,\n"
        "  \"disk\": \"%s\"\n"
        "}\n",
        f->path, f->thread, f->station, f->ref_epoch, iso,
        f->first_sec, f->first_frame, f->last_sec, f->last_frame,
        (unsigned long long)f->frames,
        (unsigned long long)(span_secs * FRAMES_PER_SECOND),
        (unsigned long long)gap_frames_at_close,
        (unsigned long long)(f->frames * VDIF_FRAME_BYTES),
        g_disks[f->disk]);
    fclose(j);
}

static void file_finalize(ofile_t *f) {
    if (f->fd < 0) return;
    /* trim the preallocated extent to the byte-exact frame count, flush, close,
     * then atomically reveal as ".vdif" (rename signals a complete file). */
    off_t exact = (off_t)f->frames * VDIF_FRAME_BYTES;
    if (ftruncate(f->fd, exact) != 0)
        fprintf(stderr, "ftruncate(%s) failed: %s\n", f->tmp, strerror(errno));
    fdatasync(f->fd);
    close(f->fd); f->fd = -1;
    if (rename(f->tmp, f->path) != 0)
        fprintf(stderr, "rename(%s -> %s) failed: %s\n", f->tmp, f->path, strerror(errno));
    write_sidecar(f, atomic_load(&st_gap_frames));
    refresh_free();
}

/* write one slab (full or partial). O_DIRECT needs 4 KiB-aligned length, so a
 * partial (eof) slab is zero-padded up to the next page; the file is trimmed
 * byte-exact in file_finalize(). */
static int slab_write(ofile_t *f, const uint8_t *slab, uint32_t frames) {
    size_t bytes = (size_t)frames * VDIF_FRAME_BYTES;
    size_t wlen  = bytes;
    if (g_use_odirect && (wlen % PAGE)) {
        size_t padded = (wlen + PAGE - 1) / PAGE * PAGE;     /* room exists: slab is 2008 pages */
        memset((void *)(slab + bytes), 0, padded - bytes);
        wlen = padded;
    }
    size_t off = 0;
    while (off < wlen) {
        ssize_t n = write(f->fd, slab + off, wlen - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSPC) return -1;
            fprintf(stderr, "write(%s) failed: %s\n", f->tmp, strerror(errno));
            return -1;
        }
        off += (size_t)n;
    }
    f->frames += frames;
    atomic_fetch_add(&st_frames_written, frames);
    atomic_fetch_add(&st_bytes_written, bytes);
    return 0;
}

static void *writer_thread(void *arg) {
    (void)arg;
    pin_thread(g_wr_core);
    ofile_t cur; memset(&cur, 0, sizeof cur); cur.fd = -1;
    int      have_file  = 0;
    uint32_t cur_tag    = 0;     /* file_id of the currently open file              */
    int      start_disk = 0;     /* disk to start the next choose_disk() search at  */
    struct timespec idle = { .tv_sec = 0, .tv_nsec = 200000 };  /* 200 us */

    while (1) {
        int32_t idx;
        if (!spsc_pop(&q_full, &idx)) {
            if (!atomic_load(&g_capture_running)) break;
            nanosleep(&idle, NULL);
            continue;
        }
        uint8_t   *slab = g_arena + (size_t)idx * g_slab_bytes;
        slabdesc_t d    = g_desc[idx];

        /* finalize-on-file_id-change: when a slab from a new file arrives, close the
         * old file first. This is robust even if a stall meant the old file's last
         * slab was never flagged eof. */
        if (!have_file || d.file_id != cur_tag) {
            if (have_file) { file_finalize(&cur); start_disk = cur.disk; }
            if (file_open(&cur, slab, start_disk) != 0) {
                set_alarm("all disks full -- recording stopped, data preserved");
                atomic_store(&st_state, ST_STOPPED_DISK_FULL);
                g_stop = 1; spsc_push(&q_free, idx); break;
            }
            cur_tag = d.file_id; have_file = 1;
        }

        if (slab_write(&cur, slab, d.frames) != 0) {
            /* current disk filled mid-file: finalize, advance to the next disk, retry. */
            file_finalize(&cur); have_file = 0;
            start_disk = cur.disk + 1;
            if (file_open(&cur, slab, start_disk) != 0 ||
                slab_write(&cur, slab, d.frames) != 0) {
                set_alarm("all disks full -- recording stopped, data preserved");
                atomic_store(&st_state, ST_STOPPED_DISK_FULL);
                g_stop = 1; spsc_push(&q_free, idx); break;
            }
            cur_tag = d.file_id; have_file = 1;
        }

        /* track last frame for the sidecar (read the slab's last frame header) */
        {
            const uint8_t *lastf = slab + (size_t)(d.frames - 1) * VDIF_FRAME_BYTES;
            vhdr_t h; parse_vhdr(lastf, &h);
            cur.last_sec = h.sec; cur.last_frame = h.frame;
        }

        spsc_push(&q_free, idx);             /* return the slab to the pool */
    }

    if (cur.fd >= 0) file_finalize(&cur);    /* finalize any still-open file (idempotent) */
    return NULL;
}

/* ============================================================================
 *  CAPTURE THREAD
 * ============================================================================ */
typedef struct {
    int       fd;
    uint8_t  *map;
    size_t    map_len;
    uint32_t  block_size;
    uint32_t  nblocks;
} ring_t;

/* attach a kernel BPF so only IPv4/UDP/dst-port==g_dport reaches the ring; ARP,
 * LLDP and link-train junk are dropped in-kernel and never pollute the counters. */
static int attach_bpf(int fd, int dport) {
    struct sock_filter code[] = {
        { 0x28, 0, 0, 0x0000000c },                 /* ldh  [12]  ethertype           */
        { 0x15, 0, 8, 0x00000800 },                 /* jeq  IPv4 ? : ret0             */
        { 0x30, 0, 0, 0x00000017 },                 /* ldb  [23]  IP proto            */
        { 0x15, 0, 6, 0x00000011 },                 /* jeq  UDP  ? : ret0             */
        { 0x28, 0, 0, 0x00000014 },                 /* ldh  [20]  frag field          */
        { 0x45, 4, 0, 0x00001fff },                 /* jset frag -> ret0              */
        { 0xb1, 0, 0, 0x0000000e },                 /* ldxb 4*([14]&0xf) -> X=IP hlen  */
        { 0x48, 0, 0, 0x00000010 },                 /* ldh  [x+16] UDP dst port        */
        { 0x15, 0, 1, 0x00000000 },                 /* jeq  dport ? PASS : ret0  (k patched) */
        { 0x06, 0, 0, 0x00040000 },                 /* ret  262144  (PASS)            */
        { 0x06, 0, 0, 0x00000000 },                 /* ret  0       (DROP)            */
    };
    code[8].k = (uint32_t)dport;
    struct sock_fprog prog = { .len = sizeof(code)/sizeof(code[0]), .filter = code };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) != 0) {
        fprintf(stderr, "SO_ATTACH_FILTER failed (continuing, software-validated): %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static int ring_setup(ring_t *r) {
    r->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (r->fd < 0) { perror("socket(AF_PACKET)"); return -1; }

    int ver = TPACKET_V3;
    if (setsockopt(r->fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof ver) != 0) {
        perror("PACKET_VERSION"); return -1;
    }
    attach_bpf(r->fd, g_dport);                     /* before bind -> no race */

    struct tpacket_req3 req; memset(&req, 0, sizeof req);
    req.tp_block_size      = (unsigned)g_block_mb * 1024u * 1024u;
    req.tp_block_nr        = (unsigned)g_nblocks;
    req.tp_frame_size      = 16384;                 /* >= any MTU-9000 frame + v3 hdr  */
    req.tp_frame_nr        = (req.tp_block_size / req.tp_frame_size) * req.tp_block_nr;
    req.tp_retire_blk_tov  = 10;                    /* ms; flush idle partial blocks   */
    req.tp_sizeof_priv     = 0;
    req.tp_feature_req_word = 0;
    if (setsockopt(r->fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof req) != 0) {
        perror("PACKET_RX_RING (lower --block-mb/--nblocks, or raise RLIMIT_MEMLOCK)");
        return -1;
    }
    r->block_size = req.tp_block_size;
    r->nblocks    = req.tp_block_nr;
    r->map_len    = (size_t)req.tp_block_size * req.tp_block_nr;
    /* MAP_SHARED only: mlockall(MCL_FUTURE) (set in main before this) already pins it,
     * and MAP_LOCKED can spuriously fail under a low memlock limit. */
    r->map = mmap(NULL, r->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (r->map == MAP_FAILED) { perror("mmap(ring)"); return -1; }

    struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = if_nametoindex(g_iface);
    if (sll.sll_ifindex == 0) { fprintf(stderr, "no iface %s\n", g_iface); return -1; }
    if (bind(r->fd, (struct sockaddr *)&sll, sizeof sll) != 0) { perror("bind"); return -1; }
    return 0;
}

/* extract the VDIF payload pointer + length from one captured frame; validates
 * IPv4/UDP/dst-port/size. Returns the 8032-byte VDIF frame, or NULL (bad).
 * NOTE: tp_mac is the offset from THIS frame header (ppd), not from the block. */
static const uint8_t *frame_to_vdif(struct tpacket3_hdr *ppd) {
    const uint8_t *eth = (const uint8_t *)ppd + ppd->tp_mac;
    uint32_t snap = ppd->tp_snaplen;
    if (snap < 14 + 20 + 8 + VDIF_FRAME_BYTES) return NULL;
    if (eth[12] != 0x08 || eth[13] != 0x00) return NULL;          /* IPv4 */
    const uint8_t *ip = eth + 14;
    int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20) return NULL;
    if (ip[9] != 17) return NULL;                                  /* UDP */
    const uint8_t *udp = ip + ihl;
    int dport = (udp[2] << 8) | udp[3];
    if (dport != g_dport) return NULL;
    const uint8_t *vdif = udp + 8;
    /* ensure the whole VDIF frame is within the captured bytes */
    if ((uint32_t)((vdif - eth) + VDIF_FRAME_BYTES) > snap) return NULL;
    vhdr_t h; parse_vhdr(vdif, &h);
    if (h.frame_len != VDIF_FRAME_BYTES) return NULL;              /* size gate */
    return vdif;
}

/* ---- shared per-frame state + processing (used by every capture backend) ---- */
typedef struct {
    int32_t  cur_idx; uint8_t *cur; uint32_t cur_frames, cur_file_id;
    int      started; uint32_t file_start_sec;
    int      have_prev; uint32_t prev_sec, prev_frame;
    uint32_t sec_cur, sec_min, sec_max; int sec_have;
} capstate_t;

static void capstate_init(capstate_t *s) {
    memset(s, 0, sizeof *s); s->cur_idx = -1; s->sec_min = 0xffffffff;
}

/* Process one 8032-byte VDIF frame: validate, gap-account, second-align files, pack
 * into a slab. Returns 1 if we must stop (g_stop hit while blocked pushing a slab). */
static int process_one_frame(capstate_t *s, const uint8_t *vdif) {
    vhdr_t h; parse_vhdr(vdif, &h);
    if (h.frame_len != VDIF_FRAME_BYTES) { atomic_fetch_add(&st_bad_frames, 1); return 0; }
    atomic_fetch_add(&st_frames_seen, 1);

    if (!s->started) {
        if (g_align_start && h.frame != 0) return 0;
        s->started = 1; s->file_start_sec = h.sec; s->cur_file_id = 0; s->have_prev = 0;
        s->sec_cur = h.sec; s->sec_min = 0xffffffff; s->sec_max = 0; s->sec_have = 0;
    }

    if (s->have_prev) {
        uint32_t es, ef;
        if (s->prev_frame + 1 >= FRAMES_PER_SECOND) { es = s->prev_sec + 1; ef = 0; }
        else { es = s->prev_sec; ef = s->prev_frame + 1; }
        if (h.sec == es && h.frame == ef) { /* in order */ }
        else if (h.sec == s->prev_sec && h.frame == s->prev_frame) atomic_fetch_add(&st_dup, 1);
        else if (h.sec > es || (h.sec == es && h.frame > ef)) {
            uint64_t miss = (h.sec == es) ? (h.frame - ef)
                : ((uint64_t)(h.sec - es)) * FRAMES_PER_SECOND + h.frame - ef;
            atomic_fetch_add(&st_gap_frames, miss); atomic_fetch_add(&st_gap_events, 1);
        } else atomic_fetch_add(&st_ooo, 1);
    }
    s->prev_sec = h.sec; s->prev_frame = h.frame; s->have_prev = 1;

    if (h.sec != s->sec_cur) {
        if (s->sec_have)
            atomic_store(&st_time_ok, (s->sec_min == 0 && s->sec_max == FRAMES_PER_SECOND - 1));
        s->sec_cur = h.sec; s->sec_min = h.frame; s->sec_max = h.frame; s->sec_have = 1;
    } else {
        if (h.frame < s->sec_min) s->sec_min = h.frame;
        if (h.frame > s->sec_max) s->sec_max = h.frame;
    }

    if (h.frame == 0 && (h.sec % (uint32_t)g_secs_per_file) == 0 && h.sec != s->file_start_sec) {
        if (s->cur_idx >= 0 && s->cur_frames > 0) {
            g_desc[s->cur_idx].frames = s->cur_frames;
            g_desc[s->cur_idx].file_id = s->cur_file_id; g_desc[s->cur_idx].eof = 1;
            while (!spsc_push(&q_full, s->cur_idx)) { if (g_stop) return 1; sched_yield(); }
            s->cur_idx = -1; s->cur_frames = 0;
        }
        s->cur_file_id++; s->file_start_sec = h.sec;
    }

    if (s->cur_idx < 0) {
        if (!spsc_pop(&q_free, &s->cur_idx)) { atomic_fetch_add(&st_spsc_drops, 1); return 0; }
        s->cur = g_arena + (size_t)s->cur_idx * g_slab_bytes; s->cur_frames = 0;
    }
    memcpy(s->cur + (size_t)s->cur_frames * VDIF_FRAME_BYTES, vdif, VDIF_FRAME_BYTES);
    s->cur_frames++;
    if (s->cur_frames == (uint32_t)g_frames_per_slab) {
        g_desc[s->cur_idx].frames = s->cur_frames;
        g_desc[s->cur_idx].file_id = s->cur_file_id; g_desc[s->cur_idx].eof = 0;
        while (!spsc_push(&q_full, s->cur_idx)) { if (g_stop) return 1; sched_yield(); }
        s->cur_idx = -1; s->cur_frames = 0;
    }
    return 0;
}

static void capstate_finish(capstate_t *s) {
    if (s->cur_idx >= 0 && s->cur_frames > 0) {
        g_desc[s->cur_idx].frames = s->cur_frames;
        g_desc[s->cur_idx].file_id = s->cur_file_id; g_desc[s->cur_idx].eof = 1;
        while (!spsc_push(&q_full, s->cur_idx)) { struct timespec t={0,500000}; nanosleep(&t,NULL); }
    } else if (s->cur_idx >= 0) {
        spsc_push(&q_free, s->cur_idx);
    }
    atomic_store(&g_capture_running, 0);
}

/* ---- backend (default): recvmmsg on a plain UDP socket (is_udp=1) or AF_PACKET
 * SOCK_RAW (is_udp=0). Uses the STANDARD kernel skb path, NOT the TPACKET mmap ring;
 * on mlx5 the ring triggers NIC rx_out_of_buffer at sustained rate, the normal path
 * does not. A UDP socket also hands us the 8032-B VDIF frame directly (no strip). ---- */
static void *capture_recvmmsg(void *arg) {
    int is_udp = *(int *)arg;
    pin_thread(g_cap_core);

    int fd;
    if (is_udp) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket(udp)"); atomic_store(&g_capture_running, 0); return NULL; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(g_dport); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            perror("bind(udp)"); atomic_store(&g_capture_running, 0); return NULL; }
    } else {
        fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) { perror("socket(raw)"); atomic_store(&g_capture_running, 0); return NULL; }
        attach_bpf(fd, g_dport);
        struct sockaddr_ll sll; memset(&sll, 0, sizeof sll);
        sll.sll_family = AF_PACKET; sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = if_nametoindex(g_iface);
        if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
            perror("bind(raw)"); atomic_store(&g_capture_running, 0); return NULL; }
    }
    /* SMALL socket buffer on purpose: a large one pins too many mlx5 RX pages in held
     * skbs and starves the driver's page pool (-> rx_out_of_buffer). The consumer keeps
     * up, so a few-MB buffer never overflows. SO_RCVBUFFORCE bypasses rmem_max. */
    int rb = g_rcvbuf_mb * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rb, sizeof rb) != 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
#ifdef SO_BUSY_POLL
    if (g_busy_poll_us > 0) {
        int bp = g_busy_poll_us;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &bp, sizeof bp) != 0)
            fprintf(stderr, "SO_BUSY_POLL failed (%s); continuing without it\n", strerror(errno));
    }
#endif
    g_capture_fd = fd;

    enum { BATCH = 512, BUFSZ = 9216 };
    static uint8_t bufs[BATCH][BUFSZ];
    struct mmsghdr *msgs = calloc(BATCH, sizeof *msgs);
    struct iovec   *iov  = calloc(BATCH, sizeof *iov);

    capstate_t st; capstate_init(&st);
    while (!g_stop) {
        for (int i = 0; i < BATCH; i++) {
            iov[i].iov_base = bufs[i]; iov[i].iov_len = BUFSZ;
            memset(&msgs[i], 0, sizeof msgs[i]);
            msgs[i].msg_hdr.msg_iov = &iov[i]; msgs[i].msg_hdr.msg_iovlen = 1;
        }
        struct timespec to = { .tv_sec = 0, .tv_nsec = 200000000 };   /* 200 ms */
        int n = recvmmsg(fd, msgs, BATCH, MSG_WAITFORONE, &to);
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; perror("recvmmsg"); break; }
        for (int i = 0; i < n; i++) {
            uint32_t len = msgs[i].msg_len;
            const uint8_t *vdif;
            if (is_udp) {
                if (len < VDIF_FRAME_BYTES) { atomic_fetch_add(&st_bad_frames, 1); continue; }
                vdif = bufs[i];
            } else {
                const uint8_t *eth = bufs[i];
                if (len < 14 + 20 + 8 + VDIF_FRAME_BYTES) { atomic_fetch_add(&st_bad_frames, 1); continue; }
                if (eth[12] != 0x08 || eth[13] != 0x00) { atomic_fetch_add(&st_bad_frames, 1); continue; }
                int ihl = (eth[14] & 0x0f) * 4; if (ihl < 20) { atomic_fetch_add(&st_bad_frames, 1); continue; }
                vdif = eth + 14 + ihl + 8;
            }
            if (process_one_frame(&st, vdif)) goto done;
        }
    }
done:
    capstate_finish(&st);
    free(msgs); free(iov); close(fd);
    return NULL;
}

/* ---- optional backend (--rx ring): TPACKET_V3 mmap ring. Kept for reference; on
 * mlx5 it can trigger NIC rx_out_of_buffer at sustained rate (see RECORDER_README). ---- */
static void *capture_ring(void *arg) {
    ring_t *r = (ring_t *)arg;
    pin_thread(g_cap_core);
    g_capture_fd = r->fd;
    capstate_t st; capstate_init(&st);
    struct pollfd pfd = { .fd = r->fd, .events = POLLIN };
    uint32_t bi = 0;
    while (!g_stop) {
        struct tpacket_block_desc *pbd =
            (struct tpacket_block_desc *)(r->map + (size_t)bi * r->block_size);
        if (!(pbd->hdr.bh1.block_status & TP_STATUS_USER)) { poll(&pfd, 1, 100); continue; }
        if (pbd->hdr.bh1.block_status & TP_STATUS_LOSING) atomic_fetch_add(&st_ring_losing, 1);
        uint32_t np = pbd->hdr.bh1.num_pkts;
        struct tpacket3_hdr *ppd =
            (struct tpacket3_hdr *)((uint8_t *)pbd + pbd->hdr.bh1.offset_to_first_pkt);
        for (uint32_t p = 0; p < np;
             p++, ppd = (struct tpacket3_hdr *)((uint8_t *)ppd + ppd->tp_next_offset)) {
            const uint8_t *vdif = frame_to_vdif(ppd);
            if (!vdif) { atomic_fetch_add(&st_bad_frames, 1); continue; }
            if (process_one_frame(&st, vdif)) goto done;
        }
        pbd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        bi = (bi + 1) % r->nblocks;
    }
done:
    capstate_finish(&st);
    return NULL;
}

/* ---- backend (--rx xdp): AF_XDP zero-copy multi-buffer. The mlx5 NIC DMAs frames
 * directly into a userspace UMEM; libxdp loads a frags-aware default redirect program
 * so jumbo (MTU 9000) frames are accepted. Each 8074-B wire frame arrives as 2-3
 * chained fragments (XDP_PKT_CONTD) which we reassemble, strip the 42-B wrapper, and
 * feed to the shared process_one_frame(). This is the path that breaks the kernel
 * single-flow receive ceiling. ---- */
#ifdef HAVE_XDP
/* (names deliberately avoid the XDP_*_RING uapi setsockopt optnames)
 * Rings kept tight: a small UMEM working set stays cache-resident, so the consumer
 * sustains line rate -- bigger rings hurt locality and the consumer falls behind. */
#define XSK_FRAME_SZ     4096u            /* UMEM chunk (== PAGE; max on x86-64)        */
#define XSK_NUM_FRAMES   (1u << 16)       /* 65536 chunks -> 256 MiB UMEM             */
#define XSK_FILL_SZ      (1u << 14)       /* 16384                                     */
#define XSK_COMP_SZ      (1u << 11)       /* unused for RX-only, but must be set       */
#define XSK_RX_SZ        (1u << 13)       /* 8192                                      */
#define XSK_PEEK_BATCH   256u

static struct xsk_socket *g_xsk = NULL;   /* for XDP_STATISTICS in write_status()      */

/* locate xsk_redirect.bpf.o next to the executable (falls back to cwd) */
static void resolve_bpf_obj(char *out, size_t n) {
    char exe[1024];
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r > 0) {
        exe[r] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = 0; snprintf(out, n, "%s/xsk_redirect.bpf.o", exe); return; }
    }
    snprintf(out, n, "xsk_redirect.bpf.o");
}

static void *capture_xdp(void *arg) {
    (void)arg;
    pin_thread(g_cap_core);

    /* page-aligned UMEM arena (mlockall already pins it) */
    size_t area_sz = (size_t)XSK_NUM_FRAMES * XSK_FRAME_SZ;
    void  *umem_area = NULL;
    if (posix_memalign(&umem_area, getpagesize(), area_sz) != 0) {
        fprintf(stderr, "xdp: UMEM alloc (%zu B) failed\n", area_sz);
        atomic_store(&g_capture_running, 0); return NULL;
    }
    memset(umem_area, 0, area_sz);   /* pre-fault every page now (no faults in the hot loop) */

    struct xsk_umem *umem = NULL;
    struct xsk_ring_prod fill; struct xsk_ring_cons comp;
    struct xsk_umem_config ucfg = {
        .fill_size = XSK_FILL_SZ, .comp_size = XSK_COMP_SZ,
        .frame_size = XSK_FRAME_SZ, .frame_headroom = 0, .flags = 0,
    };
    int err = xsk_umem__create(&umem, umem_area, area_sz, &fill, &comp, &ucfg);
    if (err) { fprintf(stderr, "xsk_umem__create failed: %s\n", strerror(-err));
               atomic_store(&g_capture_running, 0); return NULL; }

    unsigned int ifindex = if_nametoindex(g_iface);
    /* clear any stale XDP program from a crashed run (harmless if none) */
    bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, NULL);
    bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE, NULL);

    /* Load + attach OUR frags-aware redirect program (libxdp's default program/dispatcher
     * would not attach in COPY mode on this mlx5; SEC("xdp.frags") + modern libbpf sets
     * BPF_F_XDP_HAS_FRAGS so the MTU-9000 attach is accepted). Attaching ONCE up front
     * means the ZC->COPY bind fallback below needs no re-attach. */
    char bpf_path[1100]; resolve_bpf_obj(bpf_path, sizeof bpf_path);
    struct bpf_object *bobj = bpf_object__open_file(bpf_path, NULL);
    if (!bobj || libbpf_get_error(bobj)) {
        fprintf(stderr, "xdp: bpf_object__open_file(%s) failed\n", bpf_path);
        xsk_umem__delete(umem); free(umem_area);
        atomic_store(&g_capture_running, 0); return NULL;
    }
    if (bpf_object__load(bobj) != 0) {
        fprintf(stderr, "xdp: bpf_object__load failed: %s\n", strerror(errno));
        bpf_object__close(bobj); xsk_umem__delete(umem); free(umem_area);
        atomic_store(&g_capture_running, 0); return NULL;
    }
    struct bpf_program *prog = bpf_object__find_program_by_name(bobj, "xsk_redirect_prog");
    int xsks_map_fd = bpf_object__find_map_fd_by_name(bobj, "xsks_map");
    int prog_fd = prog ? bpf_program__fd(prog) : -1;
    if (prog_fd < 0 || xsks_map_fd < 0) {
        fprintf(stderr, "xdp: prog/map fd lookup failed (%d/%d)\n", prog_fd, xsks_map_fd);
        bpf_object__close(bobj); xsk_umem__delete(umem); free(umem_area);
        atomic_store(&g_capture_running, 0); return NULL;
    }
    __u32 attach_mode = g_xdp_drv ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
    if (bpf_xdp_attach(ifindex, prog_fd, attach_mode, NULL) != 0) {
        fprintf(stderr, "xdp: native attach failed (%s); trying generic/SKB\n", strerror(errno));
        attach_mode = XDP_FLAGS_SKB_MODE;
        if (bpf_xdp_attach(ifindex, prog_fd, attach_mode, NULL) != 0) {
            fprintf(stderr, "xdp: bpf_xdp_attach failed: %s\n", strerror(errno));
            bpf_object__close(bobj); xsk_umem__delete(umem); free(umem_area);
            atomic_store(&g_capture_running, 0); return NULL;
        }
    }

    /* create the XSK with INHIBIT_PROG_LOAD (we manage the program). Try ZERO-COPY first
     * (errors cleanly if the driver lacks ZC multi-buffer), then COPY. XDP_USE_SG enables
     * multi-buffer (mandatory for 8074-B frames); XDP_USE_NEED_WAKEUP for the poll kick. */
    struct xsk_socket *xsk = NULL;
    struct xsk_ring_cons rx;
    int want_zc = g_xdp_zerocopy, got_zc = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        struct xsk_socket_config scfg = {
            .rx_size = XSK_RX_SZ, .tx_size = 0,
            .libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,
            .xdp_flags = attach_mode,
            .bind_flags = (__u16)(XDP_USE_NEED_WAKEUP | XDP_USE_SG |
                                  (want_zc ? XDP_ZEROCOPY : XDP_COPY)),
        };
        err = xsk_socket__create(&xsk, g_iface, (__u32)g_xdp_queue, umem, &rx, NULL, &scfg);
        if (!err) { got_zc = want_zc; break; }
        fprintf(stderr, "xsk_socket__create q%d %s bind failed: %s\n",
                g_xdp_queue, want_zc ? "ZEROCOPY" : "COPY", strerror(-err));
        if (want_zc) { want_zc = 0; continue; }   /* retry COPY (program stays attached) */
        bpf_xdp_detach(ifindex, attach_mode, NULL); bpf_object__close(bobj);
        xsk_umem__delete(umem); free(umem_area);
        atomic_store(&g_capture_running, 0); return NULL;
    }
    g_xsk = xsk;
    int xfd = xsk_socket__fd(xsk);
    g_capture_fd = xfd;

    /* insert our xsk into the map so the program redirects this queue to it */
    {
        __u32 key = (__u32)g_xdp_queue, val = (__u32)xfd;
        if (bpf_map_update_elem(xsks_map_fd, &key, &val, 0) != 0)
            fprintf(stderr, "xdp: xsks_map update failed: %s (redirect may not work)\n",
                    strerror(errno));
    }

    /* confirm what we actually got (ZC vs COPY) from the kernel, not just what we asked */
    {
        struct xdp_options xo; socklen_t ol = sizeof xo;
        if (getsockopt(xfd, SOL_XDP, XDP_OPTIONS, &xo, &ol) == 0)
            got_zc = (xo.flags & XDP_OPTIONS_ZEROCOPY) ? 1 : 0;
    }
    fprintf(stderr, "xdp: bound queue %d, %s mode, %s, SG(multi-buf) on, umem=%zuMiB\n",
            g_xdp_queue, got_zc ? "ZERO-COPY" : "COPY",
            g_xdp_drv ? "native/DRV" : "generic/SKB", area_sz >> 20);

    /* opt into NAPI busy-poll (with tune_nic.sh's napi_defer_hard_irqs/gro_flush_timeout
     * this lets one core poll+repost without softirq contention). Best-effort. */
    if (g_xdp_busy) {
        int one = 1, bp = g_busy_poll_us > 0 ? g_busy_poll_us : 20, budget = 64;
        setsockopt(xfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof one);
        setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL, &bp, sizeof bp);
        setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof budget);
    }

    /* prime the fill ring with the first FILL_RING chunks (they cycle fill->rx->fill) */
    {
        uint32_t idx = 0;
        unsigned int n = xsk_ring_prod__reserve(&fill, XSK_FILL_SZ, &idx);
        for (unsigned int i = 0; i < n; i++)
            *xsk_ring_prod__fill_addr(&fill, idx + i) = (uint64_t)i * XSK_FRAME_SZ;
        xsk_ring_prod__submit(&fill, n);
    }

    capstate_t st; capstate_init(&st);
    struct pollfd pfd = { .fd = xfd, .events = POLLIN };
    static uint8_t scratch[16384];        /* reassembly buffer for one wire frame      */
    uint32_t scl = 0; int overflow = 0;   /* persists across peeks (frag may straddle) */
    int stop_now = 0;
    const uint32_t MINWIRE = 14 + 20 + 8 + VDIF_FRAME_BYTES;

    while (!g_stop) {
        /* drive RX: in busy-poll mode kick every iteration; else only when the kernel
         * asks (fill ring needs a wakeup). */
        if (g_xdp_busy || xsk_ring_prod__needs_wakeup(&fill))
            recvfrom(xfd, NULL, 0, MSG_DONTWAIT, NULL, NULL);

        uint32_t idx_rx = 0;
        unsigned int rcvd = xsk_ring_cons__peek(&rx, XSK_PEEK_BATCH, &idx_rx);
        if (!rcvd) { if (!g_xdp_busy) poll(&pfd, 1, 100); continue; }

        /* reserve fill slots to return the chunks we are about to consume (1:1) */
        uint32_t idx_fl = 0;
        while (xsk_ring_prod__reserve(&fill, rcvd, &idx_fl) < rcvd) {
            if (g_stop) { stop_now = 1; break; }
            if (xsk_ring_prod__needs_wakeup(&fill))
                recvfrom(xfd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
        }
        if (stop_now) break;

        for (unsigned int i = 0; i < rcvd; i++) {
            const struct xdp_desc *d = xsk_ring_cons__rx_desc(&rx, idx_rx + i);
            uint64_t addr = d->addr; uint32_t len = d->len, opts = d->options;
            const uint8_t *frag = (const uint8_t *)umem_area + addr;
            if (scl + len <= sizeof scratch) { memcpy(scratch + scl, frag, len); scl += len; }
            else overflow = 1;
            /* return this chunk (base = addr aligned down to the 4 KiB chunk) to fill */
            *xsk_ring_prod__fill_addr(&fill, idx_fl + i) =
                addr & ~((uint64_t)(XSK_FRAME_SZ - 1));

            if (!(opts & XDP_PKT_CONTD)) {           /* last fragment -> frame complete */
                if (!stop_now && !overflow && scl >= MINWIRE) {
                    const uint8_t *eth = scratch;
                    if (eth[12] == 0x08 && eth[13] == 0x00) {     /* IPv4 */
                        int ihl = (eth[14] & 0x0f) * 4;
                        if (ihl >= 20) {
                            if (process_one_frame(&st, eth + 14 + ihl + 8)) stop_now = 1;
                        } else atomic_fetch_add(&st_bad_frames, 1);
                    } else atomic_fetch_add(&st_bad_frames, 1);
                } else if (!stop_now && (scl > 0 || overflow)) {
                    atomic_fetch_add(&st_bad_frames, 1);
                }
                scl = 0; overflow = 0;     /* every desc's chunk is reposted regardless */
            }
        }
        xsk_ring_prod__submit(&fill, rcvd);
        xsk_ring_cons__release(&rx, rcvd);
        if (stop_now) break;
    }

    /* breakdown of any XSK-layer drops (diagnostic) before we tear down */
    {
        struct xdp_statistics xs; socklen_t xl = sizeof xs;
        if (getsockopt(xfd, SOL_XDP, XDP_STATISTICS, &xs, &xl) == 0)
            fprintf(stderr, "xdp: stats rx_dropped=%llu rx_invalid=%llu rx_ring_full=%llu "
                    "fill_empty=%llu\n",
                    (unsigned long long)xs.rx_dropped, (unsigned long long)xs.rx_invalid_descs,
                    (unsigned long long)xs.rx_ring_full,
                    (unsigned long long)xs.rx_fill_ring_empty_descs);
    }
    capstate_finish(&st);
    g_xsk = NULL;
    xsk_socket__delete(xsk);                       /* removes the xsk from the map        */
    xsk_umem__delete(umem);
    bpf_xdp_detach(ifindex, attach_mode, NULL);    /* detach our program (no leftover)    */
    bpf_object__close(bobj);
    free(umem_area);
    return NULL;
}
#else  /* !HAVE_XDP */
static void *capture_xdp(void *arg) {
    (void)arg;
    fprintf(stderr, "ERROR: this binary was built without AF_XDP support "
                    "(rebuild with HAVE_XDP + libxdp). Use --rx udp.\n");
    atomic_store(&g_capture_running, 0); g_stop = 1;
    return NULL;
}
#endif /* HAVE_XDP */

/* ============================================================================
 *  STATUS  (main thread, ~1 Hz)
 * ============================================================================ */
/* UDP socket-buffer overflow drops (cumulative since boot) from /proc/net/snmp. */
static uint64_t udp_rcvbuf_errors(void) {
    FILE *f = fopen("/proc/net/snmp", "r");
    if (!f) return 0;
    char hdr[2048], val[2048]; uint64_t out = 0;
    while (fgets(hdr, sizeof hdr, f)) {
        if (strncmp(hdr, "Udp:", 4) != 0) continue;
        if (!fgets(val, sizeof val, f)) break;        /* the paired values line */
        char *hsave, *vsave;
        char *ht = strtok_r(hdr, " \t\n", &hsave);    /* "Udp:" */
        char *vt = strtok_r(val, " \t\n", &vsave);
        while ((ht = strtok_r(NULL, " \t\n", &hsave)) && (vt = strtok_r(NULL, " \t\n", &vsave))) {
            if (strcmp(ht, "RcvbufErrors") == 0) { out = strtoull(vt, NULL, 10); break; }
        }
        break;
    }
    fclose(f);
    return out;
}

static void write_status(double fps, double wr_mb_s) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_status_path);
    FILE *j = fopen(tmp, "w");
    if (!j) return;
    pthread_mutex_lock(&st_lock);
    char curfile[1408]; snprintf(curfile, sizeof curfile, "%s", st_cur_file);
    int  curdisk = st_cur_disk;
    char alarm[256]; snprintf(alarm, sizeof alarm, "%s", st_alarm);
    uint64_t freeb[MAX_DISKS]; memcpy(freeb, st_free_bytes, sizeof freeb);
    pthread_mutex_unlock(&st_lock);

    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    fprintf(j, "{\n");
    fprintf(j, "  \"ts_unix\": %ld,\n", (long)now.tv_sec);
    fprintf(j, "  \"state\": \"%s\",\n", state_name(atomic_load(&st_state)));
    fprintf(j, "  \"fps_capture\": %.0f,\n", fps);
    fprintf(j, "  \"write_MBps\": %.1f,\n", wr_mb_s);
    fprintf(j, "  \"frames_written\": %llu,\n", (unsigned long long)atomic_load(&st_frames_written));
    fprintf(j, "  \"bytes_written\": %llu,\n", (unsigned long long)atomic_load(&st_bytes_written));
    fprintf(j, "  \"frames_seen\": %llu,\n", (unsigned long long)atomic_load(&st_frames_seen));
    fprintf(j, "  \"kernel_drops\": %llu,\n", (unsigned long long)atomic_load(&st_kernel_drops));
    fprintf(j, "  \"ring_losing\": %llu,\n", (unsigned long long)atomic_load(&st_ring_losing));
    fprintf(j, "  \"spsc_drops\": %llu,\n", (unsigned long long)atomic_load(&st_spsc_drops));
    fprintf(j, "  \"bad_frames\": %llu,\n", (unsigned long long)atomic_load(&st_bad_frames));
    fprintf(j, "  \"gap_frames\": %llu,\n", (unsigned long long)atomic_load(&st_gap_frames));
    fprintf(j, "  \"gap_events\": %llu,\n", (unsigned long long)atomic_load(&st_gap_events));
    fprintf(j, "  \"dup\": %llu,\n", (unsigned long long)atomic_load(&st_dup));
    fprintf(j, "  \"ooo\": %llu,\n", (unsigned long long)atomic_load(&st_ooo));
    fprintf(j, "  \"time_ok\": %s,\n", atomic_load(&st_time_ok) ? "true" : "false");
    fprintf(j, "  \"cur_file\": \"%s\",\n", curfile);
    fprintf(j, "  \"cur_disk\": %d,\n", curdisk);
    fprintf(j, "  \"free_bytes\": [");
    for (int d = 0; d < g_ndisks; d++)
        fprintf(j, "%s%llu", d ? ", " : "", (unsigned long long)freeb[d]);
    fprintf(j, "],\n");
    fprintf(j, "  \"alarm\": \"%s\"\n", alarm);
    fprintf(j, "}\n");
    fclose(j);
    rename(tmp, g_status_path);

    /* kernel-side drop counter: AF_PACKET stats for raw/ring, UDP RcvbufErrors for udp.
     * (gap_frames from the VDIF frame# is the authoritative end-to-end loss metric.) */
    if (g_rx_mode == 0) {                         /* udp: socket-buffer overflow (delta) */
        static uint64_t base = 0; static int have_base = 0;
        uint64_t cur = udp_rcvbuf_errors();
        if (!have_base) { base = cur; have_base = 1; }
        atomic_store(&st_kernel_drops, cur >= base ? cur - base : 0);
    } else if (g_rx_mode == 3 && g_capture_fd >= 0) {   /* xdp: XDP_STATISTICS (cumulative) */
#ifdef HAVE_XDP
        struct xdp_statistics xs; socklen_t xl = sizeof xs;
        if (getsockopt(g_capture_fd, SOL_XDP, XDP_STATISTICS, &xs, &xl) == 0)
            atomic_store(&st_kernel_drops, xs.rx_dropped + xs.rx_ring_full +
                                           xs.rx_fill_ring_empty_descs + xs.rx_invalid_descs);
#endif
    } else if (g_rx_mode != 3 && g_capture_fd >= 0) {   /* raw/ring: AF_PACKET tp_drops (resets on read) */
        struct tpacket_stats_v3 ps; socklen_t pl = sizeof ps;
        if (getsockopt(g_capture_fd, SOL_PACKET, PACKET_STATISTICS, &ps, &pl) == 0)
            atomic_fetch_add(&st_kernel_drops, ps.tp_drops);
    }
}

/* ----------------------------- signal handling ------------------------------------- */
static void on_signal(int sig) { (void)sig; g_stop = 1; if (atomic_load(&st_state)==ST_RUNNING)
                                 atomic_store(&st_state, ST_STOPPED_SIGNAL); }

/* ----------------------------- argv parsing ---------------------------------------- */
static void parse_disks(char *csv) {
    g_ndisks = 0;
    for (char *tok = strtok(csv, ","); tok && g_ndisks < MAX_DISKS; tok = strtok(NULL, ","))
        g_disks[g_ndisks++] = strdup(tok);
}
static void usage(const char *p) {
    fprintf(stderr,
      "usage: %s [opts]\n"
      "  --iface IF           NIC (default enp1s0f0np0)\n"
      "  --dport N            VDIF UDP dest port (default 4001)\n"
      "  --disks a,b,c        sequential output mounts (default /mnt/vlbi0,1,2)\n"
      "  --secs-per-file N    file roll-over period (default 10)\n"
      "  --status PATH        status.json path (default /tmp/vdif_recorder_status.json)\n"
      "  --min-free-gb F      switch disk below this free (default 12)\n"
      "  --frames-per-slab N  must be multiple of 128 (default 1024)\n"
      "  --nslabs N           slab pool depth (default 128)\n"
      "  --block-mb N         ring block MiB (default 4)\n"
      "  --nblocks N          ring block count (default 256)\n"
      "  --cap-core N / --wr-core N   pin threads (default none)\n"
      "  --run-secs N         stop after N s (default 0 = forever)\n"
      "  --rx MODE            udp (default) | raw | ring | xdp  (xdp = AF_XDP zero-copy)\n"
      "  --xdp-queue N        AF_XDP: NIC RX queue to bind (default 0; use ethtool -L combined 1)\n"
      "  --xdp-copy           AF_XDP: XDP_COPY (default; CX-5 has no ZC multi-buffer)\n"
      "  --xdp-zc             AF_XDP: try XDP_ZEROCOPY first (ZC-multibuf-capable NICs)\n"
      "  --xdp-skb            AF_XDP: generic/SKB mode (for veth testing, not mlx5)\n"
      "  --xdp-busy           AF_XDP: busy-poll RX on cap-core (default: IRQ-driven, NAPI on IRQ core)\n"
      "  --buffered           disable O_DIRECT\n"
      "  --no-align           start immediately (don't wait for frame#==0)\n", p);
}

int main(int argc, char **argv) {
    /* defaults for disks */
    static char def_disks[] = "/mnt/vlbi0,/mnt/vlbi1,/mnt/vlbi2";
    char *disks_arg = def_disks;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--iface") && i+1<argc)          snprintf(g_iface, IFNAMSIZ, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--dport") && i+1<argc)          g_dport = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--disks") && i+1<argc)          disks_arg = argv[++i];
        else if (!strcmp(argv[i], "--secs-per-file") && i+1<argc)  g_secs_per_file = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--status") && i+1<argc)         snprintf(g_status_path, sizeof g_status_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--min-free-gb") && i+1<argc)    g_min_free_gb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames-per-slab") && i+1<argc)g_frames_per_slab = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nslabs") && i+1<argc)         g_nslabs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--block-mb") && i+1<argc)       g_block_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nblocks") && i+1<argc)        g_nblocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cap-core") && i+1<argc)       g_cap_core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wr-core") && i+1<argc)        g_wr_core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--run-secs") && i+1<argc)       g_run_secs = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rcvbuf-mb") && i+1<argc)      g_rcvbuf_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--busy-poll") && i+1<argc)      g_busy_poll_us = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rx") && i+1<argc) {
            i++;
            if      (!strcmp(argv[i], "udp"))  g_rx_mode = 0;
            else if (!strcmp(argv[i], "raw"))  g_rx_mode = 1;
            else if (!strcmp(argv[i], "ring")) g_rx_mode = 2;
            else if (!strcmp(argv[i], "xdp"))  g_rx_mode = 3;
            else { usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "--xdp-queue") && i+1<argc)      g_xdp_queue = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xdp-copy"))                   g_xdp_zerocopy = 0;
        else if (!strcmp(argv[i], "--xdp-zc"))                     g_xdp_zerocopy = 1;
        else if (!strcmp(argv[i], "--xdp-skb"))                    g_xdp_drv = 0;
        else if (!strcmp(argv[i], "--xdp-no-busy"))                g_xdp_busy = 0;
        else if (!strcmp(argv[i], "--xdp-busy"))                   g_xdp_busy = 1;
        else if (!strcmp(argv[i], "--buffered"))                   g_use_odirect = 0;
        else if (!strcmp(argv[i], "--no-align"))                   g_align_start = 0;
        else { usage(argv[0]); return 2; }
    }
    parse_disks(disks_arg);
    if (g_ndisks == 0) { fprintf(stderr, "no disks\n"); return 2; }
    if (g_frames_per_slab % ALIGN_FRAMES) {
        fprintf(stderr, "ERROR: --frames-per-slab (%d) must be a multiple of %u "
                "(O_DIRECT alignment invariant)\n", g_frames_per_slab, ALIGN_FRAMES);
        return 2;
    }
    g_slab_bytes = g_frames_per_slab * VDIF_FRAME_BYTES;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* lock memory so the ring + slab pool are never paged out */
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);

    /* slab pool: one big page-aligned arena + descriptors */
    if (posix_memalign((void **)&g_arena, PAGE, (size_t)g_nslabs * g_slab_bytes) != 0) {
        fprintf(stderr, "slab arena alloc failed (%d slabs x %d B)\n", g_nslabs, g_slab_bytes);
        return 1;
    }
    g_desc = calloc(g_nslabs, sizeof(slabdesc_t));
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* queues: capacity = next power of two > nslabs */
    uint32_t cap = 1; while (cap <= (uint32_t)g_nslabs) cap <<= 1;
    spsc_init(&q_free, cap);
    spsc_init(&q_full, cap);
    for (int i = 0; i < g_nslabs; i++) spsc_push(&q_free, i);

    const char *rxname = (g_rx_mode == 0) ? "udp" : (g_rx_mode == 1) ? "raw"
                       : (g_rx_mode == 2) ? "ring" : "xdp";
    ring_t ring; memset(&ring, 0, sizeof ring); ring.fd = -1;
    int is_udp = (g_rx_mode == 0);
    if (g_rx_mode == 2 && ring_setup(&ring) != 0) return 1;   /* TPACKET ring only for --rx ring */

    fprintf(stderr, "vdif_recorder: rx=%s iface=%s dport=%d slab=%dframes(%dB) pool=%d (~%.1f GiB) "
            "odirect=%d secs/file=%d\n",
            rxname, g_iface, g_dport, g_frames_per_slab, g_slab_bytes, g_nslabs,
            (double)g_nslabs * g_slab_bytes / (1<<30), g_use_odirect, g_secs_per_file);
    fprintf(stderr, "  disks:");
    for (int d = 0; d < g_ndisks; d++) fprintf(stderr, " %s", g_disks[d]);
    fprintf(stderr, "\n");
    refresh_free();

    pthread_t cap_th, wr_th;
    pthread_create(&wr_th, NULL, writer_thread, NULL);
    if      (g_rx_mode == 2) pthread_create(&cap_th, NULL, capture_ring,     &ring);
    else if (g_rx_mode == 3) pthread_create(&cap_th, NULL, capture_xdp,      NULL);
    else                     pthread_create(&cap_th, NULL, capture_recvmmsg, &is_udp);

    /* status loop */
    uint64_t prev_frames = 0, prev_bytes = 0;
    time_t   t0 = time(NULL);
    while (!g_stop) {
        struct timespec s = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&s, NULL);
        uint64_t fw = atomic_load(&st_frames_written);
        uint64_t bw = atomic_load(&st_bytes_written);
        double fps = (double)(fw - prev_frames);
        double mbs = (double)(bw - prev_bytes) / 1e6;
        prev_frames = fw; prev_bytes = bw;
        write_status(fps, mbs);

        pthread_mutex_lock(&st_lock);
        const char *fn = st_cur_file[0] ? strrchr(st_cur_file, '/') : NULL;
        char shortf[128]; snprintf(shortf, sizeof shortf, "%s", fn ? fn+1 : "(none)");
        double freeT = (g_ndisks ? (double)st_free_bytes[st_cur_disk] / 1e12 : 0);
        int disk = st_cur_disk;
        pthread_mutex_unlock(&st_lock);

        fprintf(stdout,
            "[%lds] fps=%.0f wr=%.0fMB/s drops(k/s/g)=%llu/%llu/%llu time=%s disk%d free=%.2fT file=%s\n",
            (long)(time(NULL)-t0), fps, mbs,
            (unsigned long long)atomic_load(&st_kernel_drops),
            (unsigned long long)atomic_load(&st_spsc_drops),
            (unsigned long long)atomic_load(&st_gap_frames),
            atomic_load(&st_time_ok) ? "OK" : "--", disk, freeT, shortf);
        fflush(stdout);

        if (g_run_secs > 0 && (time(NULL) - t0) >= g_run_secs) { g_stop = 1; }
    }

    pthread_join(cap_th, NULL);
    pthread_join(wr_th, NULL);
    write_status(0, 0);

    fprintf(stderr, "\n--- final: state=%s frames_written=%llu seen=%llu "
            "kernel_drops=%llu spsc_drops=%llu gap_frames=%llu dup=%llu bad=%llu ---\n",
            state_name(atomic_load(&st_state)),
            (unsigned long long)atomic_load(&st_frames_written),
            (unsigned long long)atomic_load(&st_frames_seen),
            (unsigned long long)atomic_load(&st_kernel_drops),
            (unsigned long long)atomic_load(&st_spsc_drops),
            (unsigned long long)atomic_load(&st_gap_frames),
            (unsigned long long)atomic_load(&st_dup),
            (unsigned long long)atomic_load(&st_bad_frames));
    return 0;
}
