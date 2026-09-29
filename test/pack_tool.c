/* Host tool for pattern packs, used by tests/test_remote_pack.py:
 *   pack_tool list FILE              one line per entry: index kind group mode start nmod name; or "ERROR n text"
 *   pack_tool play FILE INDEX TICKS  plays the entry through the C engine (levels 0.7 / 0.45, MA swept over 3000
 *                                    ticks, seed 7) and prints "digest HEX" over the per-tick lines (the
 *                                    golden_runner format) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pack.h"

static uint8_t buf[1 << 20];

static unsigned long long dbits(double d) {
    unsigned long long u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int chan(char *o, size_t cap, const et312_channel_t *c) {
    return snprintf(o, cap, " %d %016llx %016llx %016llx %d %d %d %u %u %u %u %u", c->gate_on ? 1 : 0,
                    dbits(c->intensity), dbits(c->pulse_rate_hz), dbits(c->pulse_width_us), c->biphasic ? 1 : 0,
                    c->leading_polarity, c->alternating ? 1 : 0, c->raw_gate, c->raw_ramp, c->raw_intensity,
                    c->raw_frequency, c->raw_width);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "rb");
    if (!f) { printf("ERROR -1 cannot open\n"); return 1; }
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    pack_t p;
    int err = pack_open(&p, buf, len);
    if (err != PACK_OK) { printf("ERROR %d %s\n", err, pack_error_text(err)); return 0; }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; i < p.count; i++) {
            pack_entry_t e;
            err = pack_entry(&p, i, &e);
            if (err != PACK_OK) { printf("ERROR %d %s\n", err, pack_error_text(err)); return 0; }
            printf("%d %d %d %d %d %d %s\n", i, e.kind, e.group, e.mode, e.start, e.nmod, e.name);
        }
        return 0;
    }
    if (!strcmp(argv[1], "play") && argc >= 5) {
        static et312_engine_t eng;
        et312_engine_init(&eng, 7);
        pack_install_builtins(&p, &eng);
        pack_entry_t e;
        if (pack_entry(&p, atoi(argv[3]), &e) != PACK_OK) { printf("ERROR bad index\n"); return 0; }
        et312_set_levels(&eng, 0.7, 0.45);
        err = pack_play(&p, &e, &eng);
        long ticks = atol(argv[4]);
        uint32_t digest = 2166136261u;
        char line[512];
        et312_frame_t fr;
        for (long t = 0; t < ticks; t++) {
            et312_set_ma(&eng, (double)(t % 3000) / 3000.0);
            et312_step(&eng, &fr);
            int n = snprintf(line, sizeof line, "%u %08x %d", fr.tick, fnv(2166136261u, eng.vm.mem, ET312_MEM), fr.mode);
            n += chan(line + n, sizeof line - (size_t)n, &fr.a);
            n += snprintf(line + n, sizeof line - (size_t)n, " |");
            n += chan(line + n, sizeof line - (size_t)n, &fr.b);
            n += snprintf(line + n, sizeof line - (size_t)n, " %d %u %u %d\n", (int)fr.phase_mode, fr.ma_value,
                          fr.ctrl_flags, eng.vm.error);
            digest = fnv(digest, (const uint8_t *)line, (size_t)n);
        }
        printf("digest %08x play %d\n", digest, err);
        return 0;
    }
    return 2;
}
