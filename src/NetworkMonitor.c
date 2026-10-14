/*
 * NetworkMonitor.c - Windows implementation.
 *
 * Native IP Helper API usage (no shell commands):
 *   GetAdaptersAddresses   - adapters, names, unicast addresses, gateways, DNS
 *   GetIpForwardTable2     - default route -> primary interface
 *   GetIfTable2            - interface traffic counters (In/Out octets)
 *   GetExtendedTcpTable    - TCP endpoints incl. state and owning PID
 *   GetExtendedUdpTable    - UDP endpoints (local bind only on Windows)
 *   OpenProcess/QueryFullProcessImageNameW - process names for PIDs
 *
 * The worker thread owns every OS handle; the render thread only copies the
 * published snapshot under a mutex.
 */
#include "NetworkMonitor.h"
#include "NetPlatform.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PID_CACHE_MAX        128
#define LISTEN_PORT_MAX      512
#define DEFAULT_POLL_SEC     1.0

/* MinGW-w64 headers name these differently from the MSVC Windows SDK. */
#ifdef MIB_TCP_STATE_ESTAB
#define TCP_STATE_CLOSED      MIB_TCP_STATE_CLOSED
#define TCP_STATE_LISTEN      MIB_TCP_STATE_LISTEN
#define TCP_STATE_SYN_SENT    MIB_TCP_STATE_SYN_SENT
#define TCP_STATE_SYN_RCVD    MIB_TCP_STATE_SYN_RCVD
#define TCP_STATE_ESTAB       MIB_TCP_STATE_ESTAB
#define TCP_STATE_FIN_WAIT1   MIB_TCP_STATE_FIN_WAIT1
#define TCP_STATE_FIN_WAIT2   MIB_TCP_STATE_FIN_WAIT2
#define TCP_STATE_CLOSE_WAIT  MIB_TCP_STATE_CLOSE_WAIT
#define TCP_STATE_CLOSING     MIB_TCP_STATE_CLOSING
#define TCP_STATE_LAST_ACK    MIB_TCP_STATE_LAST_ACK
#define TCP_STATE_TIME_WAIT   MIB_TCP_STATE_TIME_WAIT
#define TCP_STATE_DELETE_TCB  MIB_TCP_STATE_DELETE_TCB
#endif
#ifndef MIB_TCP_TABLE_OWNER_PID_ALL   /* MinGW: MIB_TCPTABLE_OWNER_PID       */
#define MIB_TCP_TABLE_OWNER_PID_ALL   MIB_TCPTABLE_OWNER_PID
#endif
#ifndef MIB_TCP6_TABLE_OWNER_PID_ALL  /* MinGW: MIB_TCP6TABLE_OWNER_PID      */
#define MIB_TCP6_TABLE_OWNER_PID_ALL  MIB_TCP6TABLE_OWNER_PID
#endif
#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD  6
#endif
#ifndef IF_TYPE_WWAN
#define IF_TYPE_WWAN             243
#endif

/* --------------------------------------------------------------- helpers -- */

static void WideToUtf8(const WCHAR *w, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = '\0';
}

static const char *IfTypeName(int ifType)
{
    switch (ifType) {
        case IF_TYPE_ETHERNET_CSMACD: return "Ethernet";
        case IF_TYPE_IEEE80211:         return "Wi-Fi";
        case IF_TYPE_SOFTWARE_LOOPBACK: return "Loopback";
        case IF_TYPE_PPP:               return "PPP";
        case IF_TYPE_TUNNEL:            return "Tunnel/VPN";
        case IF_TYPE_IEEE1394:          return "FireWire";
        case IF_TYPE_WWAN:              return "Cellular";
        case IF_TYPE_PROP_VIRTUAL:      return "Virtual";
        default:                        return "Other";
    }
}

static void CopyAddrFromSockaddr(const struct sockaddr *sa, NetAddr *out)
{
    NetAddr_Clear(out);
    if (!sa) return;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
        out->kind = NET_ADDR_IPV4;
        memcpy(out->bytes, &v4->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;
        out->kind = NET_ADDR_IPV6;
        memcpy(out->bytes, &v6->sin6_addr, 16);
        out->scopeId = v6->sin6_scope_id;
    }
}

static void CopyAddrFromUlong(DWORD addr, NetAddr *out)
{
    NetAddr_Clear(out);
    if (addr == 0) return;
    out->kind = NET_ADDR_IPV4;
    out->bytes[0] = (uint8_t)(addr & 0xFF);
    out->bytes[1] = (uint8_t)((addr >> 8) & 0xFF);
    out->bytes[2] = (uint8_t)((addr >> 16) & 0xFF);
    out->bytes[3] = (uint8_t)((addr >> 24) & 0xFF);
}

/* --------------------------------------------------- process name cache --- */

typedef struct PidNameEntry {
    uint32_t pid;
    uint64_t processStartTime;
    char     name[NET_STR_SMALL];
    double   lastUsed;
    double   lastIdentityCheck;
} PidNameEntry;

struct NetworkMonitor {
    NetMutex  *lock;          /* guards snapshot / hasSnapshot / requestedId  */
    NetThread *thread;        /* polling worker (NULL when stopped)           */
    volatile LONG running;    /* 1 while the worker should keep looping       */

    NetSnapshot snapshot;     /* last published snapshot (guarded by lock)    */
    bool        hasSnapshot;

    double   pollInterval;    /* seconds between OS polls                     */

    /* user-selected adapter, by stable id (guarded by lock). Empty string
     * means "all adapters" (the default: follow the primary interface).       */
    char     requestedId[NET_STR_NAME];

    /* interface rate estimation (worker thread only)                         */
    uint64_t prevIn, prevOut;
    double   prevRateTime;
    bool     havePrevRates;
    char     prevRateId[NET_STR_NAME];   /* adapter the counters above belong to */

    /* process name cache (worker thread only)                                */
    PidNameEntry pidCache[PID_CACHE_MAX];
    int          pidCacheCount;
};

static uint64_t FileTimeValue(const FILETIME *time)
{
    ULARGE_INTEGER value;
    value.LowPart = time->dwLowDateTime;
    value.HighPart = time->dwHighDateTime;
    return value.QuadPart;
}

static void ResolvePidName(NetworkMonitor *m, uint32_t pid, char *out, size_t cap,
                           uint64_t *processStartTime)
{
    if (processStartTime) *processStartTime = 0;
    snprintf(out, cap, "pid:%u", pid);
    if (pid == 0) { snprintf(out, cap, "Idle"); return; }
    if (pid == 4) { snprintf(out, cap, "System"); return; }

    double now = NetTime_Now();
    PidNameEntry *cached = NULL;
    for (int i = 0; i < m->pidCacheCount; i++) {
        if (m->pidCache[i].pid == pid) {
            cached = &m->pidCache[i];
            if (now - cached->lastIdentityCheck < 1.0) {
                snprintf(out, cap, "%s", cached->name);
                if (processStartTime) *processStartTime = cached->processStartTime;
                cached->lastUsed = now;
                return;
            }
            break;
        }
    }

    uint64_t created = cached ? cached->processStartTime : 0;
    char resolvedName[NET_STR_SMALL];
    snprintf(resolvedName, sizeof(resolvedName), "pid:%u", pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        FILETIME createTime, exitTime, kernelTime, userTime;
        if (GetProcessTimes(h, &createTime, &exitTime, &kernelTime, &userTime)) {
            uint64_t observedStart = FileTimeValue(&createTime);
            created = observedStart;
        }
        WCHAR path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const WCHAR *base = path;
            for (DWORD i = 0; i < n; i++) {
                if (path[i] == L'\\' || path[i] == L'/') base = &path[i + 1];
            }
            char utf8[NET_STR_SMALL];
            WideToUtf8(base, utf8, sizeof(utf8));
            /* strip ".exe" for display */
            size_t len = strlen(utf8);
            if (len > 4 && _stricmp(&utf8[len - 4], ".exe") == 0) utf8[len - 4] = '\0';
            snprintf(resolvedName, sizeof(resolvedName), "%s", utf8);
        }
        CloseHandle(h);
    }

    if (!cached) {
        /* Store or replace a PID entry when process creation time changes. */
        if (m->pidCacheCount < PID_CACHE_MAX) {
            cached = &m->pidCache[m->pidCacheCount++];
        } else {
            cached = &m->pidCache[0];
            for (int i = 1; i < m->pidCacheCount; i++) {
                if (m->pidCache[i].lastUsed < cached->lastUsed) cached = &m->pidCache[i];
            }
        }
    }
    cached->pid = pid;
    cached->processStartTime = created;
    cached->lastUsed = now;
    cached->lastIdentityCheck = now;
    snprintf(cached->name, sizeof(cached->name), "%s", resolvedName);
    snprintf(out, cap, "%s", resolvedName);
    if (processStartTime) *processStartTime = created;
}

static bool ReadTcpCounters4(const MIB_TCPROW *row, NetConnection *conn)
{
    TCP_ESTATS_DATA_RW_v0 rw = {TRUE};
    ULONG enabled = SetPerTcpConnectionEStats((PMIB_TCPROW)row, TcpConnectionEstatsData,
                                               (PUCHAR)&rw, 0, sizeof(rw), 0);
    if (enabled != NO_ERROR) return false;

    TCP_ESTATS_DATA_ROD_v0 counters;
    memset(&counters, 0, sizeof(counters));
    if (GetPerTcpConnectionEStats((PMIB_TCPROW)row, TcpConnectionEstatsData,
                                  NULL, 0, 0, NULL, 0, 0,
                                  (PUCHAR)&counters, 0, sizeof(counters)) != NO_ERROR) {
        return false;
    }
    conn->bytesReceived = counters.DataBytesIn;
    conn->bytesSent = counters.DataBytesOut;
    conn->countersValid = true;
    return true;
}

static bool ReadTcpCounters6(const MIB_TCP6ROW *row, NetConnection *conn)
{
    TCP_ESTATS_DATA_RW_v0 rw = {TRUE};
    ULONG enabled = SetPerTcp6ConnectionEStats((PMIB_TCP6ROW)row, TcpConnectionEstatsData,
                                                (PUCHAR)&rw, 0, sizeof(rw), 0);
    if (enabled != NO_ERROR) return false;

    TCP_ESTATS_DATA_ROD_v0 counters;
    memset(&counters, 0, sizeof(counters));
    if (GetPerTcp6ConnectionEStats((PMIB_TCP6ROW)row, TcpConnectionEstatsData,
                                   NULL, 0, 0, NULL, 0, 0,
                                   (PUCHAR)&counters, 0, sizeof(counters)) != NO_ERROR) {
        return false;
    }
    conn->bytesReceived = counters.DataBytesIn;
    conn->bytesSent = counters.DataBytesOut;
    conn->countersValid = true;
    return true;
}

/* -------------------------------------------------------- interfaces ----- */

static uint32_t FindPrimaryInterfaceIndex(char *why, size_t whyCap)
{
    /* Primary interface = owner of the lowest metric default route.          */
    MIB_IPFORWARD_TABLE2 *routes = NULL;
    DWORD err = GetIpForwardTable2(AF_UNSPEC, &routes);
    uint32_t best = 0;
    uint64_t bestMetric = UINT64_MAX;
    bool found = false;

    if (err == NO_ERROR && routes) {
        for (DWORD i = 0; i < routes->NumEntries; i++) {
            const MIB_IPFORWARD_ROW2 *row = &routes->Table[i];
            if (row->DestinationPrefix.PrefixLength != 0) continue;   /* not default route */
            if (row->InterfaceLuid.Value == 0) continue;

            MIB_IPINTERFACE_ROW ifrow;
            InitializeIpInterfaceEntry(&ifrow);
            ifrow.Family = row->DestinationPrefix.Prefix.si_family;
            ifrow.InterfaceLuid = row->InterfaceLuid;
            uint64_t metric = row->Metric;
            if (GetIpInterfaceEntry(&ifrow) == NO_ERROR) {
#ifdef __MINGW32__             /* MSVC calls this member InterfaceMetric */
                metric += ifrow.Metric;
#else
                metric += ifrow.InterfaceMetric;
#endif
            }
            if (metric < bestMetric) {
                bestMetric = metric;
                best = row->InterfaceIndex;
                found = true;
            }
        }
        FreeMibTable(routes);
    }

    if (found) {
        snprintf(why, whyCap, "default route metric %llu", (unsigned long long)bestMetric);
        return best;
  }

    snprintf(why, whyCap, "no default route found");
    return 0;
}

static void FillAdapterInfo(const IP_ADAPTER_ADDRESSES *a, NetInterfaceInfo *info)
{
    info->index = a->IfIndex;
    info->luid = a->Luid.Value;
    /* AdapterName is the system-assigned GUID ("{...}") - it survives
     * disconnect/reconnect and renames, which is what we need for selection.  */
    if (a->AdapterName && a->AdapterName[0]) {
        snprintf(info->id, sizeof(info->id), "%s", a->AdapterName);
    } else {
        snprintf(info->id, sizeof(info->id), "luid:%llu", (unsigned long long)info->luid);
    }
    WideToUtf8(a->FriendlyName, info->name, sizeof(info->name));
    WideToUtf8(a->Description, info->description, sizeof(info->description));
    if (info->name[0] == '\0') snprintf(info->name, sizeof(info->name), "%s", info->description);
    info->ifType = (int)a->IfType;
    snprintf(info->typeName, sizeof(info->typeName), "%s", IfTypeName(info->ifType));
    info->isUp = (a->OperStatus == IfOperStatusUp);
    info->isLoopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
    info->isTunnel = (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP ||
                      strstr(info->description, "VPN") != NULL ||
                      strstr(info->name, "VPN") != NULL);

    for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
        NetAddr addr;
        CopyAddrFromSockaddr(u->Address.lpSockaddr, &addr);
        int prefix = (int)u->OnLinkPrefixLength;
        if (addr.kind == NET_ADDR_IPV4 && NetAddr_IsZero(&info->ipv4)) {
            info->ipv4 = addr;
            info->prefix4 = prefix;
        } else if (addr.kind == NET_ADDR_IPV6 && NetAddr_IsZero(&info->ipv6) &&
                   NetAddr_Classify(&addr) != NET_ADDR_CLASS_LINK_LOCAL) {
            info->ipv6 = addr;
            info->prefix6 = prefix;
        }
    }

    for (const IP_ADAPTER_GATEWAY_ADDRESS_LH *g = a->FirstGatewayAddress; g; g = g->Next) {
        NetAddr addr;
        CopyAddrFromSockaddr(g->Address.lpSockaddr, &addr);
        if (addr.kind == NET_ADDR_IPV4 && NetAddr_IsZero(&info->gateway4)) info->gateway4 = addr;
        if (addr.kind == NET_ADDR_IPV6 && NetAddr_IsZero(&info->gateway6)) info->gateway6 = addr;
    }

    for (const IP_ADAPTER_DNS_SERVER_ADDRESS *d = a->FirstDnsServerAddress;
         d && info->dnsCount < NET_MAX_DNS; d = d->Next) {
        NetAddr addr;
        CopyAddrFromSockaddr(d->Address.lpSockaddr, &addr);
        if (NetAddr_IsZero(&addr)) continue;
        if (NetAddr_Classify(&addr) == NET_ADDR_CLASS_LOOPBACK && info->dnsCount > 0) continue;
        NetAddr_ToString(&addr, info->dnsServers[info->dnsCount], NET_STR_IP);
        info->dnsCount++;
    }
}

static void EnumerateInterfaces(NetworkMonitor *m, NetSnapshot *snap)
{
    char why[128] = "unknown";
    uint32_t primaryIndex = FindPrimaryInterfaceIndex(why, sizeof(why));

    /* read the user's adapter choice (written by the UI thread) */
    char requestedId[NET_STR_NAME] = "";
    NetMutex_Lock(m->lock);
    snprintf(requestedId, sizeof(requestedId), "%s", m->requestedId);
    NetMutex_Unlock(m->lock);
    const bool wantSpecific = (requestedId[0] != '\0');

    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size);
    if (size == 0) {
        snprintf(snap->status, sizeof(snap->status), "no network adapters reported by Windows");
        snap->ok = false;
        return;
    }
    IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!addrs) {
        snprintf(snap->status, sizeof(snap->status), "out of memory enumerating adapters");
        snap->ok = false;
        return;
    }
    DWORD err = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, addrs, &size);
    if (err != NO_ERROR) {
        snprintf(snap->status, sizeof(snap->status), "GetAdaptersAddresses failed (error %lu)", err);
        snap->ok = false;
        free(addrs);
        return;
    }

    /* traffic counters for every interface, one call */
    MIB_IF_TABLE2 *ifTable = NULL;
    bool haveIfTable = (GetIfTable2(&ifTable) == NO_ERROR);

    bool sawPrimary = false;
    int  selectedIdx = -1;
    for (const IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
        if (snap->interfaceCount >= NET_MAX_IFACES) break;
        /* software loopback has no peer traffic of its own; skip it so the
         * adapter picker lists only interfaces that can be monitored          */
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        NetInterfaceInfo *info = &snap->interfaces[snap->interfaceCount];
        memset(info, 0, sizeof(*info));
        info->rateIn = -1;
        info->rateOut = -1;
        FillAdapterInfo(a, info);
        info->isPrimary = (a->IfIndex == primaryIndex);
        if (info->isPrimary) {
            sawPrimary = true;
            snap->primaryInterface = snap->interfaceCount;
        }
        if (wantSpecific && strcmp(info->id, requestedId) == 0) {
            info->isSelected = true;
            selectedIdx = snap->interfaceCount;
        }

        if (haveIfTable) {
            for (DWORD i = 0; i < ifTable->NumEntries; i++) {
                const MIB_IF_ROW2 *row = &ifTable->Table[i];
                if (row->InterfaceIndex != info->index) continue;
                info->bytesReceived = row->InOctets;
                info->bytesSent = row->OutOctets;
                info->countersValid = true;
                if (!info->isUp) info->isUp = (row->OperStatus == IfOperStatusUp);
                break;
            }
        }
        snap->interfaceCount++;
    }
    if (ifTable) FreeMibTable(ifTable);
    free(addrs);

    /* fallback: no default route matched any adapter */
    if (!sawPrimary && snap->interfaceCount > 0) {
        int pick = -1;
        for (int i = 0; i < snap->interfaceCount; i++) {
            NetInterfaceInfo *it = &snap->interfaces[i];
            if (!it->isUp || it->isLoopback) continue;
            if (!NetAddr_IsZero(&it->gateway4) || !NetAddr_IsZero(&it->gateway6)) { pick = i; break; }
            if (pick < 0) pick = i;
        }
        if (pick < 0) pick = 0;
        snap->interfaces[pick].isPrimary = true;
        snap->primaryInterface = pick;
        snprintf(why, sizeof(why), "fallback selection");
    }

    /* resolve which adapter is being monitored */
    if (wantSpecific && selectedIdx >= 0) {
        snap->interfaces[selectedIdx].isSelected = true;
        snap->selectedInterface = selectedIdx;
        snap->selectedIsSpecific = true;
    } else {
        /* "all adapters" mode, or the requested adapter disappeared: fall back
         * to the primary. The request is kept so a re-plug re-selects it.      */
        snap->selectedInterface = snap->primaryInterface;
        snap->selectedIsSpecific = false;
    }

    /* interface rates for the MONITORED adapter (never the primary when the
     * user picked a different one). Reset the baseline when the monitored
     * adapter identity changes so one adapter's counters never leak onto
     * another's rate figures.                                                  */
    const char *rateId = wantSpecific ? requestedId : "__all__";
    if (strcmp(rateId, m->prevRateId) != 0) {
        memset(m->prevRateId, 0, sizeof(m->prevRateId));
        m->havePrevRates = false;
    }
    if (snap->selectedInterface >= 0) {
        NetInterfaceInfo *p = &snap->interfaces[snap->selectedInterface];
        double now = snap->timestamp;
        if (m->havePrevRates && p->countersValid && now > m->prevRateTime) {
            double dt = now - m->prevRateTime;
            if (p->bytesReceived >= m->prevIn && p->bytesSent >= m->prevOut) {
                p->rateIn = (int64_t)((double)(p->bytesReceived - m->prevIn) / dt);
                p->rateOut = (int64_t)((double)(p->bytesSent - m->prevOut) / dt);
            }
        }
        if (p->countersValid) {
            m->prevIn = p->bytesReceived;
            m->prevOut = p->bytesSent;
            m->prevRateTime = now;
            m->havePrevRates = true;
        }
    }
    snprintf(m->prevRateId, sizeof(m->prevRateId), "%s", rateId);

    snap->ok = true;
    if (snap->selectedInterface >= 0) {
        const NetInterfaceInfo *p = &snap->interfaces[snap->selectedInterface];
        char ipStr[NET_STR_IP];
        NetAddr_ToString(&p->ipv4, ipStr, sizeof(ipStr));
        if (snap->selectedIsSpecific) {
            snprintf(snap->status, sizeof(snap->status),
                     "monitoring: %s (%s), %s, %s", p->name, p->typeName, ipStr, why);
        } else {
            snprintf(snap->status, sizeof(snap->status),
                     "monitoring all adapters (primary: %s), %s, %s", p->name, ipStr, why);
        }
    } else {
        snprintf(snap->status, sizeof(snap->status), "no active network interface detected");
    }
    if (wantSpecific && selectedIdx < 0) {
        char note[96];
        snprintf(note, sizeof(note), "; '%s' not present - monitoring primary",
                 snap->interfaceCount > 0 ? requestedId : requestedId);
        strncat(snap->status, note, sizeof(snap->status) - strlen(snap->status) - 1);
    }
}

/* -------------------------------------------------------- connections ----- */

/* True when the LOCAL address of a connection belongs to the given adapter.
 * Windows never reports the interface behind a connection directly, so this
 * local-address match (socket bound to the adapter's unicast address) is the
 * only honest attribution the OS data supports.                               */
static bool LocalAddrBelongsTo(const NetAddr *addr, const NetInterfaceInfo *info)
{
    if (!addr || !info) return false;
    if (addr->kind == NET_ADDR_IPV4 && info->ipv4.kind == NET_ADDR_IPV4)
        return memcmp(addr->bytes, info->ipv4.bytes, 4) == 0;
    if (addr->kind == NET_ADDR_IPV6 && info->ipv6.kind == NET_ADDR_IPV6)
        return memcmp(addr->bytes, info->ipv6.bytes, 16) == 0;
    return false;
}

typedef struct ConnBuilder {
    NetworkMonitor *m;
    NetConnection  *out;
    int             cap;
    int             count;
    bool            truncated;
    uint16_t        listenPorts[LISTEN_PORT_MAX];
    int             listenPortCount;
    int             tcpCount;
    int             udpCount;
} ConnBuilder;

static bool IsListeningPort(const ConnBuilder *b, uint16_t port)
{
    if (port == 0) return false;
    for (int i = 0; i < b->listenPortCount; i++) {
        if (b->listenPorts[i] == port) return true;
    }
    return false;
}

static void AddListenPort(ConnBuilder *b, uint16_t port)
{
    if (port == 0 || b->listenPortCount >= LISTEN_PORT_MAX) return;
    if (IsListeningPort(b, port)) return;
    b->listenPorts[b->listenPortCount++] = port;
}

static NetConnState MapTcpState(DWORD s)
{
    switch (s) {
        case TCP_STATE_CLOSED:       return NET_CONN_CLOSED;
        case TCP_STATE_LISTEN:       return NET_CONN_LISTEN;
        case TCP_STATE_SYN_SENT:     return NET_CONN_SYN_SENT;
        case TCP_STATE_SYN_RCVD:     return NET_CONN_SYN_RECEIVED;
        case TCP_STATE_ESTAB:        return NET_CONN_ESTABLISHED;
        case TCP_STATE_FIN_WAIT1:    return NET_CONN_FIN_WAIT1;
        case TCP_STATE_FIN_WAIT2:    return NET_CONN_FIN_WAIT2;
        case TCP_STATE_CLOSE_WAIT:   return NET_CONN_CLOSE_WAIT;
        case TCP_STATE_CLOSING:      return NET_CONN_CLOSING;
        case TCP_STATE_LAST_ACK:     return NET_CONN_LAST_ACK;
        case TCP_STATE_TIME_WAIT:    return NET_CONN_TIME_WAIT;
        case TCP_STATE_DELETE_TCB:   return NET_CONN_CLOSED;
        default:                     return NET_CONN_UNKNOWN;
    }
}

/* Direction inference from TCP state plus the local-port/listener relation.
 * Only marked when the OS data actually supports the conclusion.              */
static NetDirection InferDirection(NetConnState state, uint16_t localPort, bool localIsListening)
{
    switch (state) {
        case NET_CONN_LISTEN:       return NET_DIR_LISTENER;
        case NET_CONN_SYN_SENT:
        case NET_CONN_FIN_WAIT1:
        case NET_CONN_FIN_WAIT2:
        case NET_CONN_TIME_WAIT:    return NET_DIR_OUTBOUND;   /* we opened it */
        case NET_CONN_SYN_RECEIVED:
        case NET_CONN_LAST_ACK:     return NET_DIR_INBOUND;    /* we were the listener */
        case NET_CONN_ESTABLISHED:
        case NET_CONN_CLOSE_WAIT:   return localIsListening ? NET_DIR_INBOUND : NET_DIR_OUTBOUND;
        default:                    return NET_DIR_UNKNOWN;
    }
}

static NetConnection *NewConnection(ConnBuilder *b, NetProtocol proto)
{
    if (b->count >= b->cap) { b->truncated = true; return NULL; }
    NetConnection *c = &b->out[b->count++];
    memset(c, 0, sizeof(*c));
    c->key.protocol = proto;
    c->state = NET_CONN_UNKNOWN;
    c->direction = NET_DIR_UNKNOWN;
    return c;
}

static void EnumerateTcp(ConnBuilder *b)
{
    /* Phase 1: collect listening ports from both families (direction
     * inference needs the full listener set first). Phase 2: enumerate
     * every TCP endpoint and attach the listener relation.               */
    for (int phase = 0; phase < 2; phase++) {
        for (int fam = 0; fam < 2; fam++) {
            ULONG af = (fam == 0) ? AF_INET : AF_INET6;
            DWORD size = 0;
            GetExtendedTcpTable(NULL, &size, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0);
            if (size == 0) continue;
            BYTE *buffer = (BYTE *)malloc(size);
            if (!buffer) continue;
            DWORD err = GetExtendedTcpTable(buffer, &size, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0);
            if (err != NO_ERROR) { free(buffer); continue; }

            if (phase == 0) { /* collect listener ports for direction inference */
                if (af == AF_INET) {
                    MIB_TCP_TABLE_OWNER_PID_ALL *tab = (MIB_TCP_TABLE_OWNER_PID_ALL *)buffer;
                    for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                        if (tab->table[i].dwState == TCP_STATE_LISTEN) {
                            AddListenPort(b, ntohs((u_short)tab->table[i].dwLocalPort));
                        }
                    }
                } else {
                    MIB_TCP6_TABLE_OWNER_PID_ALL *tab = (MIB_TCP6_TABLE_OWNER_PID_ALL *)buffer;
                    for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                        if (tab->table[i].dwState == TCP_STATE_LISTEN) {
                            AddListenPort(b, ntohs((u_short)tab->table[i].dwLocalPort));
                        }
                    }
                }
                free(buffer);
                continue;
            }

            if (af == AF_INET) {
            MIB_TCP_TABLE_OWNER_PID_ALL *tab = (MIB_TCP_TABLE_OWNER_PID_ALL *)buffer;
            for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                const MIB_TCPROW_OWNER_PID *r = &tab->table[i];
                NetConnection *c = NewConnection(b, NET_PROTO_TCP);
                if (!c) break;
                CopyAddrFromUlong(r->dwLocalAddr, &c->key.localAddr);
                CopyAddrFromUlong(r->dwRemoteAddr, &c->key.remoteAddr);
                c->key.localPort = ntohs((u_short)r->dwLocalPort);
                c->key.remotePort = ntohs((u_short)r->dwRemotePort);
                c->pid = r->dwOwningPid;
                c->state = MapTcpState(r->dwState);
                c->direction = InferDirection(c->state, c->key.localPort,
                                              IsListeningPort(b, c->key.localPort));
                ResolvePidName(b->m, c->pid, c->processName, sizeof(c->processName),
                               &c->processStartTime);
                MIB_TCPROW statsRow = {r->dwState, r->dwLocalAddr, r->dwLocalPort,
                                       r->dwRemoteAddr, r->dwRemotePort};
                ReadTcpCounters4(&statsRow, c);
                b->tcpCount++;
            }
        } else {
            MIB_TCP6_TABLE_OWNER_PID_ALL *tab = (MIB_TCP6_TABLE_OWNER_PID_ALL *)buffer;
            for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                const MIB_TCP6ROW_OWNER_PID *r = &tab->table[i];
                NetConnection *c = NewConnection(b, NET_PROTO_TCP);
                if (!c) break;
                c->key.localAddr.kind = NET_ADDR_IPV6;
                memcpy(c->key.localAddr.bytes, r->ucLocalAddr, 16);
                c->key.localAddr.scopeId = r->dwLocalScopeId;
                c->key.remoteAddr.kind = NET_ADDR_IPV6;
                memcpy(c->key.remoteAddr.bytes, r->ucRemoteAddr, 16);
                c->key.remoteAddr.scopeId = r->dwRemoteScopeId;
                c->key.localPort = ntohs((u_short)r->dwLocalPort);
                c->key.remotePort = ntohs((u_short)r->dwRemotePort);
                c->pid = r->dwOwningPid;
                c->state = MapTcpState(r->dwState);
                c->direction = InferDirection(c->state, c->key.localPort,
                                              IsListeningPort(b, c->key.localPort));
                ResolvePidName(b->m, c->pid, c->processName, sizeof(c->processName),
                               &c->processStartTime);
                MIB_TCP6ROW statsRow;
                memset(&statsRow, 0, sizeof(statsRow));
                statsRow.State = (MIB_TCP_STATE)r->dwState;
                memcpy(&statsRow.LocalAddr, r->ucLocalAddr, 16);
                statsRow.dwLocalScopeId = r->dwLocalScopeId;
                statsRow.dwLocalPort = r->dwLocalPort;
                memcpy(&statsRow.RemoteAddr, r->ucRemoteAddr, 16);
                statsRow.dwRemoteScopeId = r->dwRemoteScopeId;
                statsRow.dwRemotePort = r->dwRemotePort;
                ReadTcpCounters6(&statsRow, c);
                b->tcpCount++;
            }
        }
        free(buffer);
        }
    }
}

static void EnumerateUdp(ConnBuilder *b)
{
    /* Windows only exposes the local bind for UDP endpoints (no remote peer),
     * so these connections carry NET_DIR_UNKNOWN and no remote address.       */
    for (int pass = 0; pass < 2; pass++) {
        ULONG af = (pass == 0) ? AF_INET : AF_INET6;
        DWORD size = 0;
        GetExtendedUdpTable(NULL, &size, FALSE, af, UDP_TABLE_OWNER_PID, 0);
        if (size == 0) continue;
        BYTE *buffer = (BYTE *)malloc(size);
        if (!buffer) continue;
        DWORD err = GetExtendedUdpTable(buffer, &size, FALSE, af, UDP_TABLE_OWNER_PID, 0);
        if (err != NO_ERROR) { free(buffer); continue; }

        if (af == AF_INET) {
            MIB_UDPTABLE_OWNER_PID *tab = (MIB_UDPTABLE_OWNER_PID *)buffer;
            for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                const MIB_UDPROW_OWNER_PID *r = &tab->table[i];
                NetConnection *c = NewConnection(b, NET_PROTO_UDP);
                if (!c) break;
                CopyAddrFromUlong(r->dwLocalAddr, &c->key.localAddr);
                c->key.localPort = ntohs((u_short)r->dwLocalPort);
                c->pid = r->dwOwningPid;
                c->state = NET_CONN_BOUND;
                c->direction = NET_DIR_UNKNOWN;
                ResolvePidName(b->m, c->pid, c->processName, sizeof(c->processName),
                               &c->processStartTime);
                b->udpCount++;
            }
        } else {
            MIB_UDP6TABLE_OWNER_PID *tab = (MIB_UDP6TABLE_OWNER_PID *)buffer;
            for (DWORD i = 0; i < tab->dwNumEntries; i++) {
                const MIB_UDP6ROW_OWNER_PID *r = &tab->table[i];
                NetConnection *c = NewConnection(b, NET_PROTO_UDP);
                if (!c) break;
                c->key.localAddr.kind = NET_ADDR_IPV6;
                memcpy(c->key.localAddr.bytes, r->ucLocalAddr, 16);
                c->key.localAddr.scopeId = r->dwLocalScopeId;
                c->key.localPort = ntohs((u_short)r->dwLocalPort);
                c->pid = r->dwOwningPid;
                c->state = NET_CONN_BOUND;
                c->direction = NET_DIR_UNKNOWN;
                ResolvePidName(b->m, c->pid, c->processName, sizeof(c->processName),
                               &c->processStartTime);
                b->udpCount++;
            }
        }
        free(buffer);
    }
}

static void EnumerateConnections(NetworkMonitor *m, NetSnapshot *snap)
{
    ConnBuilder b;
    memset(&b, 0, sizeof(b));
    b.m = m;
    b.out = snap->connections;
    b.cap = NET_MAX_CONNS;

    EnumerateTcp(&b);
    EnumerateUdp(&b);

    /* attribute each connection to the monitored adapter via its local
     * address; sockets with an unspecified local address cannot be tied to a
     * specific adapter and are left unattributed (onSelectedIface == false) */
    if (snap->selectedInterface >= 0) {
        const NetInterfaceInfo *sel = &snap->interfaces[snap->selectedInterface];
        for (int i = 0; i < b.count; i++) {
            b.out[i].onSelectedIface = LocalAddrBelongsTo(&b.out[i].key.localAddr, sel);
        }
    }

    snap->connectionCount = b.count;
    snap->truncated = b.truncated;
}

/* --------------------------------------------------------------- public --- */

static void MonitorWorker(void *ud);

NetworkMonitor *NetworkMonitor_Create(void)
{
    NetPlatform_Init();
    NetworkMonitor *m = (NetworkMonitor *)calloc(1, sizeof(NetworkMonitor));
    if (!m) return NULL;
    m->lock = NetMutex_Create();
    m->pollInterval = DEFAULT_POLL_SEC;
    m->snapshot.primaryInterface = -1;
    m->snapshot.selectedInterface = -1;
    m->snapshot.selectedIsSpecific = false;
    return m;
}

void NetworkMonitor_Start(NetworkMonitor *m)
{
    if (!m || m->thread) return;
    InterlockedExchange(&m->running, 1);
    m->thread = NetThread_Start(MonitorWorker, m);
}

void NetworkMonitor_Stop(NetworkMonitor *m)
{
    if (!m) return;
    InterlockedExchange(&m->running, 0);
    if (m->thread) {
        NetThread_Join(m->thread);
        m->thread = NULL;
    }
}

void NetworkMonitor_Destroy(NetworkMonitor *m)
{
    if (!m) return;
    NetworkMonitor_Stop(m);
    NetMutex_Destroy(m->lock);
    free(m);
    NetPlatform_Shutdown();
}

bool NetworkMonitor_GetSnapshot(NetworkMonitor *m, NetSnapshot *out)
{
    if (!m || !out) return false;
    NetMutex_Lock(m->lock);
    bool has = m->hasSnapshot;
    if (has) *out = m->snapshot;
    NetMutex_Unlock(m->lock);
    return has;
}

void NetworkMonitor_SetPollInterval(NetworkMonitor *m, double seconds)
{
    if (!m) return;
    if (seconds < 0.25) seconds = 0.25;
    if (seconds > 10.0) seconds = 10.0;
    m->pollInterval = seconds;
}

double NetworkMonitor_GetPollInterval(const NetworkMonitor *m)
{
    return m ? m->pollInterval : DEFAULT_POLL_SEC;
}

/* Selects which adapter is monitored, identified by its stable id
 * (NetInterfaceInfo.id, the system adapter GUID). An empty/NULL id resets to
 * the default "all adapters" mode. Safe to call at any time from any thread;
 * it only swaps a string under the monitor lock - enumeration and any OS work
 * stay on the worker thread.                                                    */
void NetworkMonitor_SetSelectedInterface(NetworkMonitor *m, const char *id)
{
    if (!m) return;
    NetMutex_Lock(m->lock);
    if (id && id[0]) {
        snprintf(m->requestedId, sizeof(m->requestedId), "%s", id);
    } else {
        m->requestedId[0] = '\0';
    }
    NetMutex_Unlock(m->lock);
}

void NetworkMonitor_GetSelectedInterface(const NetworkMonitor *m, char *out, size_t cap)
{
    if (!m || !out || cap == 0) return;
    NetMutex_Lock(m->lock);
    snprintf(out, cap, "%s", m->requestedId);
    NetMutex_Unlock(m->lock);
}

bool NetworkMonitor_PollOnce(NetworkMonitor *m)
{
    if (!m) return false;
    NetSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.primaryInterface = -1;
    snap.timestamp = NetTime_Now();

    EnumerateInterfaces(m, &snap);
    EnumerateConnections(m, &snap);

    if (snap.ok) {
        char tail[96];
        snprintf(tail, sizeof(tail), "; %d connections (tcp/udp tables)", snap.connectionCount);
        strncat(snap.status, tail, sizeof(snap.status) - strlen(snap.status) - 1);
    }

    NetMutex_Lock(m->lock);
    m->snapshot = snap;
    m->hasSnapshot = true;
    NetMutex_Unlock(m->lock);
    return snap.ok;
}

static void MonitorWorker(void *ud)
{
    NetworkMonitor *m = (NetworkMonitor *)ud;
    while (InterlockedCompareExchange(&m->running, 1, 1) == 1) {
        NetworkMonitor_PollOnce(m);

        /* sleep in small slices so Stop() stays responsive */
        uint32_t totalMs = (uint32_t)(m->pollInterval * 1000.0);
        if (totalMs < 50) totalMs = 50;
        uint32_t step = totalMs / 10;
        if (step < 5) step = 5;
        for (uint32_t slept = 0; slept < totalMs; slept += step) {
            if (InterlockedCompareExchange(&m->running, 1, 1) != 1) break;
            NetThread_SleepMs(step);
        }
    }
}
