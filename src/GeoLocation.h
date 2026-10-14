/*
 * GeoLocation.h - Asynchronous IP geolocation and reverse DNS resolution.
 *
 * A dedicated worker thread performs every blocking operation (DNS queries and
 * HTTPS requests), so the Raylib render thread only ever takes a mutex for a
 * short memcpy. Results are cached per IP address; failed lookups are cached
 * too (with a retry cooldown) so a flaky network never floods the provider.
 *
 * Privacy / policy:
 *   - Only globally routable unicast addresses are sent to the provider.
 *     Private, loopback, link-local, CGNAT, multicast and reserved addresses
 *     are classified locally and never leave the machine.
 *   - Requests are rate limited (one HTTP request per GEO_MIN_REQUEST_GAP
 *     seconds by default) and every successful result is cached in memory.
 *   - The default provider is https://ipwho.is (no API key required). Any
 *     compatible JSON endpoint can be configured with GeoLocation_SetProvider.
 */
#pragma once

#include "NetTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeoLocation GeoLocation;

GeoLocation *GeoLocation_Create(void);
void         GeoLocation_Destroy(GeoLocation *g);

void         GeoLocation_Start(GeoLocation *g);
void         GeoLocation_Stop(GeoLocation *g);

/* Queues a lookup. Safe to call from any thread; duplicates are ignored and
 * non-public addresses are resolved synchronously (no I/O).                   */
void         GeoLocation_RequestLookup(GeoLocation *g, const char *ip);

/* Reads the cached result for an IP without blocking. Returns false when the
 * address has never been looked up. hostnameOut may be NULL.                  */
bool         GeoLocation_TryGet(GeoLocation *g, const char *ip, GeoInfo *out,
                                char *hostnameOut, size_t hostnameCap);

/* Pops one completed lookup (geo + reverse DNS) from the result queue.        */
bool         GeoLocation_PollResult(GeoLocation *g, char *ipOut, size_t ipCap,
                                    GeoInfo *out, char *hostnameOut, size_t hostnameCap);

/* The machine's own public address and its geolocation (from the provider's
 * "self" lookup). Used to anchor outbound arcs at our own location.           */
bool         GeoLocation_GetSelf(GeoLocation *g, GeoInfo *out, char *ipOut, size_t ipCap);

/* Enables/disables network lookups (offline mode). Local classification keeps
 * working; the queue is simply not drained while disabled.                    */
void         GeoLocation_SetEnabled(GeoLocation *g, bool enabled);
bool         GeoLocation_GetEnabled(GeoLocation *g);

int          GeoLocation_PendingCount(GeoLocation *g);

/* Provider base URL, e.g. "https://ipwho.is" (default) or an internal mirror. */
void         GeoLocation_SetProvider(GeoLocation *g, const char *baseUrl);
const char  *GeoLocation_GetProvider(const GeoLocation *g);

/* True after the self lookup succeeded (we know our public address/position). */
bool         GeoLocation_HasSelf(const GeoLocation *g);

#ifdef __cplusplus
}
#endif
