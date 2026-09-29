/* Script runner for the C core: reads commands on stdin, prints frame lines. The Python reference
 * (tests/test_remote_core.py) runs the same script through stimengine.et312 and the lines must match exactly.
 *
 * Commands (doubles are the 16 hex digits of their IEEE-754 bits, so both sides see identical values):
 *   seed U64 | power N | level_model linear|deadzone | random1 uniform|firmware | split A B
 *   block IDX HEX        program block bytecode (built-in blocks 0..35, routine modules >= 0x80)
 *   clear_user_blocks | mode N | routine START | silent | start_ramp
 *   levels BITS BITS | ma BITS
 *   run N PERIOD         N ticks; PERIOD > 0 sweeps the MA knob as (t % PERIOD) / PERIOD before each tick
 *   frame                the current frame without ticking
 *   every K              print only every K-th frame line (default 1); every line still goes into the digest
 *   digest               print "digest HEX": FNV-1a over every frame line so far (text, with newlines)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "et312.h"

static et312_engine_t eng;
static uint8_t block_store[256][512];
static char linebuf[512];
static int linelen;
static uint32_t digest = 2166136261u;
static long every = 1, counter;

static double bits_to_double(const char *hex) {
    unsigned long long u = strtoull(hex, NULL, 16);
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

static unsigned long long dbits(double d) {
    unsigned long long u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void append_channel(const et312_channel_t *c) {
    linelen += snprintf(linebuf + linelen, sizeof linebuf - (size_t)linelen,
                        " %d %016llx %016llx %016llx %d %d %d %u %u %u %u %u", c->gate_on ? 1 : 0,
                        dbits(c->intensity), dbits(c->pulse_rate_hz), dbits(c->pulse_width_us), c->biphasic ? 1 : 0,
                        c->leading_polarity, c->alternating ? 1 : 0, c->raw_gate, c->raw_ramp, c->raw_intensity,
                        c->raw_frequency, c->raw_width);
}

static void emit_frame(const et312_frame_t *f) {
    linelen = snprintf(linebuf, sizeof linebuf, "%u %08x %d", f->tick, fnv1a(eng.vm.mem, ET312_MEM), f->mode);
    append_channel(&f->a);
    linelen += snprintf(linebuf + linelen, sizeof linebuf - (size_t)linelen, " |");
    append_channel(&f->b);
    linelen += snprintf(linebuf + linelen, sizeof linebuf - (size_t)linelen, " %d %u %u %d\n", (int)f->phase_mode,
                        f->ma_value, f->ctrl_flags, eng.vm.error);
    for (int i = 0; i < linelen; i++) { digest ^= (uint8_t)linebuf[i]; digest *= 16777619u; }
    if (++counter % every == 0) fputs(linebuf, stdout);
}

static void load_block(const char *line) {
    char word[16];
    int idx, pos = 0;
    if (sscanf(line, "%15s %i %n", word, &idx, &pos) < 2 || idx < 0 || idx > 255) return;
    const char *hex = line + pos;
    int len = 0;
    while (hex[0] && hex[1] && hex[0] != '\n' && hex[0] != '\r' && len < (int)sizeof block_store[0]) {
        char byte[3] = {hex[0], hex[1], 0};
        block_store[idx][len++] = (uint8_t)strtol(byte, NULL, 16);
        hex += 2;
    }
    et312_set_block(&eng, idx, block_store[idx], (uint16_t)len);
}

int main(void) {
    static char line[4096];
    et312_engine_init(&eng, 0);
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = {0}, a1[64] = {0}, a2[64] = {0};
        if (sscanf(line, "%31s %63s %63s", cmd, a1, a2) < 1) continue;
        if (!strcmp(cmd, "seed")) {
            et312_engine_t fresh;
            et312_engine_init(&fresh, strtoull(a1, NULL, 10));
            memcpy(fresh.vm.blocks, eng.vm.blocks, sizeof fresh.vm.blocks);   /* registered blocks survive */
            fresh.builtins_available = eng.builtins_available;
            eng = fresh;
        } else if (!strcmp(cmd, "power")) {
            eng.power = atoi(a1);
            eng.vm.mem[0x1F4] = (uint8_t)eng.power;
        } else if (!strcmp(cmd, "level_model")) {
            eng.level_linear = !strcmp(a1, "linear");
        } else if (!strcmp(cmd, "skip_ramp")) {
            eng.skip_mode_ramp = atoi(a1) != 0;
        } else if (!strcmp(cmd, "random1")) {
            eng.random1_uniform = !strcmp(a1, "uniform");
        } else if (!strcmp(cmd, "split")) {
            eng.split_a = (int)strtol(a1, NULL, 0);
            eng.split_b = (int)strtol(a2, NULL, 0);
        } else if (!strcmp(cmd, "block")) {
            load_block(line);
        } else if (!strcmp(cmd, "clear_user_blocks")) {
            et312_clear_user_blocks(&eng);
        } else if (!strcmp(cmd, "mode")) {
            et312_set_mode(&eng, (int)strtol(a1, NULL, 0));
        } else if (!strcmp(cmd, "routine")) {
            et312_play_routine(&eng, (int)strtol(a1, NULL, 0));
        } else if (!strcmp(cmd, "silent")) {
            et312_silent(&eng);
        } else if (!strcmp(cmd, "start_ramp")) {
            et312_start_ramp(&eng);
        } else if (!strcmp(cmd, "levels")) {
            et312_set_levels(&eng, bits_to_double(a1), bits_to_double(a2));
        } else if (!strcmp(cmd, "ma")) {
            et312_set_ma(&eng, bits_to_double(a1));
        } else if (!strcmp(cmd, "run")) {
            long ticks = strtol(a1, NULL, 10), period = strtol(a2, NULL, 10);
            et312_frame_t f;
            for (long t = 0; t < ticks; t++) {
                if (period > 0) et312_set_ma(&eng, (double)(t % period) / (double)period);
                et312_step(&eng, &f);
                emit_frame(&f);
            }
        } else if (!strcmp(cmd, "frame")) {
            et312_frame_t f;
            et312_frame(&eng, &f);
            emit_frame(&f);
        } else if (!strcmp(cmd, "every")) {
            every = strtol(a1, NULL, 10);
            if (every < 1) every = 1;
        } else if (!strcmp(cmd, "digest")) {
            printf("digest %08x\n", digest);
        }
    }
    return 0;
}
