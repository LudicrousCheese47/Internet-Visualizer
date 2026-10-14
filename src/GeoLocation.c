/*
 * GeoLocation.c - asynchronous geolocation + reverse DNS worker (Windows).
 *
 * Worker thread flow:
 *   1. "self" lookup  - discovers our own public IP and its geolocation.
 *   2. request queue  - for each queued public IP: reverse DNS, then an HTTPS
 *                       GET against the configured provider, then publish.
 *
 * Everything blocking (DNS, TLS, HTTP) happens on this thread. The render
 * thread only calls GeoLocation_RequestLookup / _TryGet / _PollResult, which
 * take the mutex for a short memcpy each.
 */
#include "GeoLocation.h"
#include "NetPlatform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GEO_CACHE_SLOTS     2048      /* power of two */
#define GEO_QUEUE_CAP       512       /* deep enough for startup backlogs     */
#define GEO_RESULT_CAP      512       /* results must not drop under bursts   */
#define GEO_MIN_REQUEST_GAP 0.4       /* seconds between HTTP requests        */
#define GEO_RETRY_COOLDOWN  300.0     /* retry failed lookups after 5 minutes */
#define GEO_RDNS_COOLDOWN   600.0     /* retry failed reverse DNS after 10 min*/
#define GEO_RATE_BACKOFF    30.0      /* pause after HTTP 429                 */
#define GEO_SELF_RETRY_SEC  30.0      /* retry the self lookup until it works */
#define GEO_FAILOVER_AFTER  1         /* 429/403 -> switch provider at once   */
#define GEO_MAX_RESPONSE    65536

/* --------------------------------------------------------------- structs -- */

typedef struct GeoCacheEntry {
    bool     used;
    char     ip[NET_STR_IP];
    GeoInfo  info;
    char     hostname[NET_STR_HOST];
    bool     hasHostname;
    double   rdnsFailedAt;
} GeoCacheEntry;

typedef struct GeoResult {
    char    ip[NET_STR_IP];
    GeoInfo info;
    bool    hasInfo;
    char    hostname[NET_STR_HOST];
    bool    hasHostname;
} GeoResult;

struct GeoLocation {
    NetMutex  *lock;
    NetCond   *wake;
    NetThread *thread;
    volatile LONG running;
    volatile LONG enabled;

    GeoCacheEntry *cache;
    uint32_t       cacheMask;

    char (*queue)[NET_STR_IP];
    int qCap, qHead, qCount;

    GeoResult *results;
    int rCap, rHead, rCount;

    GeoInfo self;
    char    selfIp[NET_STR_IP];
    bool    selfValid;

    char    providerHost[128];
    int     providerPort;
    bool    providerSecure;
    char    providerPathPrefix[64];  /* "" for ipwho.is, "/api/json" fallback */
    int     hardFails;               /* consecutive 429/403 from the provider  */
    bool    switchedProvider;        /* already failed over to the backup      */

    double  nextRequestAt;
    int     pending;
};

/* ------------------------------------------------------------ json utils -- */

static const char *JsonFind(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    size_t plen = strlen(pattern);
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            return q;
        }
        p += plen;
    }
    return NULL;
}

static bool JsonGetString(const char *json, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char *p = JsonFind(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'u':
                    /* skip a \uXXXX sequence, keep a placeholder */
                    for (int k = 0; k < 4 && p[1]; k++) p++;
                    out[i++] = '?';
                    break;
                default:  out[i++] = *p; break;
            }
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i > 0;
}

static bool JsonGetNumber(const char *json, const char *key, double *out)
{
    const char *p = JsonFind(json, key);
    if (!p) return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    if (out) *out = v;
    return true;
}

static bool JsonGetBool(const char *json, const char *key, bool *out)
{
    const char *p = JsonFind(json, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0)  { if (out) *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { if (out) *out = false; return true; }
    return false;
}

/* ---------------------------------------------------------------- cache --- */

static uint32_t HashIp(const char *ip)
{
    uint32_t h = 2166136261u;
    while (*ip) { h ^= (uint8_t)*ip++; h *= 16777619u; }
    return h;
}

static GeoCacheEntry *CacheFind(GeoLocation *g, const char *ip)
{
    uint32_t idx = HashIp(ip) & g->cacheMask;
    for (uint32_t i = 0; i <= g->cacheMask; i++) {
        GeoCacheEntry *e = &g->cache[(idx + i) & g->cacheMask];
        if (!e->used) return NULL;
        if (strcmp(e->ip, ip) == 0) return e;
    }
    return NULL;
}

static GeoCacheEntry *CacheFindOrCreate(GeoLocation *g, const char *ip)
{
    uint32_t idx = HashIp(ip) & g->cacheMask;
    GeoCacheEntry *oldest = &g->cache[idx];
    for (uint32_t i = 0; i <= g->cacheMask; i++) {
        GeoCacheEntry *e = &g->cache[(idx + i) & g->cacheMask];
        if (!e->used) {
            e->used = true;
            snprintf(e->ip, sizeof(e->ip), "%s", ip);
            return e;
        }
        if (strcmp(e->ip, ip) == 0) return e;
        if (e->info.resolvedAt < oldest->info.resolvedAt) oldest = e;
    }
    /* cache full: evict the oldest entry */
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    snprintf(oldest->ip, sizeof(oldest->ip), "%s", ip);
    return oldest;
}

/* ----------------------------------------------------------- queues ------- */

/* Newest-first insertion: an arc for a brand-new connection must not wait
 * behind the drain of an older backlog. Items already queued keep their
 * relative order; only the fresh request jumps ahead.                       */
static bool QueuePushFront(GeoLocation *g, const char *ip)
{
    if (g->qCount >= g->qCap) return false;
    g->qHead = (g->qHead - 1 + g->qCap) % g->qCap;
    snprintf(g->queue[g->qHead], NET_STR_IP, "%s", ip);
    g->qCount++;
    g->pending++;
    return true;
}

static bool QueuePop(GeoLocation *g, char *ipOut, size_t cap)
{
    if (g->qCount == 0) return false;
    snprintf(ipOut, cap, "%s", g->queue[g->qHead]);
    g->qHead = (g->qHead + 1) % g->qCap;
    g->qCount--;
    return true;
}

static void ResultPush(GeoLocation *g, const GeoResult *r)
{
    if (g->rCount >= g->rCap) {
        /* drop the oldest result to stay bounded */
        g->rHead = (g->rHead + 1) % g->rCap;
        g->rCount--;
    }
    int tail = (g->rHead + g->rCount) % g->rCap;
    g->results[tail] = *r;
    g->rCount++;
}

/* ------------------------------------------------------- http + parsing --- */

static bool ToWide(const char *src, WCHAR *dst, int dstCap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstCap);
    return n > 0;
}

/* GET https://host:port/path into outBuf. Returns false on transport error.  */
static bool HttpGetJson(GeoLocation *g, const char *path, char *outBuf, size_t cap,
                        DWORD *httpStatus, int *winHttpError)
{
    if (httpStatus) *httpStatus = 0;
    if (winHttpError) *winHttpError = 0;
    outBuf[0] = '\0';

    HINTERNET session = WinHttpOpen(L"InternetVisualizer/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (winHttpError) *winHttpError = (int)GetLastError();
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

    WCHAR whost[128];
    if (!ToWide(g->providerHost, whost, 128)) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET conn = WinHttpConnect(session, whost, (INTERNET_PORT)g->providerPort, 0);
    if (!conn) {
        if (winHttpError) *winHttpError = (int)GetLastError();
        WinHttpCloseHandle(session);
        return false;
    }

    WCHAR wpath[512];
    if (!ToWide(path, wpath, 512)) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD flags = g->providerSecure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(conn, L"GET", wpath, NULL,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        if (winHttpError) *winHttpError = (int)GetLastError();
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(request, NULL);

    DWORD status = 0;
    if (ok) {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        size_t total = 0;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) break;
            if (avail == 0) break;
            if (total + avail >= cap) avail = (DWORD)(cap - 1 - total);
            DWORD read = 0;
            if (!WinHttpReadData(request, outBuf + total, avail, &read)) break;
            total += read;
            if (total >= cap - 1) break;
        }
        outBuf[total] = '\0';
    } else if (winHttpError) {
        *winHttpError = (int)GetLastError();
    }
    if (httpStatus) *httpStatus = status;

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok ? true : false;
}

static void ParseProviderJson(const char *json, GeoInfo *info, char *ipOut, size_t ipCap)
{
    char buf[256];

    if (ipOut && ipCap) {
        if (JsonGetString(json, "ip", buf, sizeof(buf))) {
            snprintf(ipOut, ipCap, "%s", buf);
        } else if (JsonGetString(json, "ipAddress", buf, sizeof(buf))) {
            snprintf(ipOut, ipCap, "%s", buf);   /* freeipapi.com             */
        }
    }
    if (JsonGetString(json, "country",     info->country,     sizeof(info->country)))     {}
    else if (JsonGetString(json, "countryName", info->country, sizeof(info->country)))     {}
    if (JsonGetString(json, "country_code",info->countryCode, sizeof(info->countryCode))) {}
    else if (JsonGetString(json, "countryCode",info->countryCode, sizeof(info->countryCode))) {}
    if (JsonGetString(json, "region",      info->region,      sizeof(info->region)))      {}
    else if (JsonGetString(json, "regionName", info->region,   sizeof(info->region)))      {}
    if (JsonGetString(json, "city",        info->city,        sizeof(info->city)))        {}
    else if (JsonGetString(json, "cityName", info->city,       sizeof(info->city)))        {}
    if (JsonGetString(json, "timezone",    info->timezone,    sizeof(info->timezone)))    {}

    double lat = 0.0, lon = 0.0;
    bool hasLat = JsonGetNumber(json, "latitude", &lat);
    bool hasLon = JsonGetNumber(json, "longitude", &lon);
    if (!hasLat) hasLat = JsonGetNumber(json, "lat", &lat);
    if (!hasLon) hasLon = JsonGetNumber(json, "lon", &lon);
    if (hasLat && hasLon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        info->latitude = lat;
        info->longitude = lon;
        info->hasCoords = true;
    }

    double asnNum = 0.0;
    if (JsonGetNumber(json, "asn", &asnNum)) {
        snprintf(info->asn, sizeof(info->asn), "AS%lld", (long long)asnNum);
    } else if (JsonGetString(json, "asn", buf, sizeof(buf))) {
        snprintf(info->asn, sizeof(info->asn), "%s", buf);
    }
    if (JsonGetString(json, "isp",  info->isp,  sizeof(info->isp)) && info->isp[0] == '\0') {}
    if (info->isp[0] == '\0' && JsonGetString(json, "org", info->isp, sizeof(info->isp))) {}
    if (info->isp[0] == '\0' &&
        JsonGetString(json, "asnOrganization", info->isp, sizeof(info->isp))) {}   /* freeipapi */
    if (JsonGetString(json, "domain", info->domain, sizeof(info->domain))) {}
    /* some providers nest the connection details */
    if (info->isp[0] == '\0') {
        const char *conn = JsonFind(json, "connection");
        if (conn) {
            if (JsonGetString(conn, "isp", info->isp, sizeof(info->isp))) {}
            if (JsonGetString(conn, "org", info->isp, sizeof(info->isp))) {}
            if (info->asn[0] == '\0' && JsonGetNumber(conn, "asn", &asnNum)) {
                snprintf(info->asn, sizeof(info->asn), "AS%lld", (long long)asnNum);
            }
            if (info->domain[0] == '\0' && JsonGetString(conn, "domain", info->domain, sizeof(info->domain))) {}
        }
    }
    /* timezone may be nested under "timezone":{"id":"Europe/Berlin"} */
    if (info->timezone[0] == '\0') {
        const char *tz = JsonFind(json, "timezone");
        if (tz) JsonGetString(tz, "id", info->timezone, sizeof(info->timezone));
    }
}

/* ------------------------------------------------------- provider failover */

/* freeipapi.com answers the same way (latitude/longitude, country, city, ip)
 * and is used automatically when the configured provider keeps rejecting us
 * (HTTP 429/403). Without this, a provider that rate-limits the user's IP
 * means NO coordinate can ever resolve and therefore no arc is ever drawn.  */
static const struct { const char *host; const char *pathPrefix; } kFallbackProvider = {
    "freeipapi.com", "/api/json"
};

/* Call after every HTTP response. Returns true when we just switched.       */
static bool NoteProviderStatus(GeoLocation *g, DWORD status)
{
    if (status == 429 || status == 403) {
        if (!g->switchedProvider) {
            g->hardFails++;
            if (g->hardFails >= GEO_FAILOVER_AFTER) {
                snprintf(g->providerHost, sizeof(g->providerHost), "%s",
                         kFallbackProvider.host);
                snprintf(g->providerPathPrefix, sizeof(g->providerPathPrefix), "%s",
                         kFallbackProvider.pathPrefix);
                g->switchedProvider = true;
                g->hardFails = 0;
                return true;
            }
        }
    } else if (status == 200) {
        g->hardFails = 0;
    }
    return false;
}

/* Performs the HTTPS lookup for one IP and stores the outcome in the cache.   */
static void FetchGeoForIp(GeoLocation *g, const char *ip)
{
    GeoInfo info;
    memset(&info, 0, sizeof(info));
    info.status = GEO_FAILED;
    info.resolvedAt = NetTime_Now();

    /* respect the rate limit gate */
    double now = NetTime_Now();
    if (now < g->nextRequestAt) {
        NetThread_SleepMs((uint32_t)((g->nextRequestAt - now) * 1000.0) + 1);
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", g->providerPathPrefix, ip);
    char *body = (char *)malloc(GEO_MAX_RESPONSE);
    if (!body) { info.status = GEO_FAILED; }
    else {
        DWORD status = 0;
        int whErr = 0;
        bool transportOk = HttpGetJson(g, path, body, GEO_MAX_RESPONSE, &status, &whErr);
        if (!transportOk) {
            info.status = ((whErr == ERROR_WINHTTP_NAME_NOT_RESOLVED) ||
                           (whErr == ERROR_WINHTTP_CANNOT_CONNECT) ||
                           (whErr == ERROR_WINHTTP_TIMEOUT) ||
                           (whErr == ERROR_WINHTTP_CONNECTION_ERROR))
                          ? GEO_OFFLINE : GEO_FAILED;
        } else if (status == 429) {
            info.status = GEO_RATE_LIMITED;
            g->nextRequestAt = NetTime_Now() + GEO_RATE_BACKOFF;
        } else if (status != 200) {
            info.status = (status == 0) ? GEO_OFFLINE : GEO_FAILED;
        } else {
            bool success = true;
            JsonGetBool(body, "success", &success);
            if (!success) {
                info.status = GEO_FAILED;
            } else {
                char selfIp[NET_STR_IP];
                ParseProviderJson(body, &info, selfIp, sizeof(selfIp));
                info.status = info.hasCoords ? GEO_OK : GEO_FAILED;
            }
        }
        if (NoteProviderStatus(g, status)) {
            /* provider just changed: give the fallback a chance on this IP */
            free(body);
            body = NULL;
            NetThread_SleepMs(50);
            body = (char *)malloc(GEO_MAX_RESPONSE);
            if (body) {
                status = 0; whErr = 0;
                if (HttpGetJson(g, path, body, GEO_MAX_RESPONSE, &status, &whErr) &&
                    status == 200) {
                    bool success = true;
                    JsonGetBool(body, "success", &success);
                    if (success) {
                        char selfIp[NET_STR_IP];
                        ParseProviderJson(body, &info, selfIp, sizeof(selfIp));
                        info.status = info.hasCoords ? GEO_OK : GEO_FAILED;
                    }
                }
            }
        }
        free(body);
    }
    /* Enforce the minimum gap between requests, but NEVER shorten a longer
     * backoff that this same request may have just set (HTTP 429 sets a 30 s
     * pause). Overwriting it here is what made the worker hammer the provider
     * every GEO_MIN_REQUEST_GAP and earn a permanent rate limit.              */
    {
        double floorNext = NetTime_Now() + GEO_MIN_REQUEST_GAP;
        if (g->nextRequestAt < floorNext) g->nextRequestAt = floorNext;
    }

    GeoResult r;
    memset(&r, 0, sizeof(r));
    snprintf(r.ip, sizeof(r.ip), "%s", ip);
    r.info = info;
    r.hasInfo = true;

    NetMutex_Lock(g->lock);
    GeoCacheEntry *e = CacheFindOrCreate(g, ip);
    e->info = info;
    ResultPush(g, &r);
    NetMutex_Unlock(g->lock);
}

/* Reverse DNS with a cooldown so unresolvable IPs are not retried constantly. */
static void ReverseDnsForIp(GeoLocation *g, const char *ip)
{
    char host[NET_STR_HOST];
    host[0] = '\0';

    NetAddr addr;
    if (!NetAddr_FromString(ip, &addr)) return;

    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t slen = 0;
    if (addr.kind == NET_ADDR_IPV4) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
        v4->sin_family = AF_INET;
        memcpy(&v4->sin_addr, addr.bytes, 4);
        slen = sizeof(*v4);
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;
        v6->sin6_family = AF_INET6;
        memcpy(&v6->sin6_addr, addr.bytes, 16);
        v6->sin6_scope_id = addr.scopeId;
        slen = sizeof(*v6);
    }

    if (getnameinfo((struct sockaddr *)&ss, slen, host, sizeof(host),
                    NULL, 0, NI_NAMEREQD) != 0) {
        host[0] = '\0';
    }

    GeoResult r;
    memset(&r, 0, sizeof(r));
    snprintf(r.ip, sizeof(r.ip), "%s", ip);

    NetMutex_Lock(g->lock);
    GeoCacheEntry *e = CacheFindOrCreate(g, ip);
    double now = NetTime_Now();
    if (host[0] != '\0') {
        snprintf(e->hostname, sizeof(e->hostname), "%s", host);
        e->hasHostname = true;
        e->rdnsFailedAt = 0.0;
        snprintf(r.hostname, sizeof(r.hostname), "%s", host);
        r.hasHostname = true;
    } else {
        e->rdnsFailedAt = now;
    }
    if (r.hasHostname) ResultPush(g, &r);
    NetMutex_Unlock(g->lock);
}

static bool ShouldDoReverseDns(GeoLocation *g, const char *ip)
{
    double now = NetTime_Now();
    NetMutex_Lock(g->lock);
    GeoCacheEntry *e = CacheFind(g, ip);
    bool skip = (e && e->hasHostname) ||
                (e && e->rdnsFailedAt > 0.0 && (now - e->rdnsFailedAt) < GEO_RDNS_COOLDOWN);
    NetMutex_Unlock(g->lock);
    return !skip;
}

/* ---------------------------------------------------------- self lookup --- */

/* Resolves our own public address + coordinates. This is NOT optional: every
 * arc needs local.hasGeo, which is only ever set from this result. Returns
 * true once the self endpoint is valid.                                       */
static bool PerformSelfLookup(GeoLocation *g)
{
    if (GeoLocation_HasSelf(g)) return true;

    /* respect the same rate gate as normal lookups */
    double now = NetTime_Now();
    if (now < g->nextRequestAt)
        NetThread_SleepMs((uint32_t)((g->nextRequestAt - now) * 1000.0) + 1);

    bool ok = false;
    char *body = (char *)malloc(GEO_MAX_RESPONSE);
    if (body) {
        DWORD status = 0;
        int whErr = 0;
        char selfPath[160];

        /* up to two attempts: the configured provider, then (if it hard-fails
         * with 429/403) the automatic fallback provider                       */
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            snprintf(selfPath, sizeof(selfPath), "%s/", g->providerPathPrefix);
            if (!HttpGetJson(g, selfPath, body, GEO_MAX_RESPONSE, &status, &whErr)) break;
            if (status != 200) {
                bool switched = NoteProviderStatus(g, status);
                if (status == 429) g->nextRequestAt = NetTime_Now() + GEO_RATE_BACKOFF;
                if (switched) continue;   /* retry immediately on the fallback */
                break;
            }
            GeoInfo info;
            memset(&info, 0, sizeof(info));
            info.status = GEO_FAILED;
            char selfIp[NET_STR_IP];
            selfIp[0] = '\0';
            bool success = true;
            JsonGetBool(body, "success", &success);
            if (success) {
                ParseProviderJson(body, &info, selfIp, sizeof(selfIp));
                info.status = info.hasCoords ? GEO_OK : GEO_FAILED;
                NetMutex_Lock(g->lock);
                if (selfIp[0] != '\0') {
                    snprintf(g->selfIp, sizeof(g->selfIp), "%s", selfIp);
                    /* remember our own address so it is never re-requested */
                    GeoCacheEntry *e = CacheFindOrCreate(g, selfIp);
                    e->info = info;
                    e->info.resolvedAt = NetTime_Now();
                }
                g->self = info;
                g->selfValid = (selfIp[0] != '\0');
                NetMutex_Unlock(g->lock);
                ok = (selfIp[0] != '\0');
            }
            break;
        }
        free(body);
    }
    g->nextRequestAt = NetTime_Now() + GEO_MIN_REQUEST_GAP;
    return ok || GeoLocation_HasSelf(g);
}

/* -------------------------------------------------------------- worker ---- */

static void GeoWorker(void *ud)
{
    GeoLocation *g = (GeoLocation *)ud;

    /* The self lookup anchors the local endpoint of every arc, so keep trying
     * until it succeeds instead of giving up after one attempt (a single 429
     * at startup used to leave selfValid false forever, and then NO arc could
     * ever be drawn no matter how well remote lookups resolved).            */
    double nextSelfRetryAt = 0.0;

    char ip[NET_STR_IP];
    while (InterlockedCompareExchange(&g->running, 1, 1) == 1) {
        if (InterlockedCompareExchange(&g->enabled, 1, 1) != 1) {
            NetMutex_Lock(g->lock);
            NetCond_Wait(g->wake, g->lock, 200);
            NetMutex_Unlock(g->lock);
            continue;
        }

        bool did = false;

        /* self lookup first - it gates arc drawing for the whole session */
        if (!GeoLocation_HasSelf(g) && NetTime_Now() >= nextSelfRetryAt) {
            if (!PerformSelfLookup(g))
                nextSelfRetryAt = NetTime_Now() + GEO_SELF_RETRY_SEC;
            did = true;
        }

        NetMutex_Lock(g->lock);
        bool have = QueuePop(g, ip, sizeof(ip));
        NetMutex_Unlock(g->lock);

        if (have) {
            /* Geo lookup first: publishing coordinates must never wait behind
             * a slow reverse-DNS timeout; the hostname lands when it is ready */
            FetchGeoForIp(g, ip);
            if (ShouldDoReverseDns(g, ip)) ReverseDnsForIp(g, ip);
            NetMutex_Lock(g->lock);
            if (g->pending > 0) g->pending--;
            NetMutex_Unlock(g->lock);
            did = true;
        }

        if (!did) {
            NetMutex_Lock(g->lock);
            NetCond_Wait(g->wake, g->lock, 250);
            NetMutex_Unlock(g->lock);
        }
    }
}

/* -------------------------------------------------------------- public ---- */

GeoLocation *GeoLocation_Create(void)
{
    NetPlatform_Init();
    GeoLocation *g = (GeoLocation *)calloc(1, sizeof(GeoLocation));
    if (!g) return NULL;

    g->lock = NetMutex_Create();
    g->wake = NetCond_Create();
    g->cache = (GeoCacheEntry *)calloc(GEO_CACHE_SLOTS, sizeof(GeoCacheEntry));
    g->cacheMask = GEO_CACHE_SLOTS - 1;
    g->qCap = GEO_QUEUE_CAP;
    g->queue = (char (*)[NET_STR_IP])calloc((size_t)g->qCap, NET_STR_IP);
    g->rCap = GEO_RESULT_CAP;
    g->results = (GeoResult *)calloc((size_t)g->rCap, sizeof(GeoResult));
    if (!g->lock || !g->wake || !g->cache || !g->queue || !g->results) {
        GeoLocation_Destroy(g);
        return NULL;
    }

    snprintf(g->providerHost, sizeof(g->providerHost), "ipwho.is");
    g->providerPort = INTERNET_DEFAULT_HTTPS_PORT;
    g->providerSecure = true;
    g->providerPathPrefix[0] = '\0';      /* ipwho.is uses "/<ip>"            */
    g->hardFails = 0;
    g->switchedProvider = false;
    g->enabled = 1;
    g->nextRequestAt = 0.0;
    return g;
}

void GeoLocation_Destroy(GeoLocation *g)
{
    if (!g) return;
    GeoLocation_Stop(g);
    if (g->wake)  NetCond_Destroy(g->wake);
    if (g->lock)  NetMutex_Destroy(g->lock);
    free(g->cache);
    free(g->queue);
    free(g->results);
    free(g);
    NetPlatform_Shutdown();
}

void GeoLocation_Start(GeoLocation *g)
{
    if (!g || g->thread) return;
    InterlockedExchange(&g->running, 1);
    g->thread = NetThread_Start(GeoWorker, g);
}

void GeoLocation_Stop(GeoLocation *g)
{
    if (!g) return;
    InterlockedExchange(&g->running, 0);
    NetMutex_Lock(g->lock);
    NetCond_Signal(g->wake);
    NetMutex_Unlock(g->lock);
    if (g->thread) {
        NetThread_Join(g->thread);
        g->thread = NULL;
    }
}

void GeoLocation_RequestLookup(GeoLocation *g, const char *ip)
{
    if (!g || !ip || !ip[0]) return;

    NetAddr addr;
    if (!NetAddr_FromString(ip, &addr)) return;
    NetAddrClass cls = NetAddr_Classify(&addr);

    NetMutex_Lock(g->lock);

    /* Non-public addresses: classify locally, never queue a network request.  */
    if (cls != NET_ADDR_CLASS_PUBLIC) {
        GeoStatus st = GEO_PRIVATE;
        switch (cls) {
            case NET_ADDR_CLASS_LOOPBACK:   st = GEO_LOOPBACK;   break;
            case NET_ADDR_CLASS_LINK_LOCAL: st = GEO_LINK_LOCAL; break;
            case NET_ADDR_CLASS_MULTICAST:  st = GEO_MULTICAST;  break;
            case NET_ADDR_CLASS_RESERVED:   st = GEO_RESERVED;   break;
            case NET_ADDR_CLASS_CGNAT:      st = GEO_PRIVATE;    break;
            case NET_ADDR_CLASS_NONE:       st = GEO_NOT_APPLICABLE; break;
            default: break;
        }
        GeoCacheEntry *e = CacheFindOrCreate(g, ip);
        if (e->info.status != st || e->info.resolvedAt == 0.0) {
            memset(&e->info, 0, sizeof(e->info));
            e->info.status = st;
            e->info.resolvedAt = NetTime_Now();
            GeoResult r;
            memset(&r, 0, sizeof(r));
            snprintf(r.ip, sizeof(r.ip), "%s", ip);
            r.info = e->info;
            r.hasInfo = true;
            ResultPush(g, &r);
        }
        NetMutex_Unlock(g->lock);
        return;
    }

    GeoCacheEntry *e = CacheFind(g, ip);
    double now = NetTime_Now();
    if (e) {
        bool finished = (e->info.status != GEO_NONE && e->info.status != GEO_PENDING);
        bool failed = (e->info.status == GEO_FAILED || e->info.status == GEO_OFFLINE ||
                       e->info.status == GEO_RATE_LIMITED);
        bool retryDue = failed && (now - e->info.resolvedAt) > GEO_RETRY_COOLDOWN;
        if ((finished && !retryDue) || e->info.status == GEO_PENDING) {
            /* Already resolved (or already queued). Publish the cached outcome
             * immediately so an arch created for a repeat IP resolves on this
             * very frame instead of waiting for a result that never comes.  */
            if (finished && !retryDue) {
                GeoResult r;
                memset(&r, 0, sizeof(r));
                snprintf(r.ip, sizeof(r.ip), "%s", ip);
                r.info = e->info;
                r.hasInfo = true;
                if (e->hasHostname) {
                    snprintf(r.hostname, sizeof(r.hostname), "%s", e->hostname);
                    r.hasHostname = true;
                }
                ResultPush(g, &r);
            }
            NetMutex_Unlock(g->lock);
            return;
        }
        e->info.status = GEO_PENDING;
        if (!QueuePushFront(g, ip)) e->info.status = GEO_NONE;  /* full: retryable */
    } else {
        e = CacheFindOrCreate(g, ip);
        e->info.status = GEO_PENDING;
        e->info.resolvedAt = now;
        if (!QueuePushFront(g, ip)) e->info.status = GEO_NONE;  /* full: retryable */
    }
    NetMutex_Unlock(g->lock);
    NetCond_Signal(g->wake);
}

bool GeoLocation_TryGet(GeoLocation *g, const char *ip, GeoInfo *out,
                        char *hostnameOut, size_t hostnameCap)
{
    if (!g || !ip) return false;
    NetMutex_Lock(g->lock);
    GeoCacheEntry *e = CacheFind(g, ip);
    bool found = (e != NULL);
    if (found) {
        if (out) *out = e->info;
        if (hostnameOut && hostnameCap) {
            snprintf(hostnameOut, hostnameCap, "%s", e->hasHostname ? e->hostname : "");
        }
    }
    NetMutex_Unlock(g->lock);
    return found;
}

bool GeoLocation_PollResult(GeoLocation *g, char *ipOut, size_t ipCap,
                            GeoInfo *out, char *hostnameOut, size_t hostnameCap)
{
    if (!g) return false;
    GeoResult r;
    NetMutex_Lock(g->lock);
    bool have = (g->rCount > 0);
    if (have) {
        r = g->results[g->rHead];
        g->rHead = (g->rHead + 1) % g->rCap;
        g->rCount--;
    }
    NetMutex_Unlock(g->lock);
    if (!have) return false;

    if (ipOut && ipCap) snprintf(ipOut, ipCap, "%s", r.ip);
    if (out) *out = r.info;
    if (hostnameOut && hostnameCap) {
        snprintf(hostnameOut, hostnameCap, "%s", r.hasHostname ? r.hostname : "");
    }
    return r.hasInfo || r.hasHostname;
}

bool GeoLocation_GetSelf(GeoLocation *g, GeoInfo *out, char *ipOut, size_t ipCap)
{
    if (!g) return false;
    NetMutex_Lock(g->lock);
    bool ok = g->selfValid;
    if (ok && out) *out = g->self;
    if (ok && ipOut && ipCap) snprintf(ipOut, ipCap, "%s", g->selfIp);
    NetMutex_Unlock(g->lock);
    return ok;
}

bool GeoLocation_HasSelf(const GeoLocation *g)
{
    return g ? g->selfValid : false;
}

void GeoLocation_SetEnabled(GeoLocation *g, bool enabled)
{
    if (!g) return;
    InterlockedExchange(&g->enabled, enabled ? 1 : 0);
    NetMutex_Lock(g->lock);
    NetCond_Signal(g->wake);
    NetMutex_Unlock(g->lock);
}

bool GeoLocation_GetEnabled(GeoLocation *g)
{
    return g ? (InterlockedCompareExchange(&g->enabled, 1, 1) == 1) : false;
}

int GeoLocation_PendingCount(GeoLocation *g)
{
    if (!g) return 0;
    NetMutex_Lock(g->lock);
    int n = g->pending;
    NetMutex_Unlock(g->lock);
    return n;
}

void GeoLocation_SetProvider(GeoLocation *g, const char *baseUrl)
{
    if (!g || !baseUrl || !baseUrl[0]) return;

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", baseUrl);
    bool secure = true;
    char *p = tmp;
    if (strncmp(p, "https://", 8) == 0)      { p += 8; secure = true; }
    else if (strncmp(p, "http://", 7) == 0)  { p += 7; secure = false; }

    int port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    char *slash = strchr(p, '/');
    if (slash) *slash = '\0';
    char *colon = strchr(p, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) port = secure ? 443 : 80;
    }

    NetMutex_Lock(g->lock);
    snprintf(g->providerHost, sizeof(g->providerHost), "%s", p);
    g->providerPort = port;
    g->providerSecure = secure;
    NetMutex_Unlock(g->lock);
}

const char *GeoLocation_GetProvider(const GeoLocation *g)
{
    return g ? g->providerHost : "";
}
