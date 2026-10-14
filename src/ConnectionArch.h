/*
 * ConnectionArch.h - A tracked network connection between two endpoints.
 *
 * ConnectionArch owns two independent ConnectionEndpoint objects (local and
 * remote side) plus the connection metadata and the rendering state used by
 * the ConnectionArchRenderer. The sender/receiver view is derived from the
 * connection direction, so refining the direction later never duplicates or
 * loses endpoint data.
 *
 * Lifetime: created once when the tracker first sees the connection, updated
 * in place on every subsequent observation, destroyed when it expires.
 */
#pragma once

#include "NetTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of Bézier samples cached per arch for rendering and hit testing.
 * Kept small: hundreds of arches stay comfortably inside the frame budget.   */
#define ARCH_ARC_POINTS 48

typedef struct ConnectionArch {
    uint64_t       id;                /* unique, monotonically increasing    */
    NetConnKey     key;               /* stable OS-level identity            */

    ConnectionEndpoint local;
    ConnectionEndpoint remote;

    NetProtocol    protocol;
    NetDirection   direction;
    NetConnState   state;

    double         startTime;         /* monotonic seconds at first sighting */
    double         lastActivity;      /* monotonic seconds at last sighting  */
    bool           active;            /* seen in the latest snapshot         */

    /* Statistics. Windows' IP Helper API exposes no per-connection octet
     * counters, so these stay NET_BYTES_UNAVAILABLE (-1) unless another
     * accounting source is attached. Never fabricate values.                  */
    int64_t        bytesReceived;
    int64_t        bytesTransmitted;
    double         rateBytesPerSec;   /* < 0 when unknown                     */
    bool           hasRate;

    uint32_t       pid;
    uint64_t       processStartTime;
    char           processName[NET_STR_SMALL];

    /* resolution / geolocation progress */
    bool           remoteGeoPending;
    bool           remoteHostnamePending;

    /* ---- rendering state (owned by ConnectionArchRenderer) ---- */
    bool           visible;
    bool           hovered;
    bool           selected;
    float          curvature;         /* 0..1, arc height factor              */
    float          thickness;
    float          alpha;
    uint8_t        colorR, colorG, colorB;
    double         animPhase;         /* 0..1 offset for the travelling dot   */

    /* cached screen geometry (updated each frame by the renderer) */
    bool           hasScreenPos;
    float          senderX, senderY;
    float          receiverX, receiverY;
    float          arcX[ARCH_ARC_POINTS];
    float          arcY[ARCH_ARC_POINTS];
    int            arcPointCount;
} ConnectionArch;

/* ------------------------------------------------------------- lifecycle -- */

ConnectionArch *ConnectionArch_Create(const NetConnection *conn, double now);
void            ConnectionArch_Update(ConnectionArch *arch, const NetConnection *conn, double now);
void            ConnectionArch_Destroy(ConnectionArch *arch);

/* ------------------------------------------------------------- endpoints -- */

void                      ConnectionArch_SetSender(ConnectionArch *arch, const char *ip, uint16_t port);
void                      ConnectionArch_SetReceiver(ConnectionArch *arch, const char *ip, uint16_t port);
/* Applies a geolocation result to the given side ("senderSide" selects the
 * sender endpoint, false selects the receiver).                                */
void                      ConnectionArch_SetGeolocation(ConnectionArch *arch, bool senderSide, const GeoInfo *geo);
void                      ConnectionArch_SetHostname(ConnectionArch *arch, bool senderSide, const char *host);
/* Applies the machine's own (public IP derived) location to the local side.   */
void                      ConnectionArch_SetLocalGeo(ConnectionArch *arch, const GeoInfo *self);
/* Remote/local side helpers used when a result arrives for an address: they
 * always target the correct endpoint regardless of the current direction.     */
void                      ConnectionArch_ApplyRemoteGeo(ConnectionArch *arch, const GeoInfo *geo);
void                      ConnectionArch_ApplyRemoteHostname(ConnectionArch *arch, const char *host);

/* Sender/receiver depend on the direction; both are independent objects.       */
const ConnectionEndpoint *ConnectionArch_Sender(const ConnectionArch *arch);
const ConnectionEndpoint *ConnectionArch_Receiver(const ConnectionArch *arch);
ConnectionEndpoint       *ConnectionArch_SenderMutable(ConnectionArch *arch);
ConnectionEndpoint       *ConnectionArch_ReceiverMutable(ConnectionArch *arch);

/* True when both sides carry coordinates and an arc may be drawn.              */
bool                      ConnectionArch_HasGeographicArc(const ConnectionArch *arch);
/* True while any part of the arch is still waiting for the resolver.           */
bool                      ConnectionArch_IsPending(const ConnectionArch *arch);

bool                      ConnectionArch_IsValid(const ConnectionArch *arch);

/* Convenience accessors for the remote side regardless of direction.           */
const ConnectionEndpoint *ConnectionArch_Remote(const ConnectionArch *arch);
const ConnectionEndpoint *ConnectionArch_Local(const ConnectionArch *arch);

/* Formats a one-line summary, e.g. "TCP  chrome.exe  192.168.1.5:52344 ->
 * 142.250.72.14:443 (outbound, ESTABLISHED)".                                  */
void ConnectionArch_FormatSummary(const ConnectionArch *arch, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
