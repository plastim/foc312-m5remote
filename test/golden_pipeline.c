/* Script runner for the full remote pipeline: ET-312 engine -> foc312 fork view -> safety stack -> values sent to
 * the box. The Python reference (tests/test_remote_core.py) drives stimengine's Foc312Runner + Engine the same way.
 *
 * Commands (doubles as the 16 hex digits of their IEEE-754 bits):
 *   seed U64 | block IDX HEX | mode N | routine START
 *   safety SLOW DEADMAN RAMP CAP   (all bits)
 *   output_on | levels A B | ma V | master V | arm | disarm | hb | route CH CODE | pads 1101 | shape N | mono V
 *   loop N DT     N steps of DT seconds: advance foc312, tick the safety stack, print the values
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc312.h"

static foc312_t foc;
static safety_t saf;
static double now_s;
static uint8_t block_store[256][512];

static double bits(const char *hex) {
    unsigned long long u = strtoull(hex, NULL, 16);
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
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
    et312_set_block(&foc.et, idx, block_store[idx], (uint16_t)len);
}

int main(void) {
    static char line[4096];
    foc312_init(&foc, 0);
    safety_init(&saf, 0.0, 4.0, 2.0, 3.0, 0.15);
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = {0}, a[4][64];
        memset(a, 0, sizeof a);
        if (sscanf(line, "%31s %63s %63s %63s %63s", cmd, a[0], a[1], a[2], a[3]) < 1) continue;
        if (!strcmp(cmd, "seed")) {
            foc312_t fresh;
            foc312_init(&fresh, strtoull(a[0], NULL, 10));
            memcpy(fresh.et.vm.blocks, foc.et.vm.blocks, sizeof fresh.et.vm.blocks);
            fresh.et.builtins_available = foc.et.builtins_available;
            foc = fresh;
        } else if (!strcmp(cmd, "block")) {
            load_block(line);
        } else if (!strcmp(cmd, "mode")) {
            et312_set_mode(&foc.et, (int)strtol(a[0], NULL, 0));
            et312_frame(&foc.et, &foc.frame);
        } else if (!strcmp(cmd, "routine")) {
            et312_play_routine(&foc.et, (int)strtol(a[0], NULL, 0));
            et312_frame(&foc.et, &foc.frame);
        } else if (!strcmp(cmd, "safety")) {
            safety_init(&saf, now_s, bits(a[0]), bits(a[1]), bits(a[2]), bits(a[3]));
        } else if (!strcmp(cmd, "output_on")) {
            foc.output_on = true;
        } else if (!strcmp(cmd, "levels")) {
            foc312_set_levels(&foc, bits(a[0]), bits(a[1]));
        } else if (!strcmp(cmd, "ma")) {
            foc312_set_ma(&foc, bits(a[0]));
        } else if (!strcmp(cmd, "master")) {
            safety_set_master(&saf, bits(a[0]), true, now_s);
        } else if (!strcmp(cmd, "arm")) {
            safety_arm(&saf);
        } else if (!strcmp(cmd, "disarm")) {
            safety_disarm(&saf);
        } else if (!strcmp(cmd, "hb")) {
            safety_renew_lease(&saf, now_s);
        } else if (!strcmp(cmd, "route")) {
            foc312_set_route(&foc, atoi(a[0]), atoi(a[1]), &saf, now_s);
        } else if (!strcmp(cmd, "pads")) {
            bool p[4];
            for (int i = 0; i < 4; i++) p[i] = a[0][i] == '1';
            foc312_set_pads(&foc, p, &saf, now_s);
        } else if (!strcmp(cmd, "shape")) {
            foc312_set_shape(&foc, atoi(a[0]), &saf, now_s);
        } else if (!strcmp(cmd, "mono")) {
            foc.monophasic_asymmetry = bits(a[0]);
        } else if (!strcmp(cmd, "loop")) {
            long n = strtol(a[0], NULL, 10);
            double dt = bits(a[1]);
            safety_values_t v;
            for (long i = 0; i < n; i++) {
                now_s += dt;
                foc312_advance(&foc, now_s, &saf);
                safety_tick(&saf, now_s);
                safety_values(&saf, now_s, &v);
                printf("%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d %d %d %d %.17g %.17g %.17g %.17g %.17g\n",
                       v.amps[0], v.amps[1], v.rate_hz[0], v.rate_hz[1], v.width_us[0], v.width_us[1],
                       v.asymmetry[0], v.asymmetry[1], v.route[0], v.route[1], v.shape[0], v.shape[1],
                       safety_volume(&saf), saf.master_now, saf.deadman_scale, foc.pad_gain[0], foc.pad_gain[1]);
            }
        }
    }
    return 0;
}
