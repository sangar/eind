#include "hash.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define SLOT_EMPTY UINT32_MAX
#define SLOT_DELETED (UINT32_MAX - 1)

uint64_t hash_bytes(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static uint32_t capacity_for(uint32_t expected) {
    uint32_t cap = 16;
    while (cap < expected * 2) cap *= 2;
    return cap;
}

void idtable_init(IdTable *t, uint32_t expected) {
    t->capacity = capacity_for(expected);
    t->slots = xmalloc((size_t)t->capacity * sizeof *t->slots);
    memset(t->slots, 0xff, (size_t)t->capacity * sizeof *t->slots);
    t->used = 0;
    t->live = 0;
}

void idtable_free(IdTable *t) {
    free(t->slots);
    *t = (IdTable){0};
}

uint32_t idtable_find(const IdTable *t, uint64_t hash, IdEqualFn eq, void *ctx) {
    uint32_t mask = t->capacity - 1;
    for (uint32_t i = (uint32_t)hash & mask;; i = (i + 1) & mask) {
        uint32_t id = t->slots[i];
        if (id == SLOT_EMPTY) return UINT32_MAX;
        if (id != SLOT_DELETED && eq(ctx, id)) return id;
    }
}

static void place(IdTable *t, uint64_t hash, uint32_t id) {
    uint32_t mask = t->capacity - 1;
    uint32_t i = (uint32_t)hash & mask;
    while (t->slots[i] != SLOT_EMPTY && t->slots[i] != SLOT_DELETED) i = (i + 1) & mask;
    if (t->slots[i] == SLOT_EMPTY) t->used++;
    t->slots[i] = id;
    t->live++;
}

static void resize(IdTable *t, IdHashFn rehash, void *ctx) {
    uint32_t *old = t->slots;
    uint32_t old_cap = t->capacity;
    idtable_init(t, t->live + 1);
    for (uint32_t i = 0; i < old_cap; i++)
        if (old[i] != SLOT_EMPTY && old[i] != SLOT_DELETED) place(t, rehash(ctx, old[i]), old[i]);
    free(old);
}

void idtable_insert(IdTable *t, uint64_t hash, uint32_t id, IdHashFn rehash, void *ctx) {
    if ((uint64_t)(t->used + 1) * 4 > (uint64_t)t->capacity * 3) resize(t, rehash, ctx);
    place(t, hash, id);
}

bool idtable_remove(IdTable *t, uint64_t hash, uint32_t id) {
    uint32_t mask = t->capacity - 1;
    for (uint32_t i = (uint32_t)hash & mask;; i = (i + 1) & mask) {
        if (t->slots[i] == SLOT_EMPTY) return false;
        if (t->slots[i] == id) {
            t->slots[i] = SLOT_DELETED;
            t->live--;
            return true;
        }
    }
}
