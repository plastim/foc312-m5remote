/* Device side of the M5 remote's USB loader: the PC (stimengine/remote/loader.py) sends the pattern pack and the
 * settings file over the USB serial port. Transport-agnostic: feed it received bytes, it answers through
 * callbacks and stores files through callbacks.
 *
 * Protocol (text lines end in '\n'; names are [a-z0-9._-], 1-31 chars):
 *   HELLO                  -> OK stim-remote 1 <free bytes>           | ERR busy
 *   PUT <name> <size> <crc32 hex>
 *                          -> READY <chunk>  then <size> raw bytes follow; after every <chunk> bytes and at the
 *                             end the device answers ACK <bytes so far>; at the end it checks the CRC and
 *                             answers OK (the file replaces the old one) or ERR crc (the old one stays)
 *   LIST                   -> FILE <name> <size> ... END
 *   DEL <name>             -> OK | ERR ...
 *   RELOAD                 -> OK | ERR <reason>                        the app re-reads pack + settings
 * Every command answers ERR busy while the app is busy (output armed): nothing changes under a running session.
 * A transfer that stalls for LOADER_TIMEOUT_MS is aborted (the old file stays). */
#ifndef LOADER_H
#define LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOADER_CHUNK 1024
#define LOADER_TIMEOUT_MS 5000
#define LOADER_NAME_MAX 31
#define LOADER_MAX_FILE (512u * 1024u)

typedef struct {
    void *ctx;
    void (*send)(void *ctx, const char *line);                        /* one response line, '\n' included */
    bool (*busy)(void *ctx);
    uint32_t (*free_bytes)(void *ctx);
    bool (*open)(void *ctx, const char *name, uint32_t size);         /* start a temporary file for `name` */
    bool (*write)(void *ctx, const uint8_t *data, size_t n);
    bool (*commit)(void *ctx, const char *name);                      /* temporary -> `name`, replacing it */
    void (*abort)(void *ctx);                                         /* drop the temporary file */
    bool (*remove)(void *ctx, const char *name);
    bool (*list)(void *ctx, int index, char *name, size_t cap, uint32_t *size);   /* false after the last */
    bool (*reload)(void *ctx, char *reason, size_t cap);
} loader_io_t;

typedef struct {
    loader_io_t io;
    bool receiving;
    char line[160];
    size_t linelen;
    char name[LOADER_NAME_MAX + 1];
    uint32_t size, received, crc_expect, crc, since_ack;
    uint32_t last_ms;
} loader_t;

void loader_init(loader_t *l, const loader_io_t *io);
void loader_feed(loader_t *l, const uint8_t *data, size_t n, uint32_t now_ms);
void loader_poll(loader_t *l, uint32_t now_ms);     /* call regularly: aborts a stalled transfer */
bool loader_valid_name(const char *name);

#endif
