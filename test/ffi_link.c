/* ctypes shim over boxlink for tests/test_remote_link.py (built as a shared library with the core). */
#include <string.h>

#include "boxlink.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef int (*write_cb_t)(const uint8_t *data, int n);

static boxlink_t L;
static write_cb_t W;

static bool wr(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    return W && W(d, (int)n);
}

EXPORT void bl_init(write_cb_t cb, float cap) {
    W = cb;
    boxlink_io_t io = {NULL, wr};
    boxlink_init(&L, &io, cap);
}
EXPORT void bl_connected(uint32_t now) { boxlink_connected(&L, now); }
EXPORT void bl_disconnected(const char *why) { boxlink_disconnected(&L, why); }
EXPORT void bl_feed(const uint8_t *d, int n, uint32_t now) { boxlink_feed(&L, d, (size_t)n, now); }
EXPORT void bl_poll(uint32_t now) { boxlink_poll(&L, now); }
EXPORT void bl_stop(uint32_t now) { boxlink_stop(&L, now); }
EXPORT void bl_set_playing(int on, uint32_t now) { boxlink_set_playing(&L, on != 0, now); }
EXPORT int bl_playing(void) { return L.playing; }
EXPORT int bl_send(const double *amps, const double *rate, const double *width, const double *asym, const int *route,
                   const int *shape, uint32_t now) {
    safety_values_t v;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < 2; i++) {
        v.amps[i] = amps[i];
        v.rate_hz[i] = rate[i];
        v.width_us[i] = width[i];
        v.asymmetry[i] = asym[i];
        v.route[i] = route[i];
        v.shape[i] = shape[i];
    }
    return boxlink_send_values(&L, &v, now);
}
EXPORT int bl_state(void) { return (int)L.state; }
EXPORT const char *bl_fault(void) { return L.fault; }
EXPORT const char *bl_fw_version(void) { return L.fw_version; }
EXPORT const char *bl_fw_comment(void) { return L.fw_comment; }
EXPORT int bl_fork_version(void) { return L.fork_version; }
EXPORT float bl_max_amps(void) { return L.max_amps; }
EXPORT int bl_npending(void) { return L.npending; }
EXPORT const boxlink_telemetry_t *bl_tele(void) { return &L.tele; }
EXPORT int bl_ntrip(void) { return L.ntrip; }
EXPORT uint32_t bl_trip_seq(void) { return L.trip_seq; }
EXPORT uint32_t bl_rx_frames(void) { return L.rx_frames; }
EXPORT float bl_rtt_avg(void) { return L.rtt_avg_ms; }
EXPORT uint32_t bl_rtt_max(void) { return L.rtt_max_ms; }
EXPORT uint32_t bl_last_rx_ms(void) { return L.last_rx_ms; }
EXPORT const char *bl_trip(int i) { return (i >= 0 && i < L.ntrip) ? L.trip[i] : ""; }
