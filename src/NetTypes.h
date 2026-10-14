/*
 * NetTypes.h - Shared data types for the Internet Visualizer networking stack.
 *
 * Pure C data structures shared by the network monitor, connection tracker,
 * geolocation subsystem and the Raylib renderers. This header must stay free of
 * platform and raylib dependencies so it can be included from both C and C++.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- limits -- */

#define NET_STR_IP      46          /* "ffff:ffff:...": enough for IPv6 text  */
#define NET_STR_HOST    256         /* reverse DNS names are bounded          */
#define NET_STR_NAME    128         /* adapter friendly names / descriptions  */
#define NET_STR_SMALL   64
#define NET_STR_STATUS  224

#define NET_MAX_IFACES  24
#define NET_MAX_CONNS   512
#define NET_MAX_DNS     4

/* Per-connection byte counters are NOT exposed by the Windows IP Helper API.
 * They are reported as NET_BYTES_UNAVAILABLE (-1) so that "unknown" can never
 * be confused with a real 0. See README.md ("Limitations").                     */
#define NET_BYTES_UNAVAILABLE   ((int64_t)-1)

/* ------------------------------------------------------------- protocols -- */

typedef enum NetProtocol {
    NET_PROTO_UNKNOWN = 0,
    NET_PROTO_TCP,
    NET_PROTO_UDP
} NetProtocol;

typedef enum NetDirection {
    NET_DIR_UNKNOWN = 0,        /* could not be determined from the OS data   */
    NET_DIR_OUTBOUND,           /* local endpoint initiated the connection    */
    NET_DIR_INBOUND,            /* remote peer initiated the connection       */
    NET_DIR_LISTENER            /* listening socket (not an established flow) */
} NetDirection;

typedef enum NetConnState {
    NET_CONN_UNKNOWN = 0,
    NET_CONN_LISTEN,
    NET_CONN_ESTABLISHED,
    NET_CONN_SYN_SENT,
    NET_CONN_SYN_RECEIVED,
    NET_CONN_FIN_WAIT1,
    NET_CONN_FIN_WAIT2,
    NET_CONN_CLOSE_WAIT,
    NET_CONN_CLOSING,
    NET_CONN_LAST_ACK,
    NET_CONN_TIME_WAIT,
    NET_CONN_CLOSED,
    NET_CONN_BOUND               /* UDP: bound but no peer known              */
} NetConnState;

/* -------------------------------------------------------------- addresses -- */

typedef enum NetAddrKind {
    NET_ADDR_NONE = 0,
    NET_ADDR_IPV4,
    NET_ADDR_IPV6
} NetAddrKind;

typedef enum NetAddrClass {
    NET_ADDR_CLASS_NONE = 0,
    NET_ADDR_CLASS_LOOPBACK,
    NET_ADDR_CLASS_PRIVATE,      /* RFC1918 / fc00::/7                        */
    NET_ADDR_CLASS_LINK_LOCAL,
    NET_ADDR_CLASS_CGNAT,        /* RFC6598 100.64/10                         */
    NET_ADDR_CLASS_MULTICAST,
    NET_ADDR_CLASS_RESERVED,
    NET_ADDR_CLASS_PUBLIC
} NetAddrClass;

typedef struct NetAddr {
    NetAddrKind kind;
    uint8_t     bytes[16];       /* network byte order, IPv4 uses bytes[0..3] */
    uint32_t    scopeId;         /* IPv6 scope id (link-local interfaces)     */
} NetAddr;

/* --------------------------------------------------------- geolocation ---- */

typedef enum GeoStatus {
    GEO_NONE = 0,                /* nothing attempted yet                     */
    GEO_PENDING,                 /* queued on the resolver thread             */
    GEO_OK,
    GEO_PRIVATE,                 /* RFC1918 etc. - never looked up            */
    GEO_LOOPBACK,
    GEO_LINK_LOCAL,
    GEO_MULTICAST,
    GEO_RESERVED,
    GEO_NOT_APPLICABLE,          /* no address at all (e.g. UDP has no peer)  */
    GEO_RATE_LIMITED,            /* provider asked us to slow down            */
    GEO_OFFLINE,                 /* lookup failed: no connectivity            */
    GEO_FAILED                   /* lookup failed for any other reason        */
} GeoStatus;

/* Result of an IP geolocation lookup. "hasCoords" distinguishes a real
 * (0,0) result from "we do not know".                                        */
typedef struct GeoInfo {
    GeoStatus status;
    char      country[64];
    char      countryCode[8];
    char      region[64];
    char      city[64];
    char      timezone[64];
    char      asn[32];           /* "AS15169"                                 */
    char      isp[128];          /* ISP / organisation                        */
    char      domain[128];       /* network domain reported by the provider   */
    double    longitude;
    double    latitude;
    bool      hasCoords;
    double    resolvedAt;        /* monotonic seconds when the result arrived */
} GeoInfo;

/* ------------------------------------------------------------- endpoints -- */

/* One side of a connection. Holds the address data reported by the OS plus
 * everything the geolocation / resolver subsystems managed to attach later.  */
typedef struct ConnectionEndpoint {
    char       ip[NET_STR_IP];   /* textual form of the address ("", if none) */
    uint16_t   port;
    char       hostname[NET_STR_HOST];   /* reverse DNS or provider domain    */
    bool       hostnameResolved;
    bool       hostnamePending;

    /* geolocation */
    GeoStatus  geoStatus;
    bool       hasGeo;           /* longitude/latitude are valid              */
    bool       isPublic;         /* globally routable unicast address         */
    bool       isLocal;          /* this is OUR side of the connection        */
    char       country[64];
    char       countryCode[8];
    char       region[64];
    char       city[64];
    char       timezone[64];
    char       asn[32];
    char       isp[128];
    char       domain[128];      /* network/site domain from the geo provider */
    double     longitude;
    double     latitude;
} ConnectionEndpoint;

/* ----------------------------------------------------------- connections -- */

/* Stable identity of a connection. Never use the remote address alone:
 * many connections share an address and differ by protocol/port.             */
typedef struct NetConnKey {
    NetProtocol protocol;
    NetAddr     localAddr;
    uint16_t    localPort;
    NetAddr     remoteAddr;
    uint16_t    remotePort;
} NetConnKey;

typedef struct NetConnection {
    NetConnKey  key;
    uint32_t    pid;
    uint64_t    processStartTime; /* Windows process creation FILETIME, 0 if unavailable */
    char        processName[NET_STR_SMALL];
    NetDirection direction;
    NetConnState state;
    double      lastSeen;        /* monotonic seconds of the last observation */
    uint64_t    bytesReceived;   /* TCP ESTATS cumulative bytes, if available */
    uint64_t    bytesSent;
    bool        countersValid;
    /* Best-effort attribution to the monitored adapter: true when this
     * connection's LOCAL address belongs to the selected interface. Windows
     * reports no per-connection interface, so this is the one honest signal
     * available (a socket bound to an interface's unicast address). Remains
     * false for sockets with an unspecified (0.0.0.0/::) local address.       */
    bool        onSelectedIface;
} NetConnection;

/* ------------------------------------------------------------- interfaces -- */

typedef struct NetInterfaceInfo {
    uint32_t    index;                     /* volatile IfIndex (changes)      */
    uint64_t    luid;                      /* stable while the adapter exists */
    char        id[NET_STR_NAME];          /* stable identifier (adapter GUID) */
    char        name[NET_STR_NAME];        /* adapter friendly name           */
    char        description[NET_STR_NAME];
    char        typeName[NET_STR_SMALL];   /* "Ethernet", "Wi-Fi", ...        */
    int         ifType;                    /* MIB_IF_TYPE_* / IF_TYPE_*       */
    bool        isUp;
    bool        isPrimary;                 /* owns the default route          */
    bool        isLoopback;
    bool        isTunnel;                  /* VPN / virtual adapter           */
    bool        isSelected;                /* currently monitored adapter     */

    NetAddr     ipv4;  int prefix4;
    NetAddr     ipv6;  int prefix6;
    NetAddr     gateway4;
    NetAddr     gateway6;
    char        dnsServers[NET_MAX_DNS][NET_STR_IP];
    int         dnsCount;

    uint64_t    bytesReceived;             /* cumulative interface counters   */
    uint64_t    bytesSent;
    int64_t     rateIn;                    /* bytes/second, -1 if unknown     */
    int64_t     rateOut;
    bool        countersValid;
} NetInterfaceInfo;

/* A full observation of the machine's networking, produced by the monitor
 * worker thread and consumed (copied) by the render thread.                   */
typedef struct NetSnapshot {
    double             timestamp;
    NetInterfaceInfo   interfaces[NET_MAX_IFACES];
    int                interfaceCount;
    int                primaryInterface;   /* index into interfaces[] or -1   */

    /* The adapter the user asked to monitor. selectedInterface is the index
     * used for traffic counters/rates and connection attribution, or -1 when
     * nothing is available. selectedIsSpecific is false in the default "all
     * adapters" mode (follow the primary) and when a requested adapter has
     * disappeared, so the UI never claims attribution it cannot make.         */
    int                selectedInterface;
    bool               selectedIsSpecific;

    NetConnection      connections[NET_MAX_CONNS];
    int                connectionCount;
    bool               truncated;          /* table was larger than the cap   */

    bool               ok;                 /* enumeration succeeded          */
    char               status[NET_STR_STATUS]; /* human readable status/error */
} NetSnapshot;

/* --------------------------------------------------------------- helpers -- */

uint64_t       NetAddr_Hash(const NetAddr *a);
bool           NetAddr_Equals(const NetAddr *a, const NetAddr *b);
bool           NetAddr_IsZero(const NetAddr *a);
void           NetAddr_Clear(NetAddr *a);
NetAddrClass   NetAddr_Classify(const NetAddr *a);
/* Parses textual IPv4/IPv6 into a NetAddr. Returns false when unparsable.     */
bool           NetAddr_FromString(const char *text, NetAddr *out);
/* Writes a textual representation, always NUL terminated. "none" if empty.   */
void           NetAddr_ToString(const NetAddr *a, char *buf, size_t cap);

bool           NetConnKey_Equals(const NetConnKey *a, const NetConnKey *b);
uint64_t       NetConnKey_Hash(const NetConnKey *k);

void           NetEndpoint_Clear(ConnectionEndpoint *e);
void           NetEndpoint_SetAddress(ConnectionEndpoint *e, const char *ip, uint16_t port);

const char    *NetProtocolName(NetProtocol p);
const char    *NetDirectionName(NetDirection d);
const char    *NetConnStateName(NetConnState s);
const char    *GeoStatusName(GeoStatus s);

/* Human friendly byte / rate formatting: "1.4 MB", "812 KB/s".               */
void           Net_FormatBytes(int64_t bytes, char *out, size_t cap);
void           Net_FormatRate(int64_t bytesPerSec, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
