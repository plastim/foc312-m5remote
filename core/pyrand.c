/* CPython Modules/_randommodule.c (MT19937 by Matsumoto and Nishimura) and Lib/random.py (getrandbits, _randbelow,
 * randint), reimplemented so the numbers match Python's random.Random exactly. */
#include "pyrand.h"

#define M 397
#define MATRIX_A 0x9908b0dfU
#define UPPER_MASK 0x80000000U
#define LOWER_MASK 0x7fffffffU

static void init_genrand(pyrand_t *r, uint32_t s) {
    r->mt[0] = s;
    for (int i = 1; i < PYRAND_N; i++)
        r->mt[i] = 1812433253U * (r->mt[i - 1] ^ (r->mt[i - 1] >> 30)) + (uint32_t)i;
    r->mti = PYRAND_N;
}

static void init_by_array(pyrand_t *r, const uint32_t *key, int key_length) {
    uint32_t *mt = r->mt;
    init_genrand(r, 19650218U);
    int i = 1, j = 0;
    for (int k = (PYRAND_N > key_length ? PYRAND_N : key_length); k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) + key[j] + (uint32_t)j;
        i++; j++;
        if (i >= PYRAND_N) { mt[0] = mt[PYRAND_N - 1]; i = 1; }
        if (j >= key_length) j = 0;
    }
    for (int k = PYRAND_N - 1; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) - (uint32_t)i;
        i++;
        if (i >= PYRAND_N) { mt[0] = mt[PYRAND_N - 1]; i = 1; }
    }
    mt[0] = 0x80000000U;
}

void pyrand_seed(pyrand_t *r, uint64_t seed) {
    /* random.seed(int): the absolute value as little-endian 32-bit words; 0 -> [0] */
    uint32_t key[2];
    int n = 0;
    if (seed == 0) key[n++] = 0;
    while (seed) { key[n++] = (uint32_t)(seed & 0xffffffffU); seed >>= 32; }
    init_by_array(r, key, n);
}

uint32_t pyrand_u32(pyrand_t *r) {
    static const uint32_t mag01[2] = {0x0U, MATRIX_A};
    uint32_t y;
    uint32_t *mt = r->mt;
    if (r->mti >= PYRAND_N) {
        int kk;
        for (kk = 0; kk < PYRAND_N - M; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < PYRAND_N - 1; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + (M - PYRAND_N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        y = (mt[PYRAND_N - 1] & UPPER_MASK) | (mt[0] & LOWER_MASK);
        mt[PYRAND_N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        r->mti = 0;
    }
    y = mt[r->mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

uint32_t pyrand_bits(pyrand_t *r, int k) {
    if (k <= 0) return 0;
    return pyrand_u32(r) >> (32 - k);
}

static int bit_length(uint32_t n) {
    int k = 0;
    while (n) { k++; n >>= 1; }
    return k;
}

int32_t pyrand_randint(pyrand_t *r, int32_t a, int32_t b) {
    /* randint(a, b) = randrange(a, b + 1) = a + _randbelow(width); _randbelow draws k = width.bit_length() bits
     * and retries while the draw is >= width (so width 1 still consumes draws, exactly as Python does) */
    uint32_t width = (uint32_t)(b - a) + 1U;
    int k = bit_length(width);
    uint32_t v = pyrand_bits(r, k);
    while (v >= width) v = pyrand_bits(r, k);
    return a + (int32_t)v;
}
