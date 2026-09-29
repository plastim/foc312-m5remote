/* The remote's link to a FOC-Stim running the fork firmware: HDLC + protobuf over any byte stream (the M5 uses
 * TCP to the box's Wi-Fi server, :55533). Transport-agnostic: feed received bytes, bytes go out through a callback.
 * Mirrors stimengine's client + Engine.start/_transmit:
 *   connect -> firmware version (branch "main", 1.x >= 1.1, fork comment vN >= 2) -> capabilities (the configured
 *   amps cap must not exceed the box's maximum) -> signal stop (the box may still be playing from an earlier
 *   connection) -> every axis sent once with zero amps -> signal start
 *   (OUTPUT_BIPHASIC_PAIRS) -> RUNNING: changed axes every tick, all of them every second (the box's 4 s keepalive),
 *   at most BOXLINK_MAX_PENDING unacknowledged requests.
 * Any request unanswered within its timeout, an error response, a boot notification (the box rebooted: a trip and
 * a power cycle) or a lost connection -> FAULT, with the reason; the app stops, and reconnects only when told to.
 * The box's trip report ("biphasic trip:" debug lines) is kept for the screen. No allocation. */
#ifndef BOXLINK_H
#define BOXLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdlc.h"
#include "safety.h"

#define BOXLINK_TIMEOUT_SETUP_MS 2000
#define BOXLINK_TIMEOUT_UPDATE_MS 4000
#define BOXLINK_REFRESH_MS 1000
#define BOXLINK_SILENCE_MS 1500   /* RUNNING and nothing at all from the box this long -> FAULT. The box sends
                                    system stats every 0.5 s and its knob every 1 s, so this never fires on a live
                                    box; a box that loses power can't close the connection, so this is how we know */
#define BOXLINK_MOVE_INTERVAL_MS 17    /* one 60 Hz tick (stimengine MOVE_INTERVAL_MS) */
#define BOXLINK_MAX_PENDING 20
#define BOXLINK_PENDING_SLOTS 64
#define BOXLINK_TRIP_LINES 4
#define BOXLINK_N_AXES 15          /* the fork's axes 60..74 */

typedef enum { BOXLINK_IDLE = 0, BOXLINK_HANDSHAKE_FW, BOXLINK_HANDSHAKE_CAPS, BOXLINK_STARTING, BOXLINK_RUNNING,
               BOXLINK_FAULT } boxlink_state_t;

typedef struct {
    float rms[4], peak[4], peak_cmd, output_power, output_power_skin;
    float out_r[4], out_x[4], skin_r[4], skin_x[4];   /* per electrode: output (loop) and skin estimates */
    float device_volume;                               /* the box's hardware knob, 0..1 */
    bool volume_locked;
    float battery_v, battery_soc, battery_charge_w;
    bool wall_power;
    float pulse_rate, v_drive, transformer_util, voltage_util;
    float sigma[2], guard, qnet[2];                    /* fork teleplots: bp_sigma_a/b, bp_guard, bp_qnet_a/b_uC */
    uint32_t currents_ms;                              /* when the last currents notification arrived */
    /* fork v7 guard diagnostics (0 before v7): climb holds (cumulative), per channel the sensed / commanded lead peak
     * and lead charge (smoothed) and the route's loop-resistance estimate */
    float hold, pk[2], rho[2], r_est[2];
} boxlink_telemetry_t;

typedef struct {
    void *ctx;
    bool (*write)(void *ctx, const uint8_t *data, size_t n);
} boxlink_io_t;

typedef struct {
    boxlink_io_t io;
    boxlink_state_t state;
    char fault[120];
    char fw_version[48], fw_comment[64];
    int fork_version;
    bool want_play, playing;          /* the signal: asked for / confirmed by the box (RUNNING = ready, not playing) */
    float max_amps, amps_cap;
    uint32_t next_id;
    struct { uint32_t id, deadline_ms, sent_ms; uint8_t kind; bool used; } pending[BOXLINK_PENDING_SLOTS];
    int npending;
    hdlc_decoder_t dec;
    double sent[BOXLINK_N_AXES];
    bool sent_valid[BOXLINK_N_AXES];
    uint32_t last_refresh_ms;
    uint32_t last_rx_ms, rx_frames;   /* heartbeat: every valid frame from the box (telemetry arrives ~10x/s) */
    float rtt_avg_ms;                 /* axis-update round trip (sent -> acknowledged), smoothed */
    uint32_t rtt_max_ms;              /* the worst since the app last cleared it (it shows it per second) */
    char trip[BOXLINK_TRIP_LINES][200];
    int ntrip;
    uint32_t trip_seq;                /* +1 for every new trip report; the report survives reconnects (a tripped box
                                         stops answering, so the reconnect attempts used to wipe it) */
    boxlink_telemetry_t tele;
} boxlink_t;

void boxlink_init(boxlink_t *l, const boxlink_io_t *io, float amps_cap);
/* the transport just connected: start the handshake */
void boxlink_connected(boxlink_t *l, uint32_t now_ms);
/* the transport closed or failed */
void boxlink_disconnected(boxlink_t *l, const char *reason);
void boxlink_feed(boxlink_t *l, const uint8_t *data, size_t n, uint32_t now_ms);
/* timeouts (call every loop) */
void boxlink_poll(boxlink_t *l, uint32_t now_ms);
/* RUNNING only: start or stop the box's signal (only a change is sent). Connecting leaves the box stopped, so its
 * own screen says Idle until the remote starts; a stop zeroes both channels first. */
void boxlink_set_playing(boxlink_t *l, bool on, uint32_t now_ms);
/* RUNNING only: send the axes that changed (or all of them once a second); returns false if skipped */
bool boxlink_send_values(boxlink_t *l, const safety_values_t *v, uint32_t now_ms);
/* best effort before closing: zero both channels and stop the signal */
void boxlink_stop(boxlink_t *l, uint32_t now_ms);
const char *boxlink_state_text(boxlink_state_t s);

#endif
