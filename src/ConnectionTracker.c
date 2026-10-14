/* ConnectionTracker.c - reconciles OS snapshots with the tracked arch set. */
#include "ConnectionTracker.h"

#include <stdlib.h>
#include <string.h>

struct ConnectionTracker {
    ConnectionArch **items;
    int              count;
    int              cap;

    /* open addressing hash map: key hash -> index into items (1 based, 0 = empty) */
    uint64_t        *mapHash;
    uint32_t        *mapSlot;
    uint32_t         mapCap;      /* power of two */

    double           expiry;
};

/* ------------------------------------------------------------------ map --- */

static void RebuildMap(ConnectionTracker *t)
{
    memset(t->mapHash, 0, sizeof(uint64_t) * t->mapCap);
    memset(t->mapSlot, 0, sizeof(uint32_t) * t->mapCap);
    uint32_t mask = t->mapCap - 1;
    for (int i = 0; i < t->count; i++) {
        uint64_t h = NetConnKey_Hash(&t->items[i]->key);
        uint64_t slotHash = h ? h : 1;
        uint32_t idx = (uint32_t)h & mask;
        while (t->mapHash[idx] != 0) idx = (idx + 1) & mask;
        t->mapHash[idx] = slotHash;
        t->mapSlot[idx] = (uint32_t)i + 1;
    }
}

static bool EnsureMapCapacity(ConnectionTracker *t, int needed)
{
    uint32_t want = 16;
    while (want < (uint32_t)needed * 2) want <<= 1;
    if (want <= t->mapCap) return true;

    uint64_t *nh = (uint64_t *)realloc(t->mapHash, sizeof(uint64_t) * want);
    uint32_t *ns = (uint32_t *)realloc(t->mapSlot, sizeof(uint32_t) * want);
    if (!nh || !ns) {
        if (nh) t->mapHash = nh;
        if (ns) t->mapSlot = ns;
        return false;
    }
    t->mapHash = nh;
    t->mapSlot = ns;
    t->mapCap = want;
    RebuildMap(t);
    return true;
}

/* Returns the index of the arch matching key, or -1.                          */
static int MapFind(const ConnectionTracker *t, const NetConnKey *key)
{
    if (t->mapCap == 0) return -1;
    uint64_t h = NetConnKey_Hash(key);
    uint32_t mask = t->mapCap - 1;
    uint32_t idx = (uint32_t)h & mask;
    while (t->mapHash[idx] != 0) {
        int item = (int)(t->mapSlot[idx] - 1);
        if (item >= 0 && item < t->count && NetConnKey_Equals(&t->items[item]->key, key)) {
            return item;
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

/* ------------------------------------------------------------- lifecycle --- */

ConnectionTracker *ConnectionTracker_Create(double expirySeconds)
{
    ConnectionTracker *t = (ConnectionTracker *)calloc(1, sizeof(ConnectionTracker));
    if (!t) return NULL;
    t->expiry = (expirySeconds > 0.0) ? expirySeconds : 30.0;
    t->cap = 128;
    t->items = (ConnectionArch **)calloc((size_t)t->cap, sizeof(ConnectionArch *));
    EnsureMapCapacity(t, 128);
    return t;
}

void ConnectionTracker_Destroy(ConnectionTracker *t)
{
    if (!t) return;
    for (int i = 0; i < t->count; i++) ConnectionArch_Destroy(t->items[i]);
    free(t->items);
    free(t->mapHash);
    free(t->mapSlot);
    free(t);
}

void ConnectionTracker_SetExpiry(ConnectionTracker *t, double seconds)
{
    if (t && seconds > 0.0) t->expiry = seconds;
}

double ConnectionTracker_GetExpiry(const ConnectionTracker *t)
{
    return t ? t->expiry : 0.0;
}

int ConnectionTracker_Count(const ConnectionTracker *t)
{
    return t ? t->count : 0;
}

int ConnectionTracker_GetAll(const ConnectionTracker *t, ConnectionArch **out, int max)
{
    if (!t || !out || max <= 0) return 0;
    int n = t->count < max ? t->count : max;
    for (int i = 0; i < n; i++) out[i] = t->items[i];
    return n;
}

ConnectionArch *ConnectionTracker_FindById(const ConnectionTracker *t, uint64_t id)
{
    if (!t) return NULL;
    for (int i = 0; i < t->count; i++) {
        if (t->items[i]->id == id) return t->items[i];
    }
    return NULL;
}

void ConnectionTracker_Clear(ConnectionTracker *t)
{
    if (!t) return;
    for (int i = 0; i < t->count; i++) ConnectionArch_Destroy(t->items[i]);
    t->count = 0;
    RebuildMap(t);
}

/* --------------------------------------------------------------- update --- */

void ConnectionTracker_Update(ConnectionTracker *t, const NetSnapshot *snap,
                              ConnectionArch **outAdded, int *addedCount,
                              ConnectionArch **outRemoved, int *removedCount,
                              int maxResults)
{
    if (addedCount) *addedCount = 0;
    if (removedCount) *removedCount = 0;
    if (!t || !snap) return;

    if (!EnsureMapCapacity(t, snap->connectionCount + t->count + 16)) return;

    bool mapStale = false;
    double now = snap->timestamp;

    /* 1. update existing arches, create arches for newly seen connections */
    for (int i = 0; i < snap->connectionCount; i++) {
        const NetConnection *conn = &snap->connections[i];
        int idx = MapFind(t, &conn->key);
        if (idx >= 0) {
            ConnectionArch_Update(t->items[idx], conn, now);
            continue;
        }

        ConnectionArch *arch = ConnectionArch_Create(conn, now);
        if (!arch) continue;

        if (t->count == t->cap) {
            int ncap = t->cap * 2;
            ConnectionArch **ni = (ConnectionArch **)realloc(t->items, sizeof(ConnectionArch *) * ncap);
            if (!ni) { ConnectionArch_Destroy(arch); continue; }
            t->items = ni;
            t->cap = ncap;
        }
        t->items[t->count++] = arch;
        mapStale = true;

        if (outAdded && addedCount && *addedCount < maxResults) {
            outAdded[*addedCount] = arch;
            (*addedCount)++;
        }
    }

    /* 2. mark arches the OS stopped reporting inactive, expire them on age */
    for (int i = 0; i < t->count; ) {
        ConnectionArch *arch = t->items[i];
        if (arch->lastActivity >= now) { i++; continue; }   /* seen this poll */
        arch->active = false;
        if (now - arch->lastActivity <= t->expiry) { i++; continue; }

        if (outRemoved && removedCount && *removedCount < maxResults) {
            outRemoved[*removedCount] = arch;
            (*removedCount)++;
        }
        ConnectionArch_Destroy(arch);
        t->items[i] = t->items[t->count - 1];
        t->count--;
        mapStale = true;
    }

    if (mapStale) RebuildMap(t);
}
