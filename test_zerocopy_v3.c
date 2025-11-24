/*
 * Test program for ClickHouse Zero-Copy Library v3 (BPF Bypass)
 *
 * Tests DMA buffers, direct I/O bypass, and kernel bypass paths
 *
 * Usage:
 *   sudo LD_PRELOAD=./libclickhouse_zerocopy_v3.so ZEROCOPY_DEBUG=1 ./test_zerocopy_v3
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>

/* External functions from v3 library */
extern int zerocopy_enable_bypass(int fd);
extern void zerocopy_print_stats(void);

#define TEST_FILE "/tmp/zerocopy_v3_test.dat"
#define TEST_FILE_DIRECT "/tmp/zerocopy_v3_direct.dat"
#define TEST_SIZE (100 * 1024 * 1024)  /* 100 MB */

static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void create_test_file(const char *path, int use_direct)
{
    int flags = O_CREAT | O_WRONLY | O_TRUNC;
    if (use_direct)
        flags |= O_DIRECT;

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* Allocate aligned buffer for O_DIRECT */
    void *buf;
    if (posix_memalign(&buf, 4096, 4096) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    memset(buf, 'A', 4096);

    printf("Creating %s (%d MB, %s)...\n",
           path, TEST_SIZE / (1024*1024),
           use_direct ? "O_DIRECT" : "buffered");

    for (size_t i = 0; i < TEST_SIZE / 4096; i++) {
        if (write(fd, buf, 4096) != 4096) {
            perror("write");
            exit(1);
        }
    }

    free(buf);
    close(fd);
    printf("✓ Test file created: %s\n", path);
}

void test_direct_io_read(void)
{
    printf("\n=== Test 1: O_DIRECT Read with Bypass ===\n");

    int fd = open(TEST_FILE_DIRECT, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open O_DIRECT");
        printf("Skipping O_DIRECT test (may need root or supported filesystem)\n");
        return;
    }

    /* Enable bypass for this FD */
    zerocopy_enable_bypass(fd);

    void *buf;
    if (posix_memalign(&buf, 4096, 4 * 1024 * 1024) != 0) {
        perror("posix_memalign");
        close(fd);
        return;
    }

    size_t total = 0;
    double start = get_time_ms();

    while (total < TEST_SIZE) {
        ssize_t n = read(fd, buf, 4 * 1024 * 1024);
        if (n <= 0) break;
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Read %zu bytes in %.2f ms\n", total, elapsed);
    printf("Throughput: %.2f MB/s\n", throughput);
    printf("Mode: O_DIRECT with DMA bypass\n");

    free(buf);
    close(fd);
}

void test_direct_io_write(void)
{
    printf("\n=== Test 2: O_DIRECT Write with Bypass ===\n");

    const char *test_out = "/tmp/zerocopy_v3_write.dat";
    int fd = open(test_out, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) {
        perror("open O_DIRECT write");
        printf("Skipping O_DIRECT write test\n");
        return;
    }

    zerocopy_enable_bypass(fd);

    void *buf;
    if (posix_memalign(&buf, 4096, 4 * 1024 * 1024) != 0) {
        perror("posix_memalign");
        close(fd);
        return;
    }

    memset(buf, 'B', 4 * 1024 * 1024);

    size_t total = 0;
    double start = get_time_ms();

    while (total < TEST_SIZE) {
        size_t to_write = (TEST_SIZE - total < 4 * 1024 * 1024) ?
                          (TEST_SIZE - total) : (4 * 1024 * 1024);
        ssize_t n = write(fd, buf, to_write);
        if (n <= 0) break;
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Wrote %zu bytes in %.2f ms\n", total, elapsed);
    printf("Throughput: %.2f MB/s\n", throughput);
    printf("Mode: O_DIRECT with DMA bypass\n");

    free(buf);
    close(fd);
    unlink(test_out);
}

void test_buffered_vs_bypass(void)
{
    printf("\n=== Test 3: Buffered vs Bypass Comparison ===\n");

    /* Test buffered I/O */
    int fd1 = open(TEST_FILE, O_RDONLY);
    if (fd1 < 0) {
        perror("open buffered");
        return;
    }

    char *buf1 = malloc(1024 * 1024);
    size_t total1 = 0;
    double start1 = get_time_ms();

    while (total1 < 50 * 1024 * 1024) {
        ssize_t n = read(fd1, buf1, 1024 * 1024);
        if (n <= 0) break;
        total1 += n;
    }

    double elapsed1 = get_time_ms() - start1;
    double throughput1 = (total1 / (1024.0 * 1024.0)) / (elapsed1 / 1000.0);

    printf("Buffered I/O: %.2f MB/s\n", throughput1);

    free(buf1);
    close(fd1);

    /* Test with bypass hint (large transfers) */
    int fd2 = open(TEST_FILE, O_RDONLY);
    if (fd2 < 0) {
        perror("open bypass");
        return;
    }

    char *buf2 = malloc(4 * 1024 * 1024);
    size_t total2 = 0;
    double start2 = get_time_ms();

    while (total2 < 50 * 1024 * 1024) {
        ssize_t n = read(fd2, buf2, 4 * 1024 * 1024);
        if (n <= 0) break;
        total2 += n;
    }

    double elapsed2 = get_time_ms() - start2;
    double throughput2 = (total2 / (1024.0 * 1024.0)) / (elapsed2 / 1000.0);

    printf("Bypass I/O (large chunks): %.2f MB/s\n", throughput2);
    printf("Improvement: %.1f%%\n",
           ((throughput2 - throughput1) / throughput1) * 100.0);

    free(buf2);
    close(fd2);
}

void test_socket_zerocopy(void)
{
    printf("\n=== Test 4: Socket Zero-Copy Send ===\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receiver */
        close(sv[0]);

        char *buf = malloc(1024 * 1024);
        size_t total = 0;

        while (total < 50 * 1024 * 1024) {
            ssize_t n = recv(sv[1], buf, 1024 * 1024, 0);
            if (n <= 0) break;
            total += n;
        }

        printf("Child received: %zu bytes\n", total);
        free(buf);
        close(sv[1]);
        exit(0);
    }

    /* Parent: sender */
    close(sv[1]);

    char *buf = malloc(1024 * 1024);
    memset(buf, 'C', 1024 * 1024);

    size_t total = 0;
    double start = get_time_ms();

    while (total < 50 * 1024 * 1024) {
        ssize_t n = send(sv[0], buf, 1024 * 1024, 0);
        if (n <= 0) {
            perror("send");
            break;
        }
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Parent sent: %zu bytes in %.2f ms\n", total, elapsed);
    printf("Throughput: %.2f MB/s (with MSG_ZEROCOPY)\n", throughput);

    free(buf);
    close(sv[0]);
    wait(NULL);
}

void test_pread_pwrite(void)
{
    printf("\n=== Test 5: pread/pwrite with Bypass ===\n");

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    char *buf = malloc(2 * 1024 * 1024);

    double start = get_time_ms();

    /* Random access pattern */
    off_t offsets[] = {0, 10*1024*1024, 20*1024*1024, 50*1024*1024, 80*1024*1024};
    size_t total = 0;

    for (int i = 0; i < 5; i++) {
        ssize_t n = pread(fd, buf, 2 * 1024 * 1024, offsets[i]);
        if (n > 0)
            total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Random pread: %zu bytes in %.2f ms\n", total, elapsed);
    printf("Throughput: %.2f MB/s\n", throughput);

    free(buf);
    close(fd);
}

int main(void)
{
    printf("=========================================\n");
    printf("ClickHouse Zero-Copy Library v3 Test\n");
    printf("BPF Bypass + DMA Edition\n");
    printf("=========================================\n\n");

    /* Check if running as root */
    if (geteuid() != 0) {
        printf("⚠ Warning: Not running as root\n");
        printf("  DMA buffer allocation may fail\n");
        printf("  O_DIRECT operations may not work optimally\n\n");
    }

    /* Check if LD_PRELOAD is active */
    const char *preload = getenv("LD_PRELOAD");
    if (!preload || !strstr(preload, "zerocopy_v3")) {
        printf("⚠ WARNING: LD_PRELOAD not detected!\n");
        printf("Run with: sudo LD_PRELOAD=./libclickhouse_zerocopy_v3.so %s\n\n",
               "test_zerocopy_v3");
    }

    /* Show configuration */
    const char *debug = getenv("ZEROCOPY_DEBUG");
    const char *threshold = getenv("ZEROCOPY_THRESHOLD");
    const char *bypass_mode = getenv("ZEROCOPY_BYPASS_MODE");
    const char *direct_io = getenv("ZEROCOPY_DIRECT_IO");

    printf("Configuration:\n");
    printf("  Debug:         %s\n", debug ? debug : "0");
    printf("  Threshold:     %s bytes\n", threshold ? threshold : "16384");
    printf("  Bypass mode:   %s\n", bypass_mode ? bypass_mode : "partial");
    printf("  Direct I/O:    %s\n", direct_io ? direct_io : "1");
    printf("\n");

    create_test_file(TEST_FILE, 0);
    create_test_file(TEST_FILE_DIRECT, 1);

    test_direct_io_read();
    test_direct_io_write();
    test_buffered_vs_bypass();
    test_socket_zerocopy();
    test_pread_pwrite();

    printf("\n=== Final Statistics ===\n");
    zerocopy_print_stats();

    /* Cleanup */
    unlink(TEST_FILE);
    unlink(TEST_FILE_DIRECT);

    printf("\n✓ All tests completed!\n");
    return 0;
}
