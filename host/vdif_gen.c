/*
 * vdif_gen.c -- synthetic VDIF UDP source for testing vdif_recorder WITHOUT the FPGA.
 *
 * Emits exactly the on-wire shape the RFSoC produces: UDP datagrams whose payload is
 * one 8032-byte VDIF frame (32-B header + 8000-B data), frame# 0..127999 then sec++,
 * at a paced rate (default 128000 fps). Optionally injects frame# gaps so you can
 * verify the recorder's gap accounting. Linux only (sendmmsg + clock_nanosleep).
 *
 * Typical veth self-test (cross a real wire so AF_PACKET sees the frames):
 *   sudo ip netns add ns_tx
 *   sudo ip link add veth_tx type veth peer name veth_rx
 *   sudo ip link set veth_tx netns ns_tx
 *   sudo ip netns exec ns_tx ip addr add 10.17.16.60/24 dev veth_tx
 *   sudo ip netns exec ns_tx ip link set veth_tx up mtu 9000
 *   sudo ip netns exec ns_tx ip link set lo up
 *   sudo ip addr add 10.17.16.1/24 dev veth_rx
 *   sudo ip link set veth_rx up mtu 9000
 *   # receiver (root ns):
 *   sudo ./vdif_recorder --iface veth_rx --dport 4001 --disks /tmp/d0,/tmp/d1 --secs-per-file 2
 *   # sender (tx ns):
 *   sudo ip netns exec ns_tx ./vdif_gen --dst 10.17.16.1 --port 4001 --secs 20
 *
 * Build:  make vdif_gen
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define VDIF_FRAME_BYTES  8032u
#define FPS               128000u
#define BATCH             64        /* datagrams per sendmmsg */

static void put_le32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}

/* build the 32-byte VDIF header for (sec, frame#) matching the RFSoC format:
 *   data_type=1 (complex), bps-1=1 (2-bit), frame_length field=1004 (=8032 B). */
static void fill_vdif_header(uint8_t *v, uint32_t sec, uint32_t frame,
                             int ref_epoch, int thread, int station) {
    uint32_t w0 = sec & 0x3FFFFFFFu;                              /* invalid=0 */
    uint32_t w1 = (frame & 0xFFFFFFu) | ((uint32_t)(ref_epoch & 0x3F) << 24);
    uint32_t w2 = (1004u & 0xFFFFFFu);                            /* frame_len/8; log2nch=0 */
    uint32_t w3 = (uint32_t)(station & 0xFFFF)
                | ((uint32_t)(thread & 0x3FF) << 16)
                | (1u << 26)                                     /* bps-1 = 1 */
                | (1u << 31);                                    /* data_type = 1 (complex) */
    put_le32(v+0,w0); put_le32(v+4,w1); put_le32(v+8,w2); put_le32(v+12,w3);
    memset(v+16, 0, 16);                                         /* words 4-7 (EDV) = 0 */
}

static uint32_t vdif_epoch_now(uint32_t *sec_out) {
    time_t t = time(NULL);
    struct tm g; gmtime_r(&t, &g);
    int half = (g.tm_mon < 6) ? 0 : 1;
    int ref_epoch = (g.tm_year + 1900 - 2000) * 2 + half;
    struct tm e; memset(&e,0,sizeof e);
    e.tm_year = (2000 + ref_epoch/2) - 1900;
    e.tm_mon  = half ? 6 : 0; e.tm_mday = 1;
    time_t base = timegm(&e);
    *sec_out = (uint32_t)(t - base);
    return ref_epoch;
}

int main(int argc, char **argv) {
    const char *dst = "10.17.16.1";
    int port = 4001, run_secs = 10, thread = 0, station = 0;
    long rate = FPS;                 /* frames/sec */
    long gap_period = 0;             /* if >0, skip one frame# every N frames */

    for (int i=1;i<argc;i++){
        if      (!strcmp(argv[i],"--dst")&&i+1<argc)        dst=argv[++i];
        else if (!strcmp(argv[i],"--port")&&i+1<argc)       port=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--secs")&&i+1<argc)       run_secs=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--rate")&&i+1<argc)       rate=atol(argv[++i]);
        else if (!strcmp(argv[i],"--gap-period")&&i+1<argc) gap_period=atol(argv[++i]);
        else if (!strcmp(argv[i],"--thread")&&i+1<argc)     thread=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--station")&&i+1<argc)    station=atoi(argv[++i]);
        else { fprintf(stderr,"usage: %s --dst IP --port N --secs N [--rate fps] "
                       "[--gap-period N] [--thread N] [--station N]\n", argv[0]); return 2; }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd<0){ perror("socket"); return 1; }
    struct sockaddr_in sa; memset(&sa,0,sizeof sa);
    sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if (inet_pton(AF_INET,dst,&sa.sin_addr)!=1){ fprintf(stderr,"bad --dst\n"); return 1; }
    if (connect(fd,(struct sockaddr*)&sa,sizeof sa)!=0){ perror("connect"); return 1; }
    int sndbuf=16*1024*1024; setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof sndbuf);

    /* one reusable payload buffer per batch slot */
    static uint8_t buf[BATCH][VDIF_FRAME_BYTES];
    struct mmsghdr msgs[BATCH];
    struct iovec   iov[BATCH];
    for (int b=0;b<BATCH;b++){
        for (size_t k=VDIF_FRAME_BYTES-8; k<VDIF_FRAME_BYTES; k+=8) /* sparse fill */
            buf[b][k] = (uint8_t)(b*131+k);
        iov[b].iov_base=buf[b]; iov[b].iov_len=VDIF_FRAME_BYTES;
        memset(&msgs[b],0,sizeof msgs[b]);
        msgs[b].msg_hdr.msg_iov=&iov[b]; msgs[b].msg_hdr.msg_iovlen=1;
    }

    uint32_t sec, frame=0; int ref_epoch = vdif_epoch_now(&sec);
    long total = (long)rate * run_secs;
    long sent = 0, gapctr = 0; uint64_t gaps_injected = 0;

    /* pace in 1 ms ticks: send rate/1000 frames per tick */
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    long per_ms = rate/1000; if (per_ms<1) per_ms=1;

    fprintf(stderr,"vdif_gen -> %s:%d  rate=%ld fps  secs=%d  thread=%d  gap_period=%ld\n",
            dst,port,rate,run_secs,thread,gap_period);

    while (sent < total) {
        long this_tick = per_ms;
        while (this_tick > 0) {
            int n = (this_tick < BATCH) ? (int)this_tick : BATCH;
            for (int b=0;b<n;b++){
                if (gap_period>0 && ++gapctr>=gap_period){ /* skip one frame# = a gap */
                    frame++; if(frame>=FPS){frame=0;sec++;} gapctr=0; gaps_injected++;
                }
                fill_vdif_header(buf[b], sec, frame, ref_epoch, thread, station);
                frame++; if(frame>=FPS){frame=0;sec++;}
            }
            /* send ALL n filled frames, retrying partial sends so no frame# is
             * ever burned (a partial send must not skip the unsent frame numbers). */
            int off = 0;
            while (off < n) {
                int r = sendmmsg(fd, msgs+off, n-off, 0);
                if (r<0){ if(errno==ENOBUFS||errno==EAGAIN){ continue; } perror("sendmmsg"); return 1; }
                off += r;
            }
            this_tick -= n; sent += n;
            if (sent>=total) break;
        }
        /* sleep to the next 1 ms boundary */
        t.tv_nsec += 1000000; if (t.tv_nsec>=1000000000){ t.tv_nsec-=1000000000; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }
    fprintf(stderr,"vdif_gen done: sent=%ld frames, gaps_injected=%llu\n",
            sent,(unsigned long long)gaps_injected);
    return 0;
}
