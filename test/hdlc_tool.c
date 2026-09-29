/* HDLC check tool (tests/test_remote_link.py): "enc HEX" -> the frame as hex; "dec HEX" feeds the bytes to the decoder
 * and prints every completed payload as hex ("-" for an empty one). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdlc.h"

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (s[0] && s[1] && s[0] != '\n' && s[0] != '\r' && n < cap) {
        char b[3] = {s[0], s[1], 0};
        out[n++] = (uint8_t)strtol(b, NULL, 16);
        s += 2;
    }
    return n;
}

int main(void) {
    static char line[1 << 21];
    static uint8_t in[1 << 20], out[1 << 21];
    static hdlc_decoder_t d;
    hdlc_decoder_init(&d);
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "enc ", 4)) {
            size_t n = unhex(line + 4, in, sizeof in);
            size_t m = hdlc_encode(in, n, out, sizeof out);
            for (size_t i = 0; i < m; i++) printf("%02x", out[i]);
            printf("\n");
        } else if (!strncmp(line, "dec ", 4)) {
            size_t n = unhex(line + 4, in, sizeof in);
            int any = 0;
            for (size_t i = 0; i < n; i++) {
                int r = hdlc_decode_byte(&d, in[i]);
                if (r >= 0) {
                    if (any++) printf(" ");
                    if (r == 0) printf("-");
                    for (int k = 0; k < r; k++) printf("%02x", d.buf[k]);
                }
            }
            printf("\n");
        }
        fflush(stdout);
    }
    return 0;
}
