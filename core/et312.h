/* ET-312B mode engine in portable C99: the VM (program-block bytecode, modulators, gate, block timers), mode
 * selection (Function_0x604, Random1) and the per-channel outputs. A port of stimengine/et312/{vm,modes,engine}.py,
 * which stay the reference: tests/test_remote_core.py runs both on the same inputs and compares every tick
 * (VM memory and every output, bit for bit).
 *
 * Program blocks are the firmware's own bytecode, executed as stored (no decode step). The built-in modes' 36
 * blocks are ErosTek's and are NOT in this source: the host loads them from the user's own firmware data
 * (stimengine/et312/fwdata.py) and hands them to et312_set_block(). Blocks 0 and 1 (gates off / on) are defined
 * here so routines run without that data. No allocation; one et312_engine_t is ~2 KB plus the rng. */
#ifndef ET312_H
#define ET312_H

#include <stdbool.h>
#include <stdint.h>

#include "pyrand.h"

#define ET312_MEM 0x300
#define ET312_TICK_HZ (8000000.0 / 64.0 / 256.0 / 2.0)   /* 244.140625 Hz */
#define ET312_TICK_S (1.0 / ET312_TICK_HZ)

/* mode numbers ($407b) */
enum {
    ET_WAVES = 0x76, ET_STROKE, ET_CLIMB, ET_COMBO, ET_INTENSE, ET_RHYTHM,
    ET_AUDIO1, ET_AUDIO2, ET_AUDIO3, ET_SPLIT, ET_RANDOM1, ET_RANDOM2,
    ET_TOGGLE, ET_ORGASM, ET_TORMENT, ET_PHASE1, ET_PHASE2, ET_PHASE3,
    ET_USER1 = 0x88, ET_USER2 = 0x89, ET_USER3 = 0x90, ET_USER4, ET_USER5, ET_USER6, ET_USER7,
    /* PlaStim variants of Climb (engine.py): Climb's own program, the VM clock slowed or paused */
    ET_CLIMB_SLOW = 0xF0, ET_CLIMB_HOLD = 0xF1
};

enum { ET_OK = 0, ET_ERR_NO_BLOCK, ET_ERR_BAD_OPCODE, ET_ERR_RANDOM_RANGE, ET_ERR_NO_USER_START, ET_ERR_BAD_MODE };

typedef struct {
    const uint8_t *code;    /* bytecode, ends at the first byte < 0x20 or at len */
    uint16_t len;
} et312_block_t;

typedef struct {
    uint8_t mem[ET312_MEM];
    et312_block_t blocks[256];
    pyrand_t rng;
    double ma_knob;          /* 0 = fully CCW, 1 = fully CW */
    int ma_override_r2;      /* Random1 replaces the pot reading; -1 = none */
    uint32_t tick_count;
    bool pending_next;
    int error;               /* first ET_ERR_* seen (the firmware would halt with "Failure 15") */
} et312_vm_t;

typedef struct {
    bool gate_on;
    double intensity;        /* 0..1 */
    double pulse_rate_hz;
    double pulse_width_us;
    bool biphasic;
    int leading_polarity;    /* +1 / -1 */
    bool alternating;
    uint8_t raw_gate, raw_ramp, raw_intensity, raw_frequency, raw_width;
} et312_channel_t;

typedef enum { ET_PHASE_NONE = 0, ET_PHASE_INTERLEAVED, ET_PHASE_LINKED } et312_phase_mode_t;

typedef struct {
    uint32_t tick;
    int mode;
    et312_channel_t a, b;
    et312_phase_mode_t phase_mode;
    uint8_t ma_value;
    uint8_t ctrl_flags;
} et312_frame_t;

typedef struct {
    et312_vm_t vm;
    int mode;                /* 0 = silent (no pattern) */
    bool builtins_available;
    bool routine_loaded;     /* a user routine plays as User1 */
    int user_start[ET_USER7 - ET_USER1 + 1];   /* -1 = none */
    int split_a, split_b;
    int power;               /* 0 low, 1 normal, 2 high */
    bool level_linear;       /* level_model "linear" (default "deadzone") */
    bool random1_uniform;    /* random1_ma "uniform" (default "firmware") */
    bool skip_mode_ramp;     /* start every mode at full ramp (default: the box's ~3.2 s ramp) */
    double level_a, level_b;
    uint8_t master_msb;
    /* PlaStim Climb variants (ET312Engine._variant_advance): 0 none, 1 slow finish, 2 peak hold */
    int variant;
    uint32_t frozen;         /* engine ticks the VM did not advance (frame tick = VM ticks + this) */
    uint32_t v_start, v_hold;
    int v_prev, v_top, v_div;
    bool v_held;
} et312_engine_t;

/* ---- VM ---- */
void et312_vm_init(et312_vm_t *vm, uint64_t seed);
void et312_vm_reset_defaults(et312_vm_t *vm);
void et312_vm_load_block(et312_vm_t *vm, int idx);
void et312_vm_tick(et312_vm_t *vm);
void et312_vm_update_ma(et312_vm_t *vm);
int et312_vm_random_between(et312_vm_t *vm);

/* true if every op decodes (the firmware's module format; stops at the first byte < 0x20 or at len) */
bool et312_bytecode_valid(const uint8_t *code, uint16_t len);

/* ---- modes ---- */
void et312_select_mode(et312_vm_t *vm, int mode, int split_a, int split_b, const int *user_start);

/* ---- engine ---- */
void et312_engine_init(et312_engine_t *e, uint64_t seed);
/* program blocks: the built-in modes' 0..35 (the user's own data) and a routine's modules (>= 0x80) */
void et312_set_block(et312_engine_t *e, int idx, const uint8_t *code, uint16_t len);
void et312_clear_user_blocks(et312_engine_t *e);
bool et312_is_builtin(int mode);
int et312_set_mode(et312_engine_t *e, int mode);          /* built-in or user mode; returns ET_OK or an error */
int et312_play_routine(et312_engine_t *e, int start_block); /* modules already set: play as User1 */
void et312_silent(et312_engine_t *e);
void et312_set_levels(et312_engine_t *e, double a, double b);
void et312_set_ma(et312_engine_t *e, double fraction);
void et312_start_ramp(et312_engine_t *e);
void et312_step(et312_engine_t *e, et312_frame_t *out);
void et312_frame(et312_engine_t *e, et312_frame_t *out);

#endif
