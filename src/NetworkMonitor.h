/*
 * NetworkMonitor.h - Primary network interface detection and connection table
 * enumeration.
 *
 * A background worker thread polls the operating system (Windows IP Helper
 * API) and publishes immutable NetSnapshot copies guarded by a mutex. The
 * render thread never touches OS calls: it only copies the latest snapshot.
 */
#pragma once

#include "NetTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NetworkMonitor NetworkMonitor;

NetworkMonitor *NetworkMonitor_Create(void);
void            NetworkMonitor_Destroy(NetworkMonitor *m);

/* Starts/stops the polling worker thread.                                    */
void            NetworkMonitor_Start(NetworkMonitor *m);
void            NetworkMonitor_Stop(NetworkMonitor *m);

/* Copies the most recent snapshot. Returns false before the first poll.      */
bool            NetworkMonitor_GetSnapshot(NetworkMonitor *m, NetSnapshot *out);

/* How often the OS is polled (seconds). Clamped to [0.25, 10].                */
void            NetworkMonitor_SetPollInterval(NetworkMonitor *m, double seconds);
double          NetworkMonitor_GetPollInterval(const NetworkMonitor *m);

/* Runs one poll synchronously (used by tests / the first frame so the UI has
 * data immediately). Returns false on failure; the status field of the
 * snapshot then carries a human readable error.                               */
bool            NetworkMonitor_PollOnce(NetworkMonitor *m);

/* Selects which adapter is monitored by stable id (NetInterfaceInfo.id).
 * An empty/NULL id resets to "all adapters" (follow the primary interface).  */
void            NetworkMonitor_SetSelectedInterface(NetworkMonitor *m, const char *id);
void            NetworkMonitor_GetSelectedInterface(const NetworkMonitor *m,
                                                    char *out, size_t cap);

#ifdef __cplusplus
}
#endif
