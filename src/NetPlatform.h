/*
 * NetPlatform.h - Minimal threading / synchronisation / clock abstraction.
 *
 * Windows implementation (Win32 API). Keeps every other module free of
 * <windows.h> so the rest of the code stays portable C.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide initialisation (Winsock). Reference counted, thread safe.
 * Must be called before any module that formats or parses IP addresses.      */
void NetPlatform_Init(void);
void NetPlatform_Shutdown(void);

/* Monotonic clock in seconds. Never jumps with wall-clock changes.           */
double NetTime_Now(void);

void    NetThread_SleepMs(uint32_t ms);

typedef struct NetThread NetThread;
typedef void (*NetThreadFn)(void *userData);

NetThread *NetThread_Start(NetThreadFn fn, void *userData);
/* Blocks until the thread function returned and frees the handle.            */
void       NetThread_Join(NetThread *t);

typedef struct NetMutex NetMutex;
NetMutex *NetMutex_Create(void);
void      NetMutex_Destroy(NetMutex *m);
void      NetMutex_Lock(NetMutex *m);
void      NetMutex_Unlock(NetMutex *m);

typedef struct NetCond NetCond;
NetCond *NetCond_Create(void);
void     NetCond_Destroy(NetCond *c);
/* Returns true when signalled, false on timeout.                             */
bool     NetCond_Wait(NetCond *c, NetMutex *m, uint32_t timeoutMs);
void     NetCond_Signal(NetCond *c);
void     NetCond_Broadcast(NetCond *c);

#ifdef __cplusplus
}
#endif
