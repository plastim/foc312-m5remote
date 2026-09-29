/* USB loader, device side (see loader.h). */
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pack.h"

bool loader_valid_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > LOADER_NAME_MAX || name[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static void send_num(loader_t *l, const char *word, unsigned long v) {        /* "<word> <v>\n" */
    char buf[64];
    snprintf(buf, sizeof buf, "%s %lu\n", word, v);
    l->io.send(l->io.ctx, buf);
}

static void send_text(loader_t *l, const char *word, const char *text) {     /* "<word> <text>\n" */
    char buf[128];
    snprintf(buf, sizeof buf, "%s %s\n", word, text);
    l->io.send(l->io.ctx, buf);
}

void loader_init(loader_t *l, const loader_io_t *io) {
    memset(l, 0, sizeof *l);
    l->io = *io;
}

static void end_transfer(loader_t *l, bool ok) {
    l->receiving = false;
    if (!ok) l->io.abort(l->io.ctx);
}

static void command(loader_t *l, char *line, uint32_t now_ms) {
    char cmd[16] = {0}, a1[64] = {0}, a2[32] = {0}, a3[32] = {0};
    int n = sscanf(line, "%15s %63s %31s %31s", cmd, a1, a2, a3);
    if (n < 1) return;
    if (l->io.busy(l->io.ctx)) {
        l->io.send(l->io.ctx, "ERR busy\n");
        return;
    }
    if (!strcmp(cmd, "HELLO")) {
        send_num(l, "OK stim-remote 1", (unsigned long)l->io.free_bytes(l->io.ctx));
    } else if (!strcmp(cmd, "PUT")) {
        if (n < 4 || !loader_valid_name(a1)) { l->io.send(l->io.ctx, "ERR name\n"); return; }
        char *e1, *e2;
        unsigned long size = strtoul(a2, &e1, 10), crc = strtoul(a3, &e2, 16);
        if (*e1 || *e2 || size == 0 || size > LOADER_MAX_FILE) { l->io.send(l->io.ctx, "ERR size\n"); return; }
        if (size > l->io.free_bytes(l->io.ctx)) { l->io.send(l->io.ctx, "ERR space\n"); return; }
        if (!l->io.open(l->io.ctx, a1, (uint32_t)size)) { l->io.send(l->io.ctx, "ERR open\n"); return; }
        strcpy(l->name, a1);
        l->size = (uint32_t)size;
        l->crc_expect = (uint32_t)crc;
        l->crc = 0;
        l->received = l->since_ack = 0;
        l->receiving = true;
        l->last_ms = now_ms;
        send_num(l, "READY", (unsigned long)LOADER_CHUNK);
    } else if (!strcmp(cmd, "LIST")) {
        char name[LOADER_NAME_MAX + 1];
        uint32_t size;
        for (int i = 0; l->io.list(l->io.ctx, i, name, sizeof name, &size); i++) {
            char buf[80];
            snprintf(buf, sizeof buf, "FILE %s %lu\n", name, (unsigned long)size);
            l->io.send(l->io.ctx, buf);
        }
        l->io.send(l->io.ctx, "END\n");
    } else if (!strcmp(cmd, "DEL")) {
        if (n < 2 || !loader_valid_name(a1)) { l->io.send(l->io.ctx, "ERR name\n"); return; }
        l->io.send(l->io.ctx, l->io.remove(l->io.ctx, a1) ? "OK\n" : "ERR missing\n");
    } else if (!strcmp(cmd, "RELOAD")) {
        char reason[64] = {0};
        if (l->io.reload(l->io.ctx, reason, sizeof reason)) {
            l->io.send(l->io.ctx, "OK\n");
        } else {
            send_text(l, "ERR", reason[0] ? reason : "reload failed");
        }
    } else {
        l->io.send(l->io.ctx, "ERR unknown\n");
    }
}

void loader_feed(loader_t *l, const uint8_t *data, size_t n, uint32_t now_ms) {
    size_t i = 0;
    while (i < n) {
        if (l->receiving) {
            uint32_t want = l->size - l->received;
            uint32_t room = LOADER_CHUNK - l->since_ack;
            size_t take = n - i;
            if (take > want) take = want;
            if (take > room) take = room;
            if (!l->io.write(l->io.ctx, data + i, take)) {
                end_transfer(l, false);
                l->io.send(l->io.ctx, "ERR write\n");
                return;                           /* the rest of this buffer is file data we can't use */
            }
            l->crc = pack_crc32(l->crc, data + i, take);
            l->received += (uint32_t)take;
            l->since_ack += (uint32_t)take;
            l->last_ms = now_ms;
            i += take;
            if (l->since_ack == LOADER_CHUNK || l->received == l->size) {
                l->since_ack = 0;
                send_num(l, "ACK", (unsigned long)l->received);
            }
            if (l->received == l->size) {
                bool ok = l->crc == l->crc_expect;
                if (ok && l->io.commit(l->io.ctx, l->name)) {
                    l->receiving = false;
                    l->io.send(l->io.ctx, "OK\n");
                } else {
                    end_transfer(l, false);
                    l->io.send(l->io.ctx, ok ? "ERR commit\n" : "ERR crc\n");
                }
            }
            continue;
        }
        char c = (char)data[i++];
        if (c == '\r') continue;
        if (c == '\n') {
            l->line[l->linelen] = 0;
            l->linelen = 0;
            command(l, l->line, now_ms);
        } else if (l->linelen < sizeof l->line - 1) {
            l->line[l->linelen++] = c;
        }
    }
}

void loader_poll(loader_t *l, uint32_t now_ms) {
    if (l->receiving && now_ms - l->last_ms > LOADER_TIMEOUT_MS) {
        end_transfer(l, false);
        l->io.send(l->io.ctx, "ERR timeout\n");
    }
}
