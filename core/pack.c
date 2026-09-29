/* Pattern pack reader (see pack.h; format in stimengine/remote/pack.py). */
#include "pack.h"

#include <string.h>

uint32_t pack_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool valid_mode(int m) {
    return (m >= ET_WAVES && m <= ET_PHASE3) || m == ET_CLIMB_SLOW || m == ET_CLIMB_HOLD;
}

/* walk one entry from `pos` (bounded by `end`), filling `out` if given; returns the next position or 0 on error */
static size_t walk_entry(const pack_t *p, size_t pos, size_t end, pack_entry_t *out, int *err) {
    const uint8_t *d = p->data;
    if (pos + 3 > end) { *err = PACK_ERR_TRUNCATED; return 0; }
    int kind = d[pos], group = d[pos + 1], nlen = d[pos + 2];
    pos += 3;
    if (nlen == 0 || nlen > PACK_NAME_MAX) { *err = PACK_ERR_NAME; return 0; }
    if (pos + (size_t)nlen > end) { *err = PACK_ERR_TRUNCATED; return 0; }
    for (int i = 0; i < nlen; i++)
        if (d[pos + i] < 32 || d[pos + i] > 126) { *err = PACK_ERR_NAME; return 0; }
    if (out) {
        memset(out, 0, sizeof *out);
        out->kind = kind;
        out->group = group;
        memcpy(out->name, d + pos, (size_t)nlen);
        out->name[nlen] = 0;
    }
    pos += (size_t)nlen;
    if (kind == PACK_KIND_BUILTIN) {
        if (pos + 1 > end) { *err = PACK_ERR_TRUNCATED; return 0; }
        if (!p->has_builtin) { *err = PACK_ERR_NO_BUILTINS; return 0; }
        if (!valid_mode(d[pos])) { *err = PACK_ERR_MODE; return 0; }
        if (out) out->mode = d[pos];
        return pos + 1;
    }
    if (kind != PACK_KIND_ROUTINE) { *err = PACK_ERR_KIND; return 0; }
    if (pos + 2 > end) { *err = PACK_ERR_TRUNCATED; return 0; }
    int start = d[pos], nmod = d[pos + 1];
    pos += 2;
    if (nmod < 1 || nmod > 64) { *err = PACK_ERR_MODULE; return 0; }
    if (out) { out->start = start; out->nmod = nmod; out->modules = d + pos; }
    bool start_found = false;
    int last = -1;
    for (int i = 0; i < nmod; i++) {
        if (pos + 3 > end) { *err = PACK_ERR_TRUNCATED; return 0; }
        int num = d[pos];
        uint16_t n = rd16(d + pos + 1);
        pos += 3;
        if (num < 0x80 || num <= last) { *err = PACK_ERR_MODULE; return 0; }   /* user range, ascending, unique */
        last = num;
        if (pos + n > end) { *err = PACK_ERR_TRUNCATED; return 0; }
        if (!et312_bytecode_valid(d + pos, n)) { *err = PACK_ERR_BYTECODE; return 0; }
        start_found |= num == start;
        pos += n;
    }
    if (!start_found) { *err = PACK_ERR_START; return 0; }
    return pos;
}

int pack_open(pack_t *p, const uint8_t *data, size_t len) {
    memset(p, 0, sizeof *p);
    if (len < 16 || memcmp(data, "FP31", 4) != 0) return PACK_ERR_MAGIC;
    if (pack_crc32(0, data, len - 4) != rd32(data + len - 4)) return PACK_ERR_CRC;
    if (rd16(data + 4) != 1) return PACK_ERR_VERSION;
    p->data = data;
    p->len = len;
    p->has_builtin = (rd16(data + 6) & 1) != 0;
    p->count = rd16(data + 8);
    size_t end = len - 4, pos = 12;
    if (p->has_builtin) {
        for (int i = 0; i < PACK_BUILTIN_BLOCKS; i++) {
            if (pos + 2 > end) return PACK_ERR_TRUNCATED;
            uint16_t n = rd16(data + pos);
            pos += 2;
            if (pos + n > end) return PACK_ERR_TRUNCATED;
            if (!et312_bytecode_valid(data + pos, n)) return PACK_ERR_BYTECODE;
            p->builtin[i] = data + pos;
            p->builtin_len[i] = n;
            pos += n;
        }
    }
    p->entries_at = pos;
    for (int i = 0; i < p->count; i++) {
        int err = PACK_OK;
        pos = walk_entry(p, pos, end, NULL, &err);
        if (!pos) { memset(p, 0, sizeof *p); return err; }
    }
    if (pos != end) { memset(p, 0, sizeof *p); return PACK_ERR_TRAILING; }
    return PACK_OK;
}

int pack_entry(const pack_t *p, int index, pack_entry_t *out) {
    if (!p->data || index < 0 || index >= p->count) return PACK_ERR_INDEX;
    size_t pos = p->entries_at, end = p->len - 4;
    int err = PACK_OK;
    for (int i = 0; i < index; i++) {
        pos = walk_entry(p, pos, end, NULL, &err);
        if (!pos) return err;
    }
    return walk_entry(p, pos, end, out, &err) ? PACK_OK : err;
}

void pack_install_builtins(const pack_t *p, et312_engine_t *e) {
    if (!p->has_builtin) return;
    for (int i = 0; i < PACK_BUILTIN_BLOCKS; i++) et312_set_block(e, i, p->builtin[i], p->builtin_len[i]);
}

int pack_play(const pack_t *p, const pack_entry_t *entry, et312_engine_t *e) {
    (void)p;
    if (entry->kind == PACK_KIND_BUILTIN) return et312_set_mode(e, entry->mode);
    et312_clear_user_blocks(e);
    const uint8_t *m = entry->modules;
    for (int i = 0; i < entry->nmod; i++) {
        uint16_t n = rd16(m + 1);
        et312_set_block(e, m[0], m + 3, n);
        m += 3 + n;
    }
    return et312_play_routine(e, entry->start);
}

const char *pack_error_text(int err) {
    static const char *const text[] = {"ok", "not a pattern pack", "CRC mismatch (damaged file)",
                                       "unsupported pack version", "truncated", "trailing bytes",
                                       "unknown entry kind", "bad name", "bad module", "start module missing",
                                       "invalid bytecode", "unknown mode", "no such entry",
                                       "built-in mode without built-in blocks"};
    return err >= 0 && err < (int)(sizeof text / sizeof text[0]) ? text[err] : "?";
}
