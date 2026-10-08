/* A simulated M5 remote for the USB loader tests: the real loader state machine (loader.c) on stdin/stdout,
 * files kept in the directory given as argv[1] (a temporary file per transfer, committed by rename). The app is
 * "busy" (armed) while a file named BUSY exists there. RELOAD validates patterns.bin with the real pack reader and
 * requires config.json. Used by tests/test_remote_loader.py through pipes. HELLO ends with "board=m5
 * mac=02:00:00:00:00:05"; argv[2] "radr" = the build for the RADR hardware ("board=radr mac=02:00:00:00:00:0a"),
 * "noident" = firmware from before the board words. A file DROP or FLIP in the folder holding a byte offset of the
 * file being sent: that byte of it is lost on the wire, or arrives changed (a UART without flow control); the file is
 * removed as it fires, so the PC's second try goes through. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define READ _read
#else
#include <dirent.h>
#include <sys/select.h>
#include <unistd.h>
#define READ read
#endif

#include "loader.h"
#include "pack.h"

static const char *dir;
static FILE *tmp;
static char tmp_path[512];

static void path_of(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", dir, name);
}

static void send_line(void *ctx, const char *line) {
    (void)ctx;
    fputs(line, stdout);
    fflush(stdout);
}

static bool busy(void *ctx) {
    (void)ctx;
    char p[512];
    path_of(p, sizeof p, "BUSY");
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static uint32_t free_bytes(void *ctx) {
    (void)ctx;
    return 4u * 1024u * 1024u;
}

static bool open_tmp(void *ctx, const char *name, uint32_t size) {
    (void)ctx; (void)size;
    snprintf(tmp_path, sizeof tmp_path, "%s/.%s.part", dir, name);
    tmp = fopen(tmp_path, "wb");
    return tmp != NULL;
}

static bool write_tmp(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    return tmp && fwrite(d, 1, n, tmp) == n;
}

static bool commit(void *ctx, const char *name) {
    (void)ctx;
    char p[512];
    if (!tmp) return false;
    fclose(tmp);
    tmp = NULL;
    path_of(p, sizeof p, name);
    remove(p);
    return rename(tmp_path, p) == 0;
}

static void abort_tmp(void *ctx) {
    (void)ctx;
    if (tmp) { fclose(tmp); tmp = NULL; }
    remove(tmp_path);
}

static bool remove_file(void *ctx, const char *name) {
    (void)ctx;
    char p[512];
    path_of(p, sizeof p, name);
    return remove(p) == 0;
}

/* the simulator only needs to list the two files the PC sends */
static bool list(void *ctx, int index, char *name, size_t cap, uint32_t *size) {
    (void)ctx;
    static const char *const known[] = {"patterns.bin", "config.json"};
    int seen = 0;
    for (int k = 0; k < 2; k++) {
        char p[512];
        path_of(p, sizeof p, known[k]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fclose(f);
        if (seen++ == index) {
            snprintf(name, cap, "%s", known[k]);
            *size = (uint32_t)n;
            return true;
        }
    }
    return false;
}

static uint8_t packbuf[1 << 20];

static bool reload(void *ctx, char *reason, size_t cap) {
    (void)ctx;
    char p[512];
    path_of(p, sizeof p, "patterns.bin");
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(reason, cap, "no patterns.bin"); return false; }
    size_t n = fread(packbuf, 1, sizeof packbuf, f);
    fclose(f);
    pack_t pk;
    int err = pack_open(&pk, packbuf, n);
    if (err != PACK_OK) { snprintf(reason, cap, "patterns.bin: %s", pack_error_text(err)); return false; }
    path_of(p, sizeof p, "config.json");
    f = fopen(p, "rb");
    if (!f) { snprintf(reason, cap, "no config.json"); return false; }
    fclose(f);
    return true;
}

static bool sim_radr = false;
static void sim_ident(void *ctx, char *buf, size_t cap) {
    (void)ctx;
    snprintf(buf, cap, "board=%s mac=02:00:00:00:00:%s", sim_radr ? "radr" : "m5", sim_radr ? "0a" : "05");
}

/* the byte offset in a fault file (DROP / FLIP), or -1 */
static long fault_at(const char *name) {
    char p[512];
    path_of(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    long k = -1;
    if (fscanf(f, "%ld", &k) != 1) k = -1;
    fclose(f);
    return k;
}

static void fault_done(const char *name) {
    char p[512];
    path_of(p, sizeof p, name);
    remove(p);
}

/* a transfer's bytes on the wire: DROP loses one, FLIP changes one */
static int wire(const loader_t *l, uint8_t *buf, int n) {
    if (!l->receiving) return n;
    long k = fault_at("DROP");
    if (k >= (long)l->received && k < (long)l->received + n) {
        int i = (int)(k - (long)l->received);
        memmove(buf + i, buf + i + 1, (size_t)(n - i - 1));
        fault_done("DROP");
        return n - 1;
    }
    k = fault_at("FLIP");
    if (k >= (long)l->received && k < (long)l->received + n) {
        buf[k - (long)l->received] ^= 0x5A;
        fault_done("FLIP");
    }
    return n;
}

static uint32_t now_ms(void) {                     /* wall time, so a stalled transfer times out as on the remote */
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL);
#endif
}

/* true when stdin has bytes (or is closed) within ms: the loop polls the loader meanwhile, as the remote's does */
static int input_ready(int ms) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    for (int waited = 0;; waited += 5) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail) return 1;   /* (not a pipe, or closed: read) */
        if (waited >= ms) return 0;
        Sleep(5);
    }
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(1, &fds, NULL, NULL, &tv) != 0;
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    dir = argv[1];
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    loader_io_t io = {NULL, send_line, busy, free_bytes, open_tmp, write_tmp, commit, abort_tmp, remove_file, list,
                      reload, sim_ident};
    if (argc >= 3 && !strcmp(argv[2], "noident")) io.ident = NULL;       /* firmware before the board words */
    if (argc >= 3 && !strcmp(argv[2], "radr")) sim_radr = true;           /* the RADR hardware's build */
    loader_t l;
    loader_init(&l, &io);
    uint8_t buf[4096];
    for (;;) {
        if (!input_ready(50)) {
            loader_poll(&l, now_ms());
            continue;
        }
        int n = READ(0, buf, sizeof buf);
        if (n <= 0) break;
        n = wire(&l, buf, n);
        loader_feed(&l, buf, (size_t)n, now_ms());
        loader_poll(&l, now_ms());
    }
    if (tmp) abort_tmp(NULL);
    return 0;
}
