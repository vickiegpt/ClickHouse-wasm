/*
 * Synthetic HFT workload for the zerocopy/io_uring preload experiments.
 *
 * Pipeline:
 *   UDP market data receive -> lightweight compute -> audit pwrite -> TCP send
 *
 * Run baseline:
 *   ./synthetic_hft -n 50000
 *
 * Run with zerocopy preload:
 *   ZEROCOPY_THRESHOLD=1 LD_PRELOAD=./libclickhouse_zerocopy_v2.so ./synthetic_hft -n 50000
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TICKS 50000UL
#define TICK_PAYLOAD_SIZE 192
#define DEFAULT_AUDIT_FILE "/tmp/synthetic_hft_audit.log"

struct market_tick {
    uint64_t seq;
    uint64_t tx_ts_ns;
    double bid;
    double ask;
    uint32_t bid_qty;
    uint32_t ask_qty;
    char payload[TICK_PAYLOAD_SIZE];
};

struct order_event {
    uint64_t seq;
    uint64_t rx_ts_ns;
    uint64_t done_ts_ns;
    double mid_price;
    double signal;
    uint32_t action;
    uint32_t reserved;
};

struct audit_record {
    uint64_t seq;
    uint64_t tx_ts_ns;
    uint64_t rx_ts_ns;
    double mid_price;
    double signal;
    uint32_t action;
    uint32_t reserved;
};

struct feeder_args {
    uint16_t udp_port;
    uint64_t ticks;
};

struct sink_args {
    int listen_fd;
    uint64_t bytes_received;
    int status;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *lhs, const void *rhs)
{
    uint64_t a = *(const uint64_t *)lhs;
    uint64_t b = *(const uint64_t *)rhs;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static ssize_t raw_sendto_once(int fd, const void *buf, size_t len, int flags,
                               const struct sockaddr *addr, socklen_t addrlen)
{
    return syscall(SYS_sendto, fd, buf, len, flags, addr, addrlen);
}

static ssize_t raw_read_loop(int fd, void *buf, size_t len)
{
    ssize_t total = 0;

    while (1) {
        ssize_t n = syscall(SYS_read, fd, buf, len);
        if (n <= 0)
            return total > 0 ? total : n;
        total += n;
    }
}

static int set_tcp_nodelay(int fd)
{
    int enabled = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

static int bind_loopback(int sockfd, uint16_t *port_out, int type)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    (void)type;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    if (getsockname(sockfd, (struct sockaddr *)&addr, &addrlen) < 0)
        return -1;

    *port_out = ntohs(addr.sin_port);
    return 0;
}

static void fill_tick(struct market_tick *tick, uint64_t seq)
{
    memset(tick, 0, sizeof(*tick));
    tick->seq = seq;
    tick->tx_ts_ns = now_ns();
    tick->bid = 100.0 + (double)(seq % 17) * 0.01;
    tick->ask = tick->bid + 0.02;
    tick->bid_qty = 100 + (seq % 13);
    tick->ask_qty = 120 + (seq % 11);
    memset(tick->payload, (int)('A' + (seq % 26)), sizeof(tick->payload));
}

static void *feeder_main(void *arg)
{
    struct feeder_args *cfg = arg;
    struct sockaddr_in dst;
    struct market_tick tick;
    int fd;
    int sndbuf = 8 * 1024 * 1024;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("feeder socket");
        return (void *)(uintptr_t)1;
    }

    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(cfg->udp_port);

    for (uint64_t i = 0; i < cfg->ticks; i++) {
        fill_tick(&tick, i);
        if (raw_sendto_once(fd, &tick, sizeof(tick), 0,
                            (const struct sockaddr *)&dst, sizeof(dst)) != (ssize_t)sizeof(tick)) {
            perror("raw_sendto");
            close(fd);
            return (void *)(uintptr_t)1;
        }
        if ((i & 255U) == 255U)
            sched_yield();
    }

    close(fd);
    return NULL;
}

static void *sink_main(void *arg)
{
    struct sink_args *cfg = arg;
    char buf[4096];
    int conn_fd;
    ssize_t n;

    conn_fd = accept(cfg->listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("accept");
        cfg->status = 1;
        return NULL;
    }

    n = raw_read_loop(conn_fd, buf, sizeof(buf));
    if (n < 0) {
        perror("sink read");
        cfg->status = 1;
    } else {
        cfg->bytes_received = (uint64_t)n;
        cfg->status = 0;
    }

    close(conn_fd);
    close(cfg->listen_fd);
    return NULL;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-n ticks] [-a audit_file]\n", prog);
    fprintf(stderr, "  -n <ticks>       Number of market data ticks (default: %lu)\n",
            (unsigned long)DEFAULT_TICKS);
    fprintf(stderr, "  -a <audit_file>  Audit log output (default: %s)\n",
            DEFAULT_AUDIT_FILE);
}

int main(int argc, char **argv)
{
    const char *audit_path = DEFAULT_AUDIT_FILE;
    uint64_t ticks = DEFAULT_TICKS;
    uint64_t *latencies = NULL;
    struct feeder_args feeder_cfg;
    struct sink_args sink_cfg;
    pthread_t feeder_thread;
    pthread_t sink_thread;
    struct sockaddr_in tcp_addr;
    struct sockaddr_in udp_src;
    socklen_t udp_srclen;
    int udp_fd = -1;
    int tcp_listener = -1;
    int tcp_client = -1;
    int audit_fd = -1;
    uint16_t udp_port = 0;
    uint16_t tcp_port = 0;
    uint64_t first_rx_ns = 0;
    uint64_t last_done_ns = 0;
    uint64_t processed = 0;
    double ema = 100.0;
    int opt;
    int sockbuf = 8 * 1024 * 1024;
    struct pollfd pfd;

    while ((opt = getopt(argc, argv, "n:a:h")) != -1) {
        switch (opt) {
        case 'n':
            ticks = strtoull(optarg, NULL, 10);
            break;
        case 'a':
            audit_path = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    latencies = calloc((size_t)ticks, sizeof(*latencies));
    if (!latencies) {
        perror("calloc");
        return 1;
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("udp socket");
        free(latencies);
        return 1;
    }
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
    if (bind_loopback(udp_fd, &udp_port, SOCK_DGRAM) < 0) {
        perror("bind udp");
        close(udp_fd);
        free(latencies);
        return 1;
    }

    tcp_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listener < 0) {
        perror("tcp listener");
        close(udp_fd);
        free(latencies);
        return 1;
    }
    if (bind_loopback(tcp_listener, &tcp_port, SOCK_STREAM) < 0) {
        perror("bind tcp");
        close(tcp_listener);
        close(udp_fd);
        free(latencies);
        return 1;
    }
    if (listen(tcp_listener, 1) < 0) {
        perror("listen");
        close(tcp_listener);
        close(udp_fd);
        free(latencies);
        return 1;
    }

    sink_cfg.listen_fd = tcp_listener;
    sink_cfg.bytes_received = 0;
    sink_cfg.status = 0;
    if (pthread_create(&sink_thread, NULL, sink_main, &sink_cfg) != 0) {
        perror("pthread_create sink");
        close(tcp_listener);
        close(udp_fd);
        free(latencies);
        return 1;
    }

    tcp_client = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_client < 0) {
        perror("tcp client");
        return 1;
    }
    set_tcp_nodelay(tcp_client);

    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tcp_addr.sin_port = htons(tcp_port);
    if (connect(tcp_client, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
        perror("connect");
        return 1;
    }

    audit_fd = open(audit_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (audit_fd < 0) {
        perror("open audit file");
        return 1;
    }

    feeder_cfg.udp_port = udp_port;
    feeder_cfg.ticks = ticks;
    if (pthread_create(&feeder_thread, NULL, feeder_main, &feeder_cfg) != 0) {
        perror("pthread_create feeder");
        return 1;
    }

    pfd.fd = udp_fd;
    pfd.events = POLLIN;

    for (uint64_t i = 0; i < ticks; i++) {
        struct market_tick tick;
        struct audit_record audit;
        struct order_event event;
        uint64_t rx_ns;
        uint64_t done_ns;
        double mid;
        double signal;
        uint32_t action;
        ssize_t sent;

        udp_srclen = sizeof(udp_src);
        int poll_ret = poll(&pfd, 1, 2000);
        if (poll_ret == 0) {
            fprintf(stderr, "poll timeout after %lu ticks\n",
                    (unsigned long)processed);
            break;
        }
        if (poll_ret < 0) {
            perror("poll");
            return 1;
        }

        if (recvfrom(udp_fd, &tick, sizeof(tick), 0,
                     (struct sockaddr *)&udp_src, &udp_srclen) != (ssize_t)sizeof(tick)) {
            perror("recvfrom");
            return 1;
        }

        rx_ns = now_ns();
        if (!first_rx_ns)
            first_rx_ns = rx_ns;

        mid = (tick.bid + tick.ask) * 0.5;
        ema = ema * 0.95 + mid * 0.05;
        signal = mid - ema;
        action = fabs(signal) > 0.01 ? 1U : 0U;

        memset(&audit, 0, sizeof(audit));
        audit.seq = tick.seq;
        audit.tx_ts_ns = tick.tx_ts_ns;
        audit.rx_ts_ns = rx_ns;
        audit.mid_price = mid;
        audit.signal = signal;
        audit.action = action;

        if (pwrite(audit_fd, &audit, sizeof(audit),
                   (off_t)(processed * sizeof(audit))) != (ssize_t)sizeof(audit)) {
            perror("pwrite");
            return 1;
        }

        memset(&event, 0, sizeof(event));
        event.seq = tick.seq;
        event.rx_ts_ns = rx_ns;
        event.mid_price = mid;
        event.signal = signal;
        event.action = action;

        sent = send(tcp_client, &event, sizeof(event), MSG_NOSIGNAL);
        if (sent != (ssize_t)sizeof(event)) {
            perror("send");
            return 1;
        }

        done_ns = now_ns();
        event.done_ts_ns = done_ns;
        latencies[processed] = done_ns - tick.tx_ts_ns;
        last_done_ns = done_ns;
        processed++;
    }

    pthread_join(feeder_thread, NULL);
    shutdown(tcp_client, SHUT_WR);
    close(tcp_client);
    close(udp_fd);
    close(audit_fd);
    pthread_join(sink_thread, NULL);

    if (processed == 0) {
        fprintf(stderr, "No ticks processed\n");
        free(latencies);
        return 1;
    }

    qsort(latencies, (size_t)processed, sizeof(*latencies), cmp_u64);

    {
        double seconds = (double)(last_done_ns - first_rx_ns) / 1e9;
        double throughput = seconds > 0.0 ? (double)processed / seconds : 0.0;
        double avg_ns = 0.0;
        uint64_t p50 = latencies[processed / 2];
        uint64_t p99 = latencies[(processed * 99) / 100];
        uint64_t p999 = latencies[(processed * 999) / 1000];

        for (uint64_t i = 0; i < processed; i++)
            avg_ns += (double)latencies[i];
        avg_ns /= (double)processed;

        printf("========================================\n");
        printf("Synthetic HFT Workload\n");
        printf("========================================\n");
        printf("Ticks processed:      %lu / %lu\n",
               (unsigned long)processed, (unsigned long)ticks);
        printf("Dropped ticks:        %lu\n",
               (unsigned long)(ticks - processed));
        printf("Audit file:           %s\n", audit_path);
        printf("Forwarded bytes:      %lu\n", (unsigned long)sink_cfg.bytes_received);
        printf("Throughput:           %.2f ticks/s (%.2f M ticks/s)\n",
               throughput, throughput / 1e6);
        printf("Avg latency:          %.1f ns\n", avg_ns);
        printf("P50 latency:          %lu ns\n", (unsigned long)p50);
        printf("P99 latency:          %lu ns\n", (unsigned long)p99);
        printf("P99.9 latency:        %lu ns\n", (unsigned long)p999);
        printf("LD_PRELOAD:           %s\n", getenv("LD_PRELOAD") ? getenv("LD_PRELOAD") : "(none)");
        printf("ZEROCOPY_THRESHOLD:   %s\n",
               getenv("ZEROCOPY_THRESHOLD") ? getenv("ZEROCOPY_THRESHOLD") : "(default)");
    }

    free(latencies);
    return sink_cfg.status;
}
