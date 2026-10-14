/*
 * ConnectionTracker.h - Maintains the set of ConnectionArch objects.
 *
 * Consumes NetSnapshot values from the NetworkMonitor and creates/updates/
 * expires ConnectionArch objects. A connection key (protocol + local/remote
 * address + ports) identifies a connection - never the remote address alone.
 *
 * Update() reports the arches that were created and removed so the caller can
 * queue geolocation lookups and release UI references. The tracker owns every
 * ConnectionArch and destroys it; the caller only borrows pointers.
 */
#pragma once

#include "NetTypes.h"
#include "ConnectionArch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ConnectionTracker ConnectionTracker;

/* expirySeconds: how long an arch survives after the OS stopped reporting it */
ConnectionTracker *ConnectionTracker_Create(double expirySeconds);
void               ConnectionTracker_Destroy(ConnectionTracker *t);

/* Reconciles the tracked arches with a fresh snapshot.
 * outAdded/outRemoved are caller provided arrays (capacity maxResults).       */
void ConnectionTracker_Update(ConnectionTracker *t, const NetSnapshot *snap,
                              ConnectionArch **outAdded, int *addedCount,
                              ConnectionArch **outRemoved, int *removedCount,
                              int maxResults);

int  ConnectionTracker_Count(const ConnectionTracker *t);
/* Copies up to max pointers of the currently tracked arches. Returns count.   */
int  ConnectionTracker_GetAll(const ConnectionTracker *t, ConnectionArch **out, int max);

/* Destroys every tracked arch. Used when the monitored adapter (and thereby
 * the whole observed connection set) is switched, so the old adapter's arcs
 * never linger.                                                               */
void ConnectionTracker_Clear(ConnectionTracker *t);

void ConnectionTracker_SetExpiry(ConnectionTracker *t, double seconds);
double ConnectionTracker_GetExpiry(const ConnectionTracker *t);

/* Finds an arch by its unique id; NULL when it no longer exists.              */
ConnectionArch *ConnectionTracker_FindById(const ConnectionTracker *t, uint64_t id);

#ifdef __cplusplus
}
#endif
