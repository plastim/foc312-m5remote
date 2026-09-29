/* The remote's box link (see boxlink.h). Mirrors stimengine/device/client.py and Engine.start/_transmit. */
#include "boxlink.h"

#include <stdio.h>
#include <string.h>

#include "focstim_rpc.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

enum { K_FW = 1, K_CAPS, K_START, K_STOP, K_MOVE };

#define AXIS_BASE 60
#define OUTPUT_BIPHASIC_PAIRS 5
static const char FORK_COMMENT[] = "stim-engine biphasic-pairs";

/* the order Engine._biphasic_values yields them (and _transmit sends them) */
static const int CHANNEL_AXES[2][7] = {{73, 60, 62, 64, 69, 67, 71}, {74, 61, 63, 65, 70, 68, 72}};

static bool immediate_axis(int axis) {       /* routes, polarity, shape: never glide (device/fork.IMMEDIATE_AXES) */
    return axis == 71 || axis == 72 || axis == 67 || axis == 68 || axis == 73 || axis == 74;
}

static void fault(boxlink_t *l, const char *why) {
    if (l->state == BOXLINK_FAULT) return;
    l->state = BOXLINK_FAULT;
    snprintf(l->fault, sizeof l->fault, "%s", why);
}

void boxlink_init(boxlink_t *l, const boxlink_io_t *io, float amps_cap) {
    memset(l, 0, sizeof *l);
    l->io = *io;
    l->amps_cap = amps_cap;
    l->next_id = 1;
    hdlc_decoder_init(&l->dec);
}

static bool send_request(boxlink_t *l, focstim_rpc_Request *req, uint8_t kind, uint32_t timeout_ms, uint32_t now) {
    focstim_rpc_RpcMessage msg = focstim_rpc_RpcMessage_init_zero;
    req->id = l->next_id++;
    msg.which_message = focstim_rpc_RpcMessage_request_tag;
    msg.message.request = *req;
    uint8_t buf[160], frame[2 * 160 + 8];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    if (!pb_encode(&os, focstim_rpc_RpcMessage_fields, &msg)) { fault(l, "encode failed"); return false; }
    size_t n = hdlc_encode(buf, os.bytes_written, frame, sizeof frame);
    if (!n || !l->io.write(l->io.ctx, frame, n)) { fault(l, "link write failed"); return false; }
    for (int i = 0; i < BOXLINK_PENDING_SLOTS; i++) {
        if (!l->pending[i].used) {
            l->pending[i].used = true;
            l->pending[i].id = req->id;
            l->pending[i].deadline_ms = now + timeout_ms;
            l->pending[i].sent_ms = now;
            l->pending[i].kind = kind;
            l->npending++;
            return true;
        }
    }
    fault(l, "too many requests in flight");
    return false;
}

static bool send_move(boxlink_t *l, int axis, double value, uint32_t interval_ms, uint32_t now) {
    focstim_rpc_Request req = focstim_rpc_Request_init_zero;
    req.which_params = focstim_rpc_Request_request_axis_move_to_tag;
    req.params.request_axis_move_to.axis = (focstim_rpc_AxisType)axis;
    req.params.request_axis_move_to.value = (float)value;
    req.params.request_axis_move_to.interval = interval_ms;
    if (!send_request(l, &req, K_MOVE, BOXLINK_TIMEOUT_UPDATE_MS, now)) return false;
    l->sent[axis - AXIS_BASE] = value;
    l->sent_valid[axis - AXIS_BASE] = true;
    return true;
}

static double axis_value(const safety_values_t *v, int axis) {
    int ch = (axis == 61 || axis == 63 || axis == 65 || axis == 68 || axis == 70 || axis == 72 || axis == 74) ? 1 : 0;
    switch (axis) {
    case 60: case 61: return v->amps[ch];
    case 62: case 63: return v->rate_hz[ch];
    case 64: case 65: return v->width_us[ch];
    case 66: return v->gap_us;
    case 67: case 68: return v->polarity[ch];
    case 69: case 70: return v->asymmetry[ch];
    case 71: case 72: return (double)v->route[ch];
    default: return shape_axis_value(v->shape[ch]);       /* 73, 74 (v7: a taper id goes as 4.0..5.0) */
    }
}

/* Engine._transmit: every axis that changed (all of them when forced), in the engine's order */
static bool transmit(boxlink_t *l, const safety_values_t *v, bool force, uint32_t interval_ms, uint32_t now) {
    double gap = axis_value(v, 66);
    if (force || !l->sent_valid[66 - AXIS_BASE] || l->sent[66 - AXIS_BASE] != gap)
        if (!send_move(l, 66, gap, force ? 0 : interval_ms, now)) return false;
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < 7; k++) {
            int axis = CHANNEL_AXES[ch][k];
            double val = axis_value(v, axis);
            int i = axis - AXIS_BASE;
            if (force || !l->sent_valid[i] || l->sent[i] != val) {
                uint32_t iv = (force || immediate_axis(axis)) ? 0 : interval_ms;
                if (!send_move(l, axis, val, iv, now)) return false;
            }
        }
    }
    return true;
}

void boxlink_connected(boxlink_t *l, uint32_t now_ms) {
    float cap = l->amps_cap;
    boxlink_io_t io = l->io;
    char trip[BOXLINK_TRIP_LINES][200];         /* the last trip report outlives the connection */
    int ntrip = l->ntrip;
    uint32_t trip_seq = l->trip_seq;
    memcpy(trip, l->trip, sizeof trip);
    boxlink_init(l, &io, cap);
    memcpy(l->trip, trip, sizeof trip);
    l->ntrip = ntrip;
    l->trip_seq = trip_seq;
    l->state = BOXLINK_HANDSHAKE_FW;
    focstim_rpc_Request req = focstim_rpc_Request_init_zero;
    req.which_params = focstim_rpc_Request_request_firmware_version_tag;
    send_request(l, &req, K_FW, BOXLINK_TIMEOUT_SETUP_MS, now_ms);
}

void boxlink_disconnected(boxlink_t *l, const char *reason) {
    char why[120];
    snprintf(why, sizeof why, "link lost: %s", reason ? reason : "closed");
    fault(l, why);
}

static int fork_version_of(const char *comment) {
    size_t n = strlen(FORK_COMMENT);
    if (strncmp(comment, FORK_COMMENT, n) != 0) return 0;
    const char *s = comment + n;
    if (s[0] == ' ' && s[1] == 'v' && s[2] >= '0' && s[2] <= '9') {
        int v = 0;
        for (s += 2; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
        if (*s == 0) return v;
    }
    return 1;
}

static void on_response(boxlink_t *l, const focstim_rpc_Response *r, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < BOXLINK_PENDING_SLOTS; i++)
        if (l->pending[i].used && l->pending[i].id == r->id) { slot = i; break; }
    if (slot < 0) return;                                  /* not ours (or already timed out) */
    uint8_t kind = l->pending[slot].kind;
    if (kind == K_MOVE) {                                  /* round trip: how late the box acts on an update */
        uint32_t rtt = now - l->pending[slot].sent_ms;
        l->rtt_avg_ms = l->rtt_avg_ms <= 0 ? (float)rtt : l->rtt_avg_ms * 0.9f + (float)rtt * 0.1f;
        if (rtt > l->rtt_max_ms) l->rtt_max_ms = rtt;
    }
    l->pending[slot].used = false;
    l->npending--;
    if (r->has_error && kind == K_START && r->error.code == focstim_rpc_Errors_ERROR_ALREADY_PLAYING) {
        l->playing = true;                                 /* it already was: fine */
        return;
    }
    if (r->has_error) {
        char why[120];
        static const char *const names[] = {"?", "firmware version", "capabilities", "signal start", "signal stop",
                                            "axis update"};
        snprintf(why, sizeof why, "%s refused by the box (error %d)", names[kind <= K_MOVE ? kind : 0], (int)r->error.code);
        fault(l, why);
        return;
    }
    if (kind == K_FW && l->state == BOXLINK_HANDSHAKE_FW) {
        const focstim_rpc_ResponseFirmwareVersion *fw = &r->result.response_firmware_version;
        if (r->which_result != focstim_rpc_Response_response_firmware_version_tag || !fw->has_stm32_firmware_version_2) {
            fault(l, "no firmware version from the box");
            return;
        }
        const focstim_rpc_FirmwareVersion *v = &fw->stm32_firmware_version_2;
        snprintf(l->fw_version, sizeof l->fw_version, "%u.%u.%u (%s)", (unsigned)v->major, (unsigned)v->minor,
                 (unsigned)v->revision, v->branch);
        snprintf(l->fw_comment, sizeof l->fw_comment, "%s", v->comment);
        if (strcmp(v->branch, "main") != 0 || v->major != 1 || v->minor < 1) {
            fault(l, "incompatible firmware (needs 1.x on branch main)");
            return;
        }
        l->fork_version = fork_version_of(v->comment);
        if (l->fork_version < 2) {
            fault(l, l->fork_version ? "fork firmware v1: needs v2 or later" : "not the fork firmware (flash it first)");
            return;
        }
        l->state = BOXLINK_HANDSHAKE_CAPS;
        focstim_rpc_Request req = focstim_rpc_Request_init_zero;
        req.which_params = focstim_rpc_Request_request_capabilities_get_tag;
        send_request(l, &req, K_CAPS, BOXLINK_TIMEOUT_SETUP_MS, now);
    } else if (kind == K_CAPS && l->state == BOXLINK_HANDSHAKE_CAPS) {
        l->max_amps = r->result.response_capabilities_get.maximum_waveform_amplitude_amps;
        if (l->max_amps > 0 && l->amps_cap > l->max_amps + 1e-6f) {
            char why[120];
            snprintf(why, sizeof why, "amps cap %.3f exceeds the box maximum %.3f", (double)l->amps_cap,
                     (double)l->max_amps);
            fault(l, why);
            return;
        }
        /* stop first: after a remote reboot or a dropped link the box is still "playing" (at zero) and refuses a
         * start with ERROR_ALREADY_PLAYING (4). A stop is always accepted, so every connection starts from a known
         * state (PlaStim hit "error 4" reconnect loops, 2026-09-28) */
        l->state = BOXLINK_STARTING;
        focstim_rpc_Request stop = focstim_rpc_Request_init_zero;
        stop.which_params = focstim_rpc_Request_request_signal_stop_tag;
        send_request(l, &stop, K_STOP, BOXLINK_TIMEOUT_SETUP_MS, now);
    } else if (kind == K_STOP && l->state == BOXLINK_STARTING) {
        /* Engine.start: every value once (zero amps) before the signal starts */
        safety_values_t zero;
        memset(&zero, 0, sizeof zero);
        for (int ch = 0; ch < 2; ch++) {
            zero.rate_hz[ch] = 50.0;
            zero.width_us[ch] = 150.0;
            zero.asymmetry[ch] = 1.0;
            zero.route[ch] = ch == 0 ? 12 : 34;
        }
        if (!transmit(l, &zero, true, 0, now)) return;
        /* ready, but the signal stays stopped until the remote starts (boxlink_set_playing): the box's own screen
         * says Idle while the remote is stopped (PlaStim, 2026-09-28) */
        l->state = BOXLINK_RUNNING;
        l->playing = l->want_play = false;
        l->last_refresh_ms = now;
        l->last_rx_ms = now;
    } else if (kind == K_START && l->state == BOXLINK_RUNNING) {
        l->playing = true;
    } else if (kind == K_STOP && l->state == BOXLINK_RUNNING) {
        l->playing = false;
    }
}

static void keep_trip_line(boxlink_t *l, const char *text) {
    if (strstr(text, "limit exceeded")) {       /* the first line of a new report: it replaces the last one */
        l->ntrip = 0;
        l->trip_seq++;
    }
    if (l->ntrip == BOXLINK_TRIP_LINES) {
        memmove(l->trip[0], l->trip[1], sizeof l->trip[0] * (BOXLINK_TRIP_LINES - 1));
        l->ntrip--;
    }
    snprintf(l->trip[l->ntrip++], sizeof l->trip[0], "%s", text);
}

static void on_notification(boxlink_t *l, const focstim_rpc_Notification *n, uint32_t now) {
    boxlink_telemetry_t *t = &l->tele;
    switch (n->which_notification) {
    case focstim_rpc_Notification_notification_boot_tag:
        fault(l, "the box rebooted (a trip or a power cycle)");
        break;
    case focstim_rpc_Notification_notification_currents_tag: {
        const focstim_rpc_NotificationCurrents *c = &n->notification.notification_currents;
        t->rms[0] = c->rms_a; t->rms[1] = c->rms_b; t->rms[2] = c->rms_c; t->rms[3] = c->rms_d;
        t->peak[0] = c->peak_a; t->peak[1] = c->peak_b; t->peak[2] = c->peak_c; t->peak[3] = c->peak_d;
        t->peak_cmd = c->peak_cmd;
        t->output_power = c->output_power;
        t->output_power_skin = c->output_power_skin;
        t->currents_ms = now;
        break;
    }
    case focstim_rpc_Notification_notification_output_resistance_tag: {
        const focstim_rpc_NotificationOutputResistance *r = &n->notification.notification_output_resistance;
        t->out_r[0] = r->resistance_a; t->out_r[1] = r->resistance_b; t->out_r[2] = r->resistance_c; t->out_r[3] = r->resistance_d;
        t->out_x[0] = r->reluctance_a; t->out_x[1] = r->reluctance_b; t->out_x[2] = r->reluctance_c; t->out_x[3] = r->reluctance_d;
        break;
    }
    case focstim_rpc_Notification_notification_skin_resistance_tag: {
        const focstim_rpc_NotificationSkinResistance *r = &n->notification.notification_skin_resistance;
        t->skin_r[0] = r->resistance_a; t->skin_r[1] = r->resistance_b; t->skin_r[2] = r->resistance_c; t->skin_r[3] = r->resistance_d;
        t->skin_x[0] = r->reluctance_a; t->skin_x[1] = r->reluctance_b; t->skin_x[2] = r->reluctance_c; t->skin_x[3] = r->reluctance_d;
        break;
    }
    case focstim_rpc_Notification_notification_device_volume_tag:
        t->device_volume = n->notification.notification_device_volume.volume;
        t->volume_locked = n->notification.notification_device_volume.locked;
        break;
    case focstim_rpc_Notification_notification_battery_tag: {
        const focstim_rpc_NotificationBattery *b = &n->notification.notification_battery;
        t->battery_v = b->battery_voltage;
        t->battery_soc = b->battery_soc;
        t->battery_charge_w = b->battery_charge_rate_watt;
        t->wall_power = b->wall_power_present;
        break;
    }
    case focstim_rpc_Notification_notification_signal_stats_tag: {
        const focstim_rpc_NotificationSignalStats *s = &n->notification.notification_signal_stats;
        t->pulse_rate = s->actual_pulse_frequency;
        t->v_drive = s->v_drive;
        t->transformer_util = s->transformer_utilization;
        t->voltage_util = s->voltage_utilization;
        break;
    }
    case focstim_rpc_Notification_notification_debug_string_tag: {
        const char *m = n->notification.notification_debug_string.message;
        if (strstr(m, "biphasic") || strstr(m, "limit exceeded") || strstr(m, "e-stop") || strstr(m, "estop"))
            keep_trip_line(l, m);
        break;
    }
    case focstim_rpc_Notification_notification_debug_teleplot_tag: {
        const char *id = n->notification.notification_debug_teleplot.id;
        float v = n->notification.notification_debug_teleplot.value;
        if (!strcmp(id, "bp_sigma_a")) t->sigma[0] = v;
        else if (!strcmp(id, "bp_sigma_b")) t->sigma[1] = v;
        else if (!strcmp(id, "bp_guard")) t->guard = v;
        else if (!strcmp(id, "bp_qnet_a_uC")) t->qnet[0] = v;
        else if (!strcmp(id, "bp_qnet_b_uC")) t->qnet[1] = v;
        else if (!strcmp(id, "bp_hold")) t->hold = v;
        else if (!strcmp(id, "bp_pk_a")) t->pk[0] = v;
        else if (!strcmp(id, "bp_pk_b")) t->pk[1] = v;
        else if (!strcmp(id, "bp_rho_a")) t->rho[0] = v;
        else if (!strcmp(id, "bp_rho_b")) t->rho[1] = v;
        else if (!strcmp(id, "bp_r_a")) t->r_est[0] = v;
        else if (!strcmp(id, "bp_r_b")) t->r_est[1] = v;
        break;
    }
    default:
        break;
    }
}

void boxlink_feed(boxlink_t *l, const uint8_t *data, size_t n, uint32_t now_ms) {
    for (size_t i = 0; i < n; i++) {
        int len = hdlc_decode_byte(&l->dec, data[i]);
        if (len < 0) continue;
        focstim_rpc_RpcMessage msg = focstim_rpc_RpcMessage_init_zero;
        pb_istream_t is = pb_istream_from_buffer(l->dec.buf, (size_t)len);
        if (!pb_decode(&is, focstim_rpc_RpcMessage_fields, &msg)) continue;   /* the Python client skips these too */
        l->last_rx_ms = now_ms;
        l->rx_frames++;
        if (msg.which_message == focstim_rpc_RpcMessage_response_tag) on_response(l, &msg.message.response, now_ms);
        else if (msg.which_message == focstim_rpc_RpcMessage_notification_tag) on_notification(l, &msg.message.notification, now_ms);
    }
}

void boxlink_poll(boxlink_t *l, uint32_t now_ms) {
    if (l->state == BOXLINK_IDLE || l->state == BOXLINK_FAULT) return;
    if (l->state == BOXLINK_RUNNING && (int32_t)(now_ms - l->last_rx_ms) > BOXLINK_SILENCE_MS) {
        fault(l, "the box went silent (switched off, or out of Wi-Fi range)");
        return;
    }
    for (int i = 0; i < BOXLINK_PENDING_SLOTS; i++) {
        if (l->pending[i].used && (int32_t)(now_ms - l->pending[i].deadline_ms) > 0) {
            char why[120];
            static const char *const names[] = {"?", "firmware version", "capabilities", "signal start",
                                                "signal stop", "axis update"};
            uint8_t k = l->pending[i].kind;
            snprintf(why, sizeof why, "%s request timed out (the box stopped answering)", names[k <= K_MOVE ? k : 0]);
            fault(l, why);
            return;
        }
    }
}

void boxlink_set_playing(boxlink_t *l, bool on, uint32_t now_ms) {
    if (l->state != BOXLINK_RUNNING || on == l->want_play) return;
    l->want_play = on;
    focstim_rpc_Request req = focstim_rpc_Request_init_zero;
    if (on) {
        req.which_params = focstim_rpc_Request_request_signal_start_tag;
        req.params.request_signal_start.mode = (focstim_rpc_OutputMode)OUTPUT_BIPHASIC_PAIRS;
        send_request(l, &req, K_START, BOXLINK_TIMEOUT_SETUP_MS, now_ms);
    } else {
        if (!send_move(l, 60, 0.0, 0, now_ms) || !send_move(l, 61, 0.0, 0, now_ms)) return;
        req.which_params = focstim_rpc_Request_request_signal_stop_tag;
        send_request(l, &req, K_STOP, BOXLINK_TIMEOUT_SETUP_MS, now_ms);
    }
}

bool boxlink_send_values(boxlink_t *l, const safety_values_t *v, uint32_t now_ms) {
    if (l->state != BOXLINK_RUNNING) return false;
    if (l->npending > BOXLINK_MAX_PENDING) return false;              /* the box is behind: skip this tick */
    bool refresh = (uint32_t)(now_ms - l->last_refresh_ms) >= BOXLINK_REFRESH_MS;
    if (refresh) {
        for (int i = 0; i < BOXLINK_N_AXES; i++) l->sent_valid[i] = false;
        l->last_refresh_ms = now_ms;
    }
    return transmit(l, v, false, BOXLINK_MOVE_INTERVAL_MS, now_ms);
}

void boxlink_stop(boxlink_t *l, uint32_t now_ms) {
    if (l->state == BOXLINK_RUNNING || l->state == BOXLINK_STARTING) {
        send_move(l, 60, 0.0, 0, now_ms);
        send_move(l, 61, 0.0, 0, now_ms);
        focstim_rpc_Request req = focstim_rpc_Request_init_zero;
        req.which_params = focstim_rpc_Request_request_signal_stop_tag;
        send_request(l, &req, K_STOP, BOXLINK_TIMEOUT_SETUP_MS, now_ms);
    }
    l->state = BOXLINK_IDLE;
}

const char *boxlink_state_text(boxlink_state_t s) {
    static const char *const text[] = {"idle", "handshake", "handshake", "starting", "running", "fault"};
    return (int)s >= 0 && (int)s <= BOXLINK_FAULT ? text[s] : "?";
}
