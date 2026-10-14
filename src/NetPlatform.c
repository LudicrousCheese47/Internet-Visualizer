/* NetPlatform.c - Win32 implementation of the threading/clock helpers. */
#include "NetPlatform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>

/* --------------------------------------------------------------- winsock -- */

static volatile LONG g_wsRefCount = 0;

void NetPlatform_Init(void)
{
    if (InterlockedIncrement(&g_wsRefCount) == 1) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            InterlockedDecrement(&g_wsRefCount);
        }
    }
}

void NetPlatform_Shutdown(void)
{
    if (InterlockedDecrement(&g_wsRefCount) == 0) {
        WSACleanup();
    }
}

/* ------------------------------------------------------------------ clock -- */

double NetTime_Now(void)
{
    return (double)GetTickCount64() / 1000.0;
}

void NetThread_SleepMs(uint32_t ms)
{
    Sleep(ms);
}

/* ---------------------------------------------------------------- threads -- */

struct NetThread {
    HANDLE handle;
};

static DWORD WINAPI ThreadTrampoline(LPVOID param)
{
    NetThreadFn fn = (NetThreadFn)((void **)param)[0];
    void *ud = ((void **)param)[1];
    free(param);
    fn(ud);
    return 0;
}

NetThread *NetThread_Start(NetThreadFn fn, void *userData)
{
    void **param = (void **)malloc(sizeof(void *) * 2);
    if (!param) return NULL;
    param[0] = (void *)(uintptr_t)fn;
    param[1] = userData;

    NetThread *t = (NetThread *)malloc(sizeof(NetThread));
    if (!t) { free(param); return NULL; }

    t->handle = CreateThread(NULL, 0, ThreadTrampoline, param, 0, NULL);
    if (!t->handle) { free(param); free(t); return NULL; }
    return t;
}

void NetThread_Join(NetThread *t)
{
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

/* ------------------------------------------------------------ sync objects -- */

struct NetMutex {
    CRITICAL_SECTION cs;
};

NetMutex *NetMutex_Create(void)
{
    NetMutex *m = (NetMutex *)malloc(sizeof(NetMutex));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void NetMutex_Destroy(NetMutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void NetMutex_Lock(NetMutex *m)   { EnterCriticalSection(&m->cs); }
void NetMutex_Unlock(NetMutex *m) { LeaveCriticalSection(&m->cs); }

struct NetCond {
    CONDITION_VARIABLE cv;
};

NetCond *NetCond_Create(void)
{
    NetCond *c = (NetCond *)malloc(sizeof(NetCond));
    if (!c) return NULL;
    InitializeConditionVariable(&c->cv);
    return c;
}

void NetCond_Destroy(NetCond *c)
{
    free(c); /* CONDITION_VARIABLE needs no destruction */
}

bool NetCond_Wait(NetCond *c, NetMutex *m, uint32_t timeoutMs)
{
    return SleepConditionVariableCS(&c->cv, &m->cs, timeoutMs) != 0;
}

void NetCond_Signal(NetCond *c)   { WakeConditionVariable(&c->cv); }
void NetCond_Broadcast(NetCond *c){ WakeAllConditionVariable(&c->cv); }
