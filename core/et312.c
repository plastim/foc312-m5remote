/* ET-312B mode engine, C99 port of stimengine/et312/{vm,modes,engine}.py (the reference; see et312.h). Comments
 * cite the Python functions each part mirrors; the firmware addresses behind them are documented there. */
#include "et312.h"

#include <math.h>
#include <string.h>

/* ---- interoperability tables (register reset images and constants the VM needs to behave like the box) ---- */

/* vm.DEFAULTS: reset image of $80..$bf (channel B gets bytes 8.. at $188..$1bf) */
static const uint8_t DEFAULTS[0x40] = {
    0x00, 0x00, 0x02, 0x00, 0x00, 0x03, 0x0F, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
    0x3E, 0x3E, 0x00, 0x00,
    0x9C, 0x9C, 0xFF, 0x07, 0x01, 0xFC, 0xFC, 0x01, 0x00,
    0xFF, 0xCD, 0xFF, 0x01, 0x01, 0xFF, 0xFF, 0x00, 0x00,
    0x16, 0x09, 0x64, 0x01, 0x01, 0xFF, 0xFF, 0x08, 0x00,
    0x82, 0x32, 0xC8, 0x01, 0x01, 0xFF, 0xFF, 0x04, 0x00,
};
/* vm.ADV_DEFAULTS: reset image of $1f0..$1ff */
static const uint8_t ADV_DEFAULTS[0x10] = {
    0xC0, 0x2C, 0x20, 0x87, 0x02, 0x77, 0x76, 0x76, 0xE1, 0x14, 0xD7, 0x01, 0x19, 0x05, 0x82, 0x05,
};
static const uint8_t MOD_BLOCKS[4] = {0x9C, 0xA5, 0xAE, 0xB7};   /* ramp, intensity, frequency, width */

static int invert_c(int z) {          /* vm.INVERT_C */
    switch (z) {
    case 0x9C: return 0xCD + 0xFF;
    case 0xA5: return 0xA5 + 0xFF;
    case 0xAE: return 0x0F + 0xFA;
    default: return 0x46 + 0xFA;      /* 0xB7 */
    }
}
static int adv_first(int z) {         /* vm.ADV_FIRST */
    switch (z) {
    case 0x9C: return 0x1F8;
    case 0xA5: return 0x1FA;
    case 0xAE: return 0x1FC;
    default: return 0x1FE;
    }
}

/* modes.CORE_BLOCKS: gates off / on (defined here; every other block comes from the host) */
static const uint8_t CORE_BLOCK_0[] = {0x90, 0x06, 0x00};
static const uint8_t CORE_BLOCK_1[] = {0x90, 0x07, 0x00};

static void set_error(et312_vm_t *vm, int err) {
    if (vm->error == ET_OK) vm->error = err;
}

/* ---- VM (vm.ET312VM) ---- */

void et312_vm_init(et312_vm_t *vm, uint64_t seed) {
    memset(vm, 0, sizeof *vm);
    pyrand_seed(&vm->rng, seed);
    vm->ma_knob = 0.5;
    vm->ma_override_r2 = -1;
    vm->blocks[0].code = CORE_BLOCK_0; vm->blocks[0].len = sizeof CORE_BLOCK_0;
    vm->blocks[1].code = CORE_BLOCK_1; vm->blocks[1].len = sizeof CORE_BLOCK_1;
    et312_vm_reset_defaults(vm);
}

void et312_vm_reset_defaults(et312_vm_t *vm) {
    uint8_t *m = vm->mem;
    memcpy(m + 0x80, DEFAULTS, 0x40);
    memcpy(m + 0x188, DEFAULTS + 8, 0x38);
    bool any = false;
    for (int i = 0x1F0; i < 0x200; i++) any |= m[i] != 0;
    if (!any) memcpy(m + 0x1F0, ADV_DEFAULTS, 0x10);   /* only at power-on */
    vm->pending_next = false;
    et312_vm_update_ma(vm);
}

static int resolve(int page, int addr, int t) {
    int full = ((page & 3) << 8) | (addr & 0xFF);
    if (full >= 0x8C && full < 0xC0 && t) full += 0x100;
    return full;
}

int et312_vm_random_between(et312_vm_t *vm) {
    int lo = vm->mem[0x8D], hi = vm->mem[0x8E];
    if (hi < lo) {                    /* the firmware halts ("Failure 15") */
        set_error(vm, ET_ERR_RANDOM_RANGE);
        return lo;
    }
    return pyrand_randint(&vm->rng, lo, hi);
}

/* one op, one channel pass (vm._exec_pass); op = code[0..] already length-checked */
static void exec_pass(et312_vm_t *vm, const uint8_t *op, int t) {
    uint8_t *m = vm->mem;
    uint8_t b = op[0];
    int acc = 0x8C + (t << 8);
    if (b >= 0x80) {                                  /* set */
        int a = resolve((b & 0x40) ? 1 : 0, 0x80 | (b & 0x3F), t);
        if (a < ET312_MEM) m[a] = op[1]; else set_error(vm, ET_ERR_BAD_OPCODE);
    } else if (b < 0x40) {                            /* blk: n-byte block write */
        int cnt = (b >> 2) & 7;
        int a = resolve(b & 3, op[1], t);
        if (a + cnt <= ET312_MEM) memcpy(m + a, op + 2, (size_t)cnt); else set_error(vm, ET_ERR_BAD_OPCODE);
    } else if (b < 0x50) {                            /* ldacc / stacc / shr / rand */
        int a = resolve(b & 3, op[1], t);
        if (a >= ET312_MEM) { set_error(vm, ET_ERR_BAD_OPCODE); return; }
        switch ((b >> 2) & 3) {
        case 0: m[acc] = m[a]; break;
        case 1: m[a] = m[acc]; break;
        case 2: m[a] >>= 1; break;
        default: m[a] = (uint8_t)et312_vm_random_between(vm); break;
        }
    } else if (b < 0x60) {                            /* add / and / or / xor */
        int a = resolve(b & 3, op[1], t);
        uint8_t v = op[2];
        if (a >= ET312_MEM) { set_error(vm, ET_ERR_BAD_OPCODE); return; }
        switch ((b >> 2) & 3) {
        case 0: m[a] = (uint8_t)(m[a] + v); break;
        case 1: m[a] &= v; break;
        case 2: m[a] |= v; break;
        default: m[a] ^= v; break;
        }
    } else {                                          /* 0x70.. ifacc */
        int a = resolve(b & 3, op[1], t);
        if (a < ET312_MEM && m[a] == m[acc]) vm->pending_next = true;
    }
}

/* op length per vm.decode_module; 0 = end of block (terminator, truncated op, or unknown opcode) */
static int op_len(const uint8_t *code, int i, int n, int *err) {
    uint8_t b = code[i];
    int len;
    if (b < 0x20) return 0;
    if (b >= 0x80) len = 2;
    else if (b < 0x40) len = 2 + ((b >> 2) & 7);
    else if (b < 0x50) len = 2;
    else if (b < 0x60) len = 3;
    else if (b >= 0x70) len = 2;
    else { *err = ET_ERR_BAD_OPCODE; return 0; }
    /* decode_module breaks when the op's last byte would be at or past the end ("i + 1 + cnt >= n" for blk) */
    if (b < 0x40 && b >= 0x20) return (i + 1 + ((b >> 2) & 7) >= n) ? 0 : len;
    return (i + len - 1 >= n) ? 0 : len;
}

bool et312_bytecode_valid(const uint8_t *code, uint16_t len) {
    int err = ET_OK, i = 0, n = len;
    while (i < n) {
        int l = op_len(code, i, n, &err);
        if (l == 0) break;
        i += l;
    }
    return err == ET_OK;
}

void et312_vm_load_block(et312_vm_t *vm, int idx) {
    if (idx < 0 || idx > 255 || vm->blocks[idx].code == NULL) {
        set_error(vm, ET_ERR_NO_BLOCK);
        return;
    }
    const uint8_t *code = vm->blocks[idx].code;
    int n = vm->blocks[idx].len;
    int i = 0;
    while (i < n) {
        int err = ET_OK;
        int len = op_len(code, i, n, &err);
        if (err != ET_OK) set_error(vm, err);
        if (len == 0) break;
        /* vm._exec: channel A pass if mask bit0, then channel B pass if mask bit1 (re-read after the A pass) */
        uint8_t mask = vm->mem[0x85];
        if (mask != 0) {
            int t = (mask & 1) ? 0 : 1;
            for (;;) {
                exec_pass(vm, code + i, t);
                if (t == 1 || !(vm->mem[0x85] & 2)) break;
                t = 1;
            }
        }
        i += len;
    }
}

void et312_vm_update_ma(et312_vm_t *vm) {
    uint8_t *m = vm->mem;
    int hi = m[0x86], lo = m[0x87];
    int r2 = vm->ma_override_r2 >= 0 ? vm->ma_override_r2 : (int)rint(255.0 * (1.0 - vm->ma_knob));  /* round half even */
    if (r2 < 0) r2 = 0;
    if (r2 > 255) r2 = 255;
    int rng = (lo - hi) & 0xFF;
    int v;
    if (rng == 0) {
        v = hi;
    } else {
        int q = 32640 / rng;
        v = (r2 * 128) / q + hi;
        if (v > 255) v = 255;
    }
    m[0x20D] = (uint8_t)v;
}

static bool timer(const uint8_t *m, int tsel, int *now) {   /* vm._timer */
    switch (tsel & 3) {
    case 1: *now = m[0x88]; return true;
    case 2: *now = m[0x8B]; return (m[0x88] & 7) == 7;
    case 3: *now = m[0x89]; return m[0x88] == 0;
    default: *now = 0; return false;
    }
}

static void block_timer(et312_vm_t *vm, int page) {
    uint8_t *m = vm->mem;
    int base = page << 8;
    int sel = m[base + 0x96], now;
    if ((sel & 3) == 0) return;
    if (!timer(m, sel, &now)) return;
    if (((now - m[base + 0x94]) & 0xFF) < m[base + 0x95]) return;
    m[base + 0x94] = (uint8_t)(now + 1);
    int target = m[base + 0x97];
    if (page == 1 && target == m[0x97]) return;
    et312_vm_load_block(vm, target);
}

static void gate(et312_vm_t *vm, int page) {
    uint8_t *m = vm->mem;
    int base = page << 8;
    int sel = m[base + 0x9A];
    if ((sel & 3) == 0) return;
    int gv = m[base + 0x90], t, now;
    if (!(gv & 1)) {
        t = m[base + 0x99];
        if (sel & 0x04) t = m[0x1FB];
        if (sel & 0x08) t = m[0x20D];
        if (t == 0) {
            m[base + 0x90] = (uint8_t)(gv | 1);
            return;
        }
    } else {
        t = m[base + 0x98];
        if (sel & 0x20) t = m[0x1FD];
        if (sel & 0x40) t = m[0x20D];
    }
    if (!timer(m, sel, &now)) return;
    if (((now - m[base + 0x9B]) & 0xFF) < t) return;
    m[base + 0x9B] = (uint8_t)(now + 1);
    m[base + 0x90] = (uint8_t)(gv ^ 1);
}

static int source(const uint8_t *m, int page, int z, int sel, int offset) {   /* -1 = none */
    int v;
    switch (sel & 0x0C) {
    case 0x0C: v = m[((1 - page) << 8) + z + offset]; break;
    case 0x08: v = m[0x20D]; break;
    case 0x04: v = m[adv_first(z)]; break;
    default: return -1;
    }
    if (sel & 0x10) v = (invert_c(z) - v) & 0xFF;
    return v;
}

static void action(et312_vm_t *vm, int page, int z, int a) {
    uint8_t *m = vm->mem;
    int bz = (page << 8) + z;
    if (a == 0xFE) {
        m[(page << 8) + 0x90] ^= 6;
        a = 0xFF;
    }
    if (a == 0xFF) {
        m[bz + 4] = (uint8_t)(-m[bz + 4]);
        return;
    }
    if (page == 1 && (a == m[z + 5] || a == m[z + 6])) return;
    et312_vm_load_block(vm, a);
}

static void at_min(et312_vm_t *vm, int page, int z) {
    uint8_t *m = vm->mem;
    int bz = (page << 8) + z;
    m[bz] = m[bz + 1];
    int a = m[bz + 5];
    if (a == 0xFC) return;
    if (a == 0xFD) { m[bz] = m[bz + 2]; return; }
    action(vm, page, z, a);
}

static void at_max(et312_vm_t *vm, int page, int z) {
    uint8_t *m = vm->mem;
    int bz = (page << 8) + z;
    m[bz] = m[bz + 2];
    int a = m[bz + 6];
    if (a == 0xFC) return;
    if (a == 0xFD) { m[bz] = m[bz + 1]; return; }
    action(vm, page, z, a);
}

static void modulate(et312_vm_t *vm, int page, int z) {
    uint8_t *m = vm->mem;
    int base = page << 8, bz = base + z;
    int sel = m[bz + 7];
    int tsel = sel & 3;
    if (tsel == 0) {
        int v = source(m, page, z, sel, 0);
        if (v >= 0) m[bz] = (uint8_t)v;
        return;
    }
    int rate;
    switch (sel & 0x60) {
    case 0x60: rate = m[((1 - page) << 8) + z + 3]; break;
    case 0x40: rate = m[0x20D]; break;
    case 0x20: rate = m[adv_first(z) + 1]; break;
    default: rate = m[bz + 3]; break;
    }
    if (sel & 0x80) rate = (invert_c(z) - rate) & 0xFF;
    int now;
    if (!timer(m, tsel, &now)) return;
    if (((now - m[bz + 8]) & 0xFF) < rate) return;
    m[bz + 8] = (uint8_t)(now + 1);
    int mn = source(m, page, z, sel, 1);
    if (mn < 0) mn = m[bz + 1];
    m[bz + 1] = (uint8_t)mn;
    int val = m[bz], step = m[bz + 4], mx = m[bz + 2];
    int nw = val + step;
    if (step >= 0x80) {
        if (nw < 0x100) { at_min(vm, page, z); return; }
        nw &= 0xFF;
        if (nw < mn) { at_min(vm, page, z); return; }
        if (nw >= mx) { at_max(vm, page, z); return; }
    } else {
        if (nw >= 0x100 || nw >= mx) { at_max(vm, page, z); return; }
    }
    m[bz] = (uint8_t)nw;
}

void et312_vm_tick(et312_vm_t *vm) {
    uint8_t *m = vm->mem;
    vm->tick_count++;
    m[0x88]++;
    if (m[0x88] == 0) {
        m[0x89]++;
        if (m[0x89] == 0) m[0x8A]++;
    }
    if ((m[0x88] & 7) == 7) m[0x8B]++;
    for (int page = 0; page < 2; page++) {
        block_timer(vm, page);
        gate(vm, page);
        for (int i = 0; i < 4; i++) modulate(vm, page, MOD_BLOCKS[i]);
    }
    if (vm->pending_next) {
        vm->pending_next = false;
        et312_vm_load_block(vm, m[0x84]);
    }
    et312_vm_update_ma(vm);
}

/* ---- modes (modes.select_mode / _dispatch) ---- */

static const struct { int mode, first, second; } MODE_BLOCKS_TABLE[] = {
    {ET_WAVES, 11, 12}, {ET_STROKE, 3, 4}, {ET_CLIMB, 5, 8}, {ET_COMBO, 13, 33}, {ET_INTENSE, 14, 2},
    {ET_RHYTHM, 15, -1}, {ET_RANDOM2, 32, -1}, {ET_TOGGLE, 18, -1}, {ET_ORGASM, 24, -1},
    {ET_TORMENT, 28, -1}, {ET_PHASE3, 22, -1},
};

static void dispatch(et312_vm_t *vm, int mode, int mask, const int *user_start) {
    uint8_t *m = vm->mem;
    for (unsigned i = 0; i < sizeof MODE_BLOCKS_TABLE / sizeof MODE_BLOCKS_TABLE[0]; i++) {
        if (MODE_BLOCKS_TABLE[i].mode == mode) {
            et312_vm_load_block(vm, MODE_BLOCKS_TABLE[i].first);
            if (MODE_BLOCKS_TABLE[i].second >= 0 && (mask & 2)) et312_vm_load_block(vm, MODE_BLOCKS_TABLE[i].second);
            return;
        }
    }
    if (mode == ET_AUDIO1 || mode == ET_AUDIO2 || mode == ET_AUDIO3) {
        if (mode == ET_AUDIO1) m[0x83] = 0x40;
        uint8_t gv = mode == ET_AUDIO3 ? 0x67 : 0x47;
        if (mask & 1) m[0x90] = gv;
        if (mask & 2) m[0x190] = gv;
        et312_vm_load_block(vm, mode != ET_AUDIO3 ? 23 : 34);
        if (mode == ET_AUDIO3) m[0x83] = 0x04;
        return;
    }
    if (mode == ET_RANDOM1) {
        m[0x74] = 1;
        return;
    }
    if (mode == ET_PHASE1 || mode == ET_PHASE2) {
        m[0x83] = 0x05;
        et312_vm_load_block(vm, 20);
        if (mask & 2) et312_vm_load_block(vm, 21);
        if (mode != ET_PHASE1) et312_vm_load_block(vm, 35);
        return;
    }
    if (mode >= ET_USER1 && mode <= ET_USER7) {
        int start = user_start ? user_start[mode - ET_USER1] : -1;
        if (start < 0) { set_error(vm, ET_ERR_NO_USER_START); return; }
        et312_vm_load_block(vm, start);
        return;
    }
    set_error(vm, ET_ERR_BAD_MODE);
}

void et312_select_mode(et312_vm_t *vm, int mode, int split_a, int split_b, const int *user_start) {
    uint8_t *m = vm->mem;
    et312_vm_reset_defaults(vm);
    et312_vm_load_block(vm, 1);
    if (m[0x74]) mode = m[0x74];
    if (mode == ET_SPLIT) {
        m[0x83] |= 0x10;
        m[0x85] = 1; dispatch(vm, split_a, 1, user_start);
        m[0x85] = 2; dispatch(vm, split_b, 2, user_start);
    } else {
        m[0x83] &= (uint8_t)~0x10;
        m[0x85] = 3; dispatch(vm, mode, 3, user_start);
    }
    m[0x85] = 3;
    et312_vm_update_ma(vm);
}

/* ---- engine (engine.ET312Engine) ---- */

#define MASTER_MSB_TICKS 128

void et312_engine_init(et312_engine_t *e, uint64_t seed) {
    memset(e, 0, sizeof *e);
    et312_vm_init(&e->vm, seed);
    for (int i = 0; i <= ET_USER7 - ET_USER1; i++) e->user_start[i] = -1;
    e->split_a = ET_STROKE;
    e->split_b = ET_WAVES;
    e->power = 1;
    e->level_a = e->level_b = 0.5;
    e->vm.mem[0x1F4] = (uint8_t)e->power;
    et312_set_ma(e, 0.5);
    e->mode = 0;
    e->v_prev = -1;
}

void et312_set_block(et312_engine_t *e, int idx, const uint8_t *code, uint16_t len) {
    if (idx < 0 || idx > 255) return;
    e->vm.blocks[idx].code = code;
    e->vm.blocks[idx].len = len;
    bool all = true;
    for (int i = 2; i <= 35; i++) all &= e->vm.blocks[i].code != NULL;
    e->builtins_available = all;
}

void et312_clear_user_blocks(et312_engine_t *e) {
    for (int i = 0x80; i < 256; i++) { e->vm.blocks[i].code = NULL; e->vm.blocks[i].len = 0; }
}

bool et312_is_builtin(int mode) {
    return !(mode >= ET_USER1 && mode <= ET_USER7);
}

void et312_silent(et312_engine_t *e) {
    e->mode = 0;
    e->variant = 0;
    e->routine_loaded = false;
    et312_vm_reset_defaults(&e->vm);
    et312_vm_load_block(&e->vm, 0);
}

static void after_select(et312_engine_t *e) {   /* ET312Engine._after_select */
    if (!e->skip_mode_ramp) return;
    uint8_t *m = e->vm.mem;
    m[0x9C] = m[0x9E];
    m[0x19C] = m[0x19E];
}

static void random1_pick(et312_engine_t *e) {
    et312_vm_t *vm = &e->vm;
    uint8_t *m = vm->mem;
    m[0x8D] = 0x76; m[0x8E] = 0x7B;
    m[0x74] = (uint8_t)et312_vm_random_between(vm);
    m[0x8D] = 20; m[0x8E] = 120;
    m[0x75] = (uint8_t)(e->master_msb + et312_vm_random_between(vm));
    m[0x8D] = 140; m[0x8E] = 184;
    int raw = et312_vm_random_between(vm);
    if (!e->random1_uniform) {
        int x = 3 * raw;
        vm->ma_override_r2 = 255 - (x < 255 ? x : 255);
    } else {
        vm->ma_override_r2 = pyrand_randint(&vm->rng, 0, 255);
    }
    et312_select_mode(vm, ET_RANDOM1, e->split_a, e->split_b, e->user_start);
    after_select(e);
}

static uint32_t clock_of(const et312_engine_t *e) { return e->vm.tick_count + e->frozen; }

int et312_set_mode(et312_engine_t *e, int mode) {
    if (!e->builtins_available && et312_is_builtin(mode)) return ET_ERR_NO_BLOCK;
    if (et312_is_builtin(mode)) e->routine_loaded = false;
    e->mode = mode;
    /* a PlaStim variant plays its base mode's own program; only the clock differs (et312_step) */
    int base = mode == ET_CLIMB_SLOW || mode == ET_CLIMB_HOLD ? ET_CLIMB : mode;
    e->variant = mode == ET_CLIMB_SLOW ? 1 : mode == ET_CLIMB_HOLD ? 2 : 0;
    e->v_start = clock_of(e);
    e->v_prev = -1;
    e->v_top = 0;
    e->v_div = 0;
    e->v_hold = 0;
    e->v_held = false;
    e->vm.mem[0x74] = 0;
    e->vm.ma_override_r2 = -1;
    et312_select_mode(&e->vm, base, e->split_a, e->split_b, e->user_start);
    after_select(e);
    if (e->mode == ET_RANDOM1) random1_pick(e);
    return e->vm.error;
}

int et312_play_routine(et312_engine_t *e, int start_block) {
    e->user_start[0] = start_block;       /* User1 */
    e->routine_loaded = true;
    return et312_set_mode(e, ET_USER1);
}

static double clip01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

void et312_set_levels(et312_engine_t *e, double a, double b) {
    e->level_a = clip01(a);
    e->level_b = clip01(b);
}

void et312_set_ma(et312_engine_t *e, double fraction) {
    e->vm.ma_knob = clip01(fraction);
    et312_vm_update_ma(&e->vm);
}

void et312_start_ramp(et312_engine_t *e) {
    uint8_t *m = e->vm.mem;
    for (int base = 0; base <= 0x100; base += 0x100) {
        m[base + 0xA4] = 0;
        m[base + 0xA3] = 0x27;
        m[base + 0x9C] = m[0x1F8];
    }
}

/* engine.gate_pulse: gate value -> polarity, biphasic, alternating, any pulse */
static void gate_pulse(int gv, int *pol, bool *bi, bool *alt, bool *pulses) {
    int first = (gv & 0x10) ? -1 : 1;
    *bi = false; *alt = false; *pulses = true;
    if (gv & 0x08) { *pol = -first; *alt = true; return; }
    int halves = (gv >> 1) & 3;
    if (halves == 3) { *pol = first; *bi = true; return; }
    if (halves == 2) { *pol = first; return; }
    if (halves == 1) { *pol = -first; return; }
    *pol = first; *pulses = false;
}

static double amplitude(const et312_engine_t *e, int ch, double knob, int ramp, int inten) {
    double scale = ((double)ramp / 255.0) * ((double)inten / 255.0);
    if (e->level_linear) return clip01(knob * scale);
    double k = 255.0 / (double)(5 - e->power);
    double off = ch == 0 ? 55.0 : 43.0;
    double code = (knob * k + off) * scale;
    return clip01((code - off) / k);
}

static void channel(const et312_engine_t *e, int ch, double knob, int ctrl, et312_channel_t *c) {
    const uint8_t *m = e->vm.mem;
    int base = ch << 8;
    int gv = m[base + 0x90], ramp = m[base + 0x9C], inten = m[base + 0xA5];
    int f = m[base + 0xAE] < 9 ? 9 : m[base + 0xAE];
    int w = m[base + 0xB7] < 50 ? 50 : m[base + 0xB7];
    bool pulses;
    gate_pulse(gv, &c->leading_polarity, &c->biphasic, &c->alternating, &pulses);
    int period_us = 256 * (f + 1) + (c->biphasic ? 2 : 1) * w;
    c->intensity = amplitude(e, ch, knob, ramp, inten);
    c->gate_on = (gv & 1) && !(ctrl & 0x02) && pulses;
    c->pulse_rate_hz = 1e6 / (double)period_us;
    c->pulse_width_us = (double)w;
    c->raw_gate = (uint8_t)gv; c->raw_ramp = (uint8_t)ramp; c->raw_intensity = (uint8_t)inten;
    c->raw_frequency = m[base + 0xAE]; c->raw_width = m[base + 0xB7];
}

void et312_frame(et312_engine_t *e, et312_frame_t *out) {
    const uint8_t *m = e->vm.mem;
    int ctrl = m[0x83];
    channel(e, 0, e->level_a, ctrl, &out->a);
    channel(e, 1, e->level_b, ctrl, &out->b);
    out->phase_mode = ET_PHASE_NONE;
    if (ctrl & 0x08) {
        /* Phase 3: B mirrors A's gate, rate, width and pulse type (its own polarity and alternating stay) */
        out->phase_mode = ET_PHASE_LINKED;
        out->b.gate_on = out->a.gate_on;
        out->b.pulse_rate_hz = out->a.pulse_rate_hz;
        out->b.pulse_width_us = out->a.pulse_width_us;
        out->b.biphasic = out->a.biphasic;
    } else if (ctrl & 0x04) {
        out->phase_mode = ET_PHASE_INTERLEAVED;
    }
    out->tick = clock_of(e);
    out->mode = (e->mode == ET_RANDOM1 && m[0x74] > 1) ? m[0x74] : e->mode;
    out->ma_value = m[0x20D];
    out->ctrl_flags = (uint8_t)ctrl;
}

/* ET312Engine._variant_advance, exactly: Climb counts channel A's frequency register ($ae) down from its start to its
 * minimum ($af) in steps of $b2 (-1 / -2 / -4); its at-min action jumps it back up (the drop). slow finish: the last
 * 10 % of the register's travel at 1/3 speed; peak hold: the VM pauses at the climb's last value for a quarter of
 * that climb's ticks. Returns false when the VM should not advance on this tick. */
static bool variant_advance(et312_engine_t *e) {
    const uint8_t *m = e->vm.mem;
    int f = m[0xAE], fmin = m[0xAF];
    int step = m[0xB2] >= 128 ? (int)m[0xB2] - 256 : (int)m[0xB2];
    if (e->v_prev < 0 || f > e->v_prev + 20) {           /* a new climb (mode start, or the drop just happened) */
        e->v_start = clock_of(e);
        e->v_top = f;
        e->v_held = false;
        e->v_hold = 0;
        e->v_div = 0;
    }
    e->v_prev = f;
    if (step >= 0) return true;                          /* not climbing: no change */
    if (e->variant == 1) {
        if (f <= fmin + 0.10 * (double)(e->v_top - fmin)) {
            e->v_div = (e->v_div + 1) % 3;
            return e->v_div == 0;
        }
        return true;
    }
    if (e->variant == 2) {
        if (e->v_hold > 0) { e->v_hold--; return false; }
        if (!e->v_held && f < fmin - step) {             /* the last value before the drop: hold it */
            e->v_held = true;
            e->v_hold = (clock_of(e) - e->v_start + 2) / 4;   /* a quarter of the climb, rounded */
            if (e->v_hold > 0) { e->v_hold--; return false; }
        }
    }
    return true;
}

void et312_step(et312_engine_t *e, et312_frame_t *out) {
    if (e->variant && !variant_advance(e)) {
        e->frozen++;                                     /* the VM holds still this tick; the output repeats */
        et312_frame(e, out);
        return;
    }
    et312_vm_tick(&e->vm);
    if (e->vm.tick_count % MASTER_MSB_TICKS == 0) {
        e->master_msb++;
        if (e->mode == ET_RANDOM1 && e->master_msb == e->vm.mem[0x75]) random1_pick(e);
    }
    et312_frame(e, out);
}
