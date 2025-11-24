/*
 * Benchmark comparison program
 * Compares performance with and without zero-copy optimizations
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <pthread.h>

#define TRANSFER_SIZE (100 * 1024 * 1024)  /* 100 MB */
#define CHUNK_SIZE (64 * 1024)             /* 64 KB */

static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void benchmark_send_recv(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receiver */
        close(sv[0]);

        char *buf = malloc(CHUNK_SIZE);
        size_t total = 0;

        while (total < TRANSFER_SIZE) {
            ssize_t n = recv(sv[1], buf, CHUNK_SIZE, 0);
            if (n <= 0) break;
            total += n;
        }

        free(buf);
        close(sv[1]);
        exit(0);
    }

    /* Parent: sender */
    close(sv[1]);

    char *buf = malloc(CHUNK_SIZE);
    memset(buf, 'X', CHUNK_SIZE);

    size_t total = 0;
    double start = get_time_ms();

    while (total < TRANSFER_SIZE) {
        size_t to_send = (TRANSFER_SIZE - total < CHUNK_SIZE) ?
                         (TRANSFER_SIZE - total) : CHUNK_SIZE;
        ssize_t n = send(sv[0], buf, to_send, 0);
        if (n <= 0) break;
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Transferred: %.1f MB in %.2f ms\n",
           total / (1024.0 * 1024.0), elapsed);
    printf("Throughput:  %.2f MB/s\n", throughput);
    printf("Bandwidth:   %.2f Gbps\n", throughput * 8 / 1000.0);

    free(buf);
    close(sv[0]);
    wait(NULL);
}

int main(void)
{
    const char *preload = getenv("LD_PRELOAD");

    if (preload) {
        if (strstr(preload, "zerocopy_v2"))
            printf("Using: io_uring zero-copy (v2)\n");
        else if (strstr(preload, "zerocopy"))
            printf("Using: splice/sendfile zero-copy (v1)\n");
        else
            printf("Using: Unknown LD_PRELOAD\n");
    } else {
        printf("Using: Standard syscalls (no LD_PRELOAD)\n");
    }

    printf("Transfer size: %d MB\n", TRANSFER_SIZE / (1024 * 1024));
    printf("Chunk size: %d KB\n\n", CHUNK_SIZE / 1024);

    benchmark_send_recv();

    return 0;
}
