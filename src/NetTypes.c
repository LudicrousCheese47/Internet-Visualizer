/* NetTypes.c - helpers for the shared networking data types (Windows). */
#include "NetTypes.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ addr -- */

void NetAddr_Clear(NetAddr *a)
{
    memset(a, 0, sizeof(*a));
}

bool NetAddr_IsZero(const NetAddr *a)
{
    if (a->kind == NET_ADDR_NONE) return true;
    for (int i = 0; i < 16; i++) {
        if (a->bytes[i] != 0) return false;
    }
    return true;
}

bool NetAddr_Equals(const NetAddr *a, const NetAddr *b)
{
    if (a->kind != b->kind) return false;
    if (a->kind == NET_ADDR_NONE) return true;
    int len = (a->kind == NET_ADDR_IPV4) ? 4 : 16;
    if (memcmp(a->bytes, b->bytes, (size_t)len) != 0) return false;
    if (a->kind == NET_ADDR_IPV6 && a->scopeId != b->scopeId) return false;
    return true;
}

uint64_t NetAddr_Hash(const NetAddr *a)
{
    /* FNV-1a */
    uint64_t h = 1469598103934665603ULL;
    int len = (a->kind == NET_ADDR_IPV4) ? 4 : (a->kind == NET_ADDR_IPV6 ? 16 : 0);
    for (int i = 0; i < len; i++) {
        h ^= a->bytes[i];
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)a->kind;
    h *= 1099511628211ULL;
    return h;
}

static bool IsIpv4Mapped(const NetAddr *a, NetAddr *out)
{
    static const uint8_t prefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF};
    if (a->kind != NET_ADDR_IPV6) return false;
    if (memcmp(a->bytes, prefix, 12) != 0) return false;
    if (out) {
        out->kind = NET_ADDR_IPV4;
        memcpy(out->bytes, a->bytes + 12, 4);
        out->scopeId = 0;
    }
    return true;
}

NetAddrClass NetAddr_Classify(const NetAddr *a)
{
    if (NetAddr_IsZero(a)) return NET_ADDR_CLASS_NONE;

    NetAddr mapped;
    if (IsIpv4Mapped(a, &mapped)) return NetAddr_Classify(&mapped);

    if (a->kind == NET_ADDR_IPV4) {
        const uint8_t *b = a->bytes;
        if (b[0] == 127)                                  return NET_ADDR_CLASS_LOOPBACK;
        if (b[0] == 10)                                   return NET_ADDR_CLASS_PRIVATE;
        if (b[0] == 172 && b[1] >= 16 && b[1] <= 31)      return NET_ADDR_CLASS_PRIVATE;
        if (b[0] == 192 && b[1] == 168)                   return NET_ADDR_CLASS_PRIVATE;
        if (b[0] == 169 && b[1] == 254)                   return NET_ADDR_CLASS_LINK_LOCAL;
        if (b[0] == 100 && b[1] >= 64 && b[1] <= 127)     return NET_ADDR_CLASS_CGNAT;
        if (b[0] >= 224 && b[0] <= 239)                   return NET_ADDR_CLASS_MULTICAST;
        if (b[0] == 0)                                    return NET_ADDR_CLASS_RESERVED;
        if (b[0] >= 240)                                  return NET_ADDR_CLASS_RESERVED;
        return NET_ADDR_CLASS_PUBLIC;
    }

    if (a->kind == NET_ADDR_IPV6) {
        const uint8_t *b = a->bytes;
        bool allZero = true;
        for (int i = 0; i < 15; i++) if (b[i] != 0) allZero = false;
        if (allZero && b[15] == 1)                        return NET_ADDR_CLASS_LOOPBACK;
        if (allZero && b[15] == 0)                        return NET_ADDR_CLASS_RESERVED;
        if ((b[0] & 0xFE) == 0xFC)                        return NET_ADDR_CLASS_PRIVATE;   /* fc00::/7 */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)        return NET_ADDR_CLASS_LINK_LOCAL;
        if (b[0] == 0xFF)                                 return NET_ADDR_CLASS_MULTICAST;
        if (b[0] == 0x20 && b[1] == 0x02)                 return NET_ADDR_CLASS_PUBLIC;    /* 6to4     */
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D) return NET_ADDR_CLASS_PUBLIC;    /* teredo   */
        if (b[0] == 0x20 && (b[1] & 0xC0) == 0x00)        return NET_ADDR_CLASS_PUBLIC;    /* 2000::/3 */
        return NET_ADDR_CLASS_RESERVED;
    }
    return NET_ADDR_CLASS_NONE;
}

bool NetAddr_FromString(const char *text, NetAddr *out)
{
    if (!out) return false;
    NetAddr_Clear(out);
    if (!text || !text[0]) return false;

    struct in_addr v4;
    if (inet_pton(AF_INET, text, &v4) == 1) {
        out->kind = NET_ADDR_IPV4;
        memcpy(out->bytes, &v4, 4);
        return true;
    }
    struct in6_addr v6;
    if (inet_pton(AF_INET6, text, &v6) == 1) {
        out->kind = NET_ADDR_IPV6;
        memcpy(out->bytes, &v6, 16);
        return true;
    }
    return false;
}

void NetAddr_ToString(const NetAddr *a, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!a || a->kind == NET_ADDR_NONE || NetAddr_IsZero(a)) {
        snprintf(buf, cap, "none");
        return;
    }
    if (a->kind == NET_ADDR_IPV4) {
        snprintf(buf, cap, "%u.%u.%u.%u", a->bytes[0], a->bytes[1], a->bytes[2], a->bytes[3]);
        return;
    }
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    memcpy(&sa.sin6_addr, a->bytes, 16);
    sa.sin6_scope_id = a->scopeId;
    DWORD len = (DWORD)cap;
    if (WSAAddressToStringA((struct sockaddr *)&sa, sizeof(sa), NULL, buf, &len) != 0) {
        snprintf(buf, cap, "invalid-ipv6");
    }
}

/* ------------------------------------------------------------------- key -- */

bool NetConnKey_Equals(const NetConnKey *a, const NetConnKey *b)
{
    if (a->protocol != b->protocol) return false;
    if (a->localPort != b->localPort) return false;
    if (a->remotePort != b->remotePort) return false;
    if (!NetAddr_Equals(&a->localAddr, &b->localAddr)) return false;
    if (!NetAddr_Equals(&a->remoteAddr, &b->remoteAddr)) return false;
    return true;
}

uint64_t NetConnKey_Hash(const NetConnKey *k)
{
    uint64_t h = 1469598103934665603ULL;
    h = (h ^ (uint64_t)k->protocol) * 1099511628211ULL;
    h = (h ^ (uint64_t)k->localPort) * 1099511628211ULL;
    h = (h ^ (uint64_t)k->remotePort) * 1099511628211ULL;

    uint64_t lh = NetAddr_Hash(&k->localAddr);
    uint64_t rh = NetAddr_Hash(&k->remoteAddr);
    h = (h ^ lh) * 1099511628211ULL;
    h = (h ^ rh) * 1099511628211ULL;
    return h;
}

/* ------------------------------------------------------------- endpoints -- */

void NetEndpoint_Clear(ConnectionEndpoint *e)
{
    memset(e, 0, sizeof(*e));
    e->geoStatus = GEO_NONE;
}

void NetEndpoint_SetAddress(ConnectionEndpoint *e, const char *ip, uint16_t port)
{
    snprintf(e->ip, sizeof(e->ip), "%s", ip ? ip : "");
    e->port = port;
    e->isPublic = false;
    NetAddrClass cls = NET_ADDR_CLASS_NONE;
    NetAddr a;
    NetAddr_Clear(&a);
    if (ip && ip[0]) {
        if (InetPtonA(AF_INET, ip, &a.bytes) == 1) {
            a.kind = NET_ADDR_IPV4;
            cls = NetAddr_Classify(&a);
        } else if (InetPtonA(AF_INET6, ip, &a.bytes) == 1) {
            a.kind = NET_ADDR_IPV6;
            cls = NetAddr_Classify(&a);
        }
    }
    e->isPublic = (cls == NET_ADDR_CLASS_PUBLIC);
}

/* ------------------------------------------------------------------ names -- */

const char *NetProtocolName(NetProtocol p)
{
    switch (p) {
        case NET_PROTO_TCP: return "TCP";
        case NET_PROTO_UDP: return "UDP";
        default:            return "???";
    }
}

const char *NetDirectionName(NetDirection d)
{
    switch (d) {
        case NET_DIR_OUTBOUND: return "outbound";
        case NET_DIR_INBOUND:  return "inbound";
        case NET_DIR_LISTENER: return "listening";
        default:               return "unknown";
    }
}

const char *NetConnStateName(NetConnState s)
{
    switch (s) {
        case NET_CONN_LISTEN:      return "LISTEN";
        case NET_CONN_ESTABLISHED: return "ESTABLISHED";
        case NET_CONN_SYN_SENT:    return "SYN_SENT";
        case NET_CONN_SYN_RECEIVED:return "SYN_RCVD";
        case NET_CONN_FIN_WAIT1:   return "FIN_WAIT1";
        case NET_CONN_FIN_WAIT2:   return "FIN_WAIT2";
        case NET_CONN_CLOSE_WAIT:  return "CLOSE_WAIT";
        case NET_CONN_CLOSING:     return "CLOSING";
        case NET_CONN_LAST_ACK:    return "LAST_ACK";
        case NET_CONN_TIME_WAIT:   return "TIME_WAIT";
        case NET_CONN_CLOSED:      return "CLOSED";
        case NET_CONN_BOUND:       return "BOUND";
        default:                   return "UNKNOWN";
    }
}

const char *GeoStatusName(GeoStatus s)
{
    switch (s) {
        case GEO_NONE:            return "none";
        case GEO_PENDING:         return "resolving...";
        case GEO_OK:              return "ok";
        case GEO_PRIVATE:         return "private address";
        case GEO_LOOPBACK:        return "loopback";
        case GEO_LINK_LOCAL:      return "link-local";
        case GEO_MULTICAST:       return "multicast";
        case GEO_RESERVED:        return "reserved";
        case GEO_NOT_APPLICABLE:  return "not applicable";
        case GEO_RATE_LIMITED:    return "rate limited";
        case GEO_OFFLINE:         return "offline";
        case GEO_FAILED:          return "lookup failed";
        default:                  return "?";
    }
}

/* ---------------------------------------------------------------- format -- */

void Net_FormatBytes(int64_t bytes, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (bytes == NET_BYTES_UNAVAILABLE) { snprintf(out, cap, "n/a"); return; }
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0, tb = gb * 1024.0;
    double v = (double)bytes;
    if (v >= tb)      snprintf(out, cap, "%.2f TB", v / tb);
    else if (v >= gb) snprintf(out, cap, "%.2f GB", v / gb);
    else if (v >= mb) snprintf(out, cap, "%.1f MB", v / mb);
    else if (v >= kb) snprintf(out, cap, "%.0f KB", v / kb);
    else              snprintf(out, cap, "%lld B", (long long)bytes);
}

void Net_FormatRate(int64_t bytesPerSec, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (bytesPerSec < 0) { snprintf(out, cap, "n/a"); return; }
    if (bytesPerSec == 0) { snprintf(out, cap, "0 B/s"); return; }
    char tmp[32];
    Net_FormatBytes(bytesPerSec, tmp, sizeof(tmp));
    snprintf(out, cap, "%s/s", tmp);
}
