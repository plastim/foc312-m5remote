/* HDLC-style framing of the FOC-Stim link (byte-compatible with stimengine/device/hdlc.py and upstream restim):
 * frame = 0x7E | escaped(payload | CRC-16/X-25 little-endian) | 0x7E; escape 0x7D, XOR 0x20. */
#ifndef HDLC_H
#define HDLC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HDLC_MAX_PAYLOAD 1024

uint16_t hdlc_crc16(const uint8_t *p, size_t n);
/* encodes into out (worst case 2 * (n + 2) + 2 bytes); returns the frame length, 0 if it doesn't fit */
size_t hdlc_encode(const uint8_t *payload, size_t n, uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[HDLC_MAX_PAYLOAD];      /* payload + CRC; a frame over this is dropped (as hdlc.py max_len) */
    size_t len;
    bool escape_next, consuming;
} hdlc_decoder_t;

void hdlc_decoder_init(hdlc_decoder_t *d);
/* feed one byte; returns the payload length when a CRC-valid frame completes (payload in d->buf), else -1 */
int hdlc_decode_byte(hdlc_decoder_t *d, uint8_t c);

#endif
