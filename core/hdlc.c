/* HDLC framing (see hdlc.h); mirrors stimengine/device/hdlc.py. */
#include "hdlc.h"

#define BOUNDARY 0x7E
#define ESCAPE 0x7D

uint16_t hdlc_crc16(const uint8_t *p, size_t n) {
    /* CRC-16/X-25: reflected poly 0x1021 (0x8408), init 0xFFFF, final xor 0xFFFF */
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

static bool put(uint8_t *out, size_t cap, size_t *pos, uint8_t c) {
    if (c == BOUNDARY || c == ESCAPE) {
        if (*pos + 2 > cap) return false;
        out[(*pos)++] = ESCAPE;
        out[(*pos)++] = (uint8_t)(c ^ 0x20);
    } else {
        if (*pos + 1 > cap) return false;
        out[(*pos)++] = c;
    }
    return true;
}

size_t hdlc_encode(const uint8_t *payload, size_t n, uint8_t *out, size_t cap) {
    size_t pos = 0;
    if (cap < 1) return 0;
    out[pos++] = BOUNDARY;
    for (size_t i = 0; i < n; i++)
        if (!put(out, cap, &pos, payload[i])) return 0;
    uint16_t crc = hdlc_crc16(payload, n);
    if (!put(out, cap, &pos, (uint8_t)(crc & 0xFF)) || !put(out, cap, &pos, (uint8_t)(crc >> 8))) return 0;
    if (pos + 1 > cap) return 0;
    out[pos++] = BOUNDARY;
    return pos;
}

void hdlc_decoder_init(hdlc_decoder_t *d) {
    d->len = 0;
    d->escape_next = false;
    d->consuming = false;
}

int hdlc_decode_byte(hdlc_decoder_t *d, uint8_t c) {
    if (c == BOUNDARY) {
        int result = -1;
        if (d->len >= 2) {
            size_t n = d->len - 2;
            uint16_t crc = (uint16_t)(d->buf[n] | (d->buf[n + 1] << 8));
            if (hdlc_crc16(d->buf, n) == crc) result = (int)n;
        }
        d->len = 0;
        d->escape_next = false;
        d->consuming = true;
        return result;
    }
    if (c == ESCAPE) {
        d->escape_next = true;
        return -1;
    }
    if (d->escape_next) {
        c ^= 0x20;
        d->escape_next = false;
    }
    if (d->consuming) {
        if (d->len < sizeof d->buf) d->buf[d->len++] = c;
        else { d->len = 0; d->escape_next = false; d->consuming = false; }   /* oversize: drop until the next boundary */
    }
    return -1;
}
