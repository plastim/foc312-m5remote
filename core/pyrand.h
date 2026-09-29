/* Python-compatible random numbers: CPython's random.Random (MT19937, init_by_array seeding, getrandbits,
 * randint) so the C core draws exactly the numbers the Python engine draws for the same seed. The Python engine
 * is the reference; tests compare the two tick by tick (tests/test_remote_core.py). */
#ifndef PYRAND_H
#define PYRAND_H

#include <stdint.h>

#define PYRAND_N 624

typedef struct {
    uint32_t mt[PYRAND_N];
    int mti;
} pyrand_t;

/* random.Random(seed) for a non-negative integer seed below 2**64 */
void pyrand_seed(pyrand_t *r, uint64_t seed);
uint32_t pyrand_u32(pyrand_t *r);
/* random.getrandbits(k), 0 <= k <= 32 */
uint32_t pyrand_bits(pyrand_t *r, int k);
/* random.randint(a, b), a <= b, b - a < 2**32 */
int32_t pyrand_randint(pyrand_t *r, int32_t a, int32_t b);

#endif
