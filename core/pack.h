/* Pattern packs on the M5 remote (format: stimengine/remote/pack.py). pack_open() validates the whole file before
 * anything is used - CRC-32, every length and offset, module numbers, every bytecode op - so a damaged or truncated
 * file is rejected rather than played. No allocation: entries point into the caller's buffer, which must outlive
 * the pack and the engine using it. */
#ifndef PACK_H
#define PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "et312.h"

#define PACK_NAME_MAX 40
#define PACK_BUILTIN_BLOCKS 36

enum { PACK_OK = 0, PACK_ERR_MAGIC, PACK_ERR_CRC, PACK_ERR_VERSION, PACK_ERR_TRUNCATED, PACK_ERR_TRAILING,
       PACK_ERR_KIND, PACK_ERR_NAME, PACK_ERR_MODULE, PACK_ERR_START, PACK_ERR_BYTECODE, PACK_ERR_MODE,
       PACK_ERR_INDEX, PACK_ERR_NO_BUILTINS };
enum { PACK_KIND_BUILTIN = 1, PACK_KIND_ROUTINE = 2 };
enum { PACK_GROUP_BUILTIN = 0, PACK_GROUP_EROSLINK, PACK_GROUP_EXAMPLES, PACK_GROUP_YOURS, PACK_GROUP_OURS };

typedef struct {
    const uint8_t *data;
    size_t len;
    bool has_builtin;
    uint16_t count;
    const uint8_t *builtin[PACK_BUILTIN_BLOCKS];
    uint16_t builtin_len[PACK_BUILTIN_BLOCKS];
    size_t entries_at;            /* offset of the first entry */
} pack_t;

typedef struct {
    int kind, group;
    char name[PACK_NAME_MAX + 1];
    int mode;                     /* built-in */
    int start, nmod;              /* routine */
    const uint8_t *modules;       /* routine: nmod x (u8 number, u16 length, bytecode) */
} pack_entry_t;

uint32_t pack_crc32(uint32_t crc, const uint8_t *p, size_t n);   /* zlib CRC-32; start with 0 */
int pack_open(pack_t *p, const uint8_t *data, size_t len);
int pack_entry(const pack_t *p, int index, pack_entry_t *out);
/* the built-in blocks into the engine (call once after pack_open) */
void pack_install_builtins(const pack_t *p, et312_engine_t *e);
/* play an entry: a built-in mode, or a routine (its modules replace the previous routine's) */
int pack_play(const pack_t *p, const pack_entry_t *entry, et312_engine_t *e);
const char *pack_error_text(int err);

#endif
