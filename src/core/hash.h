#ifndef EIND_HASH_H
#define EIND_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t hash_bytes(const void *data, size_t len, uint64_t seed);

/*
 * IdTable is an open-addressing set of uint32 ids whose keys live elsewhere,
 * typically in index records. The caller hashes keys and decides equality,
 * so the table itself stores nothing but ids.
 */
typedef struct {
    uint32_t *slots;
    uint32_t capacity; /* power of two */
    uint32_t used;     /* live ids plus deleted markers */
    uint32_t live;
} IdTable;

typedef bool (*IdEqualFn)(void *ctx, uint32_t id);
typedef uint64_t (*IdHashFn)(void *ctx, uint32_t id);

void idtable_init(IdTable *t, uint32_t expected);
void idtable_free(IdTable *t);
/* idtable_find returns the matching id or UINT32_MAX. */
uint32_t idtable_find(const IdTable *t, uint64_t hash, IdEqualFn eq, void *ctx);
void idtable_insert(IdTable *t, uint64_t hash, uint32_t id, IdHashFn rehash, void *ctx);
bool idtable_remove(IdTable *t, uint64_t hash, uint32_t id);

#endif
