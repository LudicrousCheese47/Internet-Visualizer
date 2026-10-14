/* ConnectionArch.c - lifecycle and endpoint management for tracked connections. */
#include "ConnectionArch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_nextArchId = 1;

/* -------------------------------------------------------------- helpers --- */

static void AssignEndpointsFromKey(ConnectionArch *arch)
{
    char ipStr[NET_STR_IP];

    NetAddr_ToString(&arch->key.localAddr, ipStr, sizeof(ipStr));
    if (arch->key.localAddr.kind != NET_ADDR_NONE) {
        NetEndpoint_SetAddress(&arch->local, ipStr, arch->key.localPort);
    } else {
        NetEndpoint_SetAddress(&arch->local, "", arch->key.localPort);
    }
    arch->local.isLocal = true;
    arch->local.geoStatus = GEO_NONE;

    NetAddr_ToString(&arch->key.remoteAddr, ipStr, sizeof(ipStr));
    if (arch->key.remoteAddr.kind != NET_ADDR_NONE && !NetAddr_IsZero(&arch->key.remoteAddr)) {
        NetEndpoint_SetAddress(&arch->remote, ipStr, arch->key.remotePort);
    } else {
        NetEndpoint_SetAddress(&arch->remote, "", arch->key.remotePort);
    }
    arch->remote.isLocal = false;

    /* Classify the remote side immediately: private/loopback/link-local
     * addresses are never sent to the geolocation provider.                    */
    if (arch->remote.ip[0] == '\0') {
        arch->remote.geoStatus = GEO_NOT_APPLICABLE;
    } else if (!arch->remote.isPublic) {
        NetAddr tmp;
        if (NetAddr_FromString(arch->remote.ip, &tmp)) {
            switch (NetAddr_Classify(&tmp)) {
                case NET_ADDR_CLASS_LOOPBACK:   arch->remote.geoStatus = GEO_LOOPBACK;    break;
                case NET_ADDR_CLASS_PRIVATE:    arch->remote.geoStatus = GEO_PRIVATE;     break;
                case NET_ADDR_CLASS_LINK_LOCAL: arch->remote.geoStatus = GEO_LINK_LOCAL;  break;
                case NET_ADDR_CLASS_MULTICAST:  arch->remote.geoStatus = GEO_MULTICAST;   break;
                case NET_ADDR_CLASS_RESERVED:   arch->remote.geoStatus = GEO_RESERVED;    break;
                default:                        arch->remote.geoStatus = GEO_NONE;        break;
            }
        }
    } else {
        arch->remote.geoStatus = GEO_PENDING;
        arch->remoteGeoPending = true;
    }
}

/* ------------------------------------------------------------- lifecycle --- */

ConnectionArch *ConnectionArch_Create(const NetConnection *conn, double now)
{
    if (!conn) return NULL;
    ConnectionArch *arch = (ConnectionArch *)calloc(1, sizeof(ConnectionArch));
    if (!arch) return NULL;

    arch->id = g_nextArchId++;
    arch->key = conn->key;
    arch->protocol = conn->key.protocol;
    arch->direction = conn->direction;
    arch->state = conn->state;
    arch->startTime = now;
    arch->lastActivity = now;
    arch->active = true;
    arch->pid = conn->pid;
    arch->processStartTime = conn->processStartTime;
    snprintf(arch->processName, sizeof(arch->processName), "%s", conn->processName);

    arch->bytesReceived = conn->countersValid ? (int64_t)conn->bytesReceived
                                               : NET_BYTES_UNAVAILABLE;
    arch->bytesTransmitted = conn->countersValid ? (int64_t)conn->bytesSent
                                                  : NET_BYTES_UNAVAILABLE;
    arch->rateBytesPerSec = -1.0;
    arch->hasRate = false;

    AssignEndpointsFromKey(arch);

    /* Rendering defaults: curvature varies per arch so overlapping arcs stay
     * readable; colour follows the direction once it is known.                */
    arch->visible = true;
    arch->curvature = 0.25f + (float)((arch->id * 2654435761ULL) % 1000) / 1000.0f * 0.35f;
    arch->thickness = 2.0f;
    arch->alpha = 0.75f;
    arch->colorR = 90; arch->colorG = 210; arch->colorB = 140;
    arch->animPhase = (double)(arch->id % 997) / 997.0;

    return arch;
}

void ConnectionArch_Update(ConnectionArch *arch, const NetConnection *conn, double now)
{
    if (!arch || !conn) return;

    /* The key identifies the connection; update everything that can change:
     * refined direction, state, owning process. Endpoints keep their cached
     * geolocation - they cannot change for a given key.                       */
    double elapsed = now - arch->lastActivity;
    int64_t oldReceived = arch->bytesReceived;
    int64_t oldTransmitted = arch->bytesTransmitted;
    if (conn->direction != NET_DIR_UNKNOWN && conn->direction != arch->direction) {
        arch->direction = conn->direction;
    }
    arch->state = conn->state;
    arch->lastActivity = now;
    arch->active = true;
    if (conn->pid != 0) arch->pid = conn->pid;
    arch->processStartTime = conn->processStartTime;
    if (conn->processName[0] != '\0') {
        snprintf(arch->processName, sizeof(arch->processName), "%s", conn->processName);
    }
    if (conn->countersValid) {
        arch->bytesReceived = (int64_t)conn->bytesReceived;
        arch->bytesTransmitted = (int64_t)conn->bytesSent;
        if (oldReceived >= 0 && oldTransmitted >= 0 && elapsed > 0.0 &&
            arch->bytesReceived >= oldReceived && arch->bytesTransmitted >= oldTransmitted) {
            arch->rateBytesPerSec = ((double)(arch->bytesReceived - oldReceived) +
                                     (double)(arch->bytesTransmitted - oldTransmitted)) / elapsed;
            arch->hasRate = true;
        } else {
            arch->hasRate = false;
        }
    } else {
        arch->bytesReceived = NET_BYTES_UNAVAILABLE;
        arch->bytesTransmitted = NET_BYTES_UNAVAILABLE;
        arch->rateBytesPerSec = -1.0;
        arch->hasRate = false;
    }

    /* If the remote side just learned it is not public, drop any pending geo
     * expectation (cannot happen for a stable key, kept for safety).          */
    if (arch->remote.ip[0] == '\0' && arch->remote.geoStatus == GEO_PENDING) {
        arch->remote.geoStatus = GEO_NOT_APPLICABLE;
        arch->remoteGeoPending = false;
    }
}

void ConnectionArch_Destroy(ConnectionArch *arch)
{
    free(arch);
}

/* ------------------------------------------------------------- endpoints -- */

void ConnectionArch_SetSender(ConnectionArch *arch, const char *ip, uint16_t port)
{
    if (!arch) return;
    ConnectionEndpoint *e = ConnectionArch_SenderMutable(arch);
    bool wasLocal = e->isLocal;
    NetEndpoint_SetAddress(e, ip, port);
    e->isLocal = wasLocal;
}

void ConnectionArch_SetReceiver(ConnectionArch *arch, const char *ip, uint16_t port)
{
    if (!arch) return;
    ConnectionEndpoint *e = ConnectionArch_ReceiverMutable(arch);
    bool wasLocal = e->isLocal;
    NetEndpoint_SetAddress(e, ip, port);
    e->isLocal = wasLocal;
}

static void ApplyGeoToEndpoint(ConnectionEndpoint *e, const GeoInfo *geo)
{
    e->geoStatus = geo->status;
    e->hasGeo = (geo->status == GEO_OK && geo->hasCoords);
    /* The provider's network domain is the closest thing to a "website name"
     * we can recover for a shared/CDN address; keep it even without coords.  */
    snprintf(e->domain, sizeof(e->domain), "%s", geo->domain);
    if (e->hasGeo) {
        e->longitude = geo->longitude;
        e->latitude = geo->latitude;
        snprintf(e->country, sizeof(e->country), "%s", geo->country);
        snprintf(e->countryCode, sizeof(e->countryCode), "%s", geo->countryCode);
        snprintf(e->region, sizeof(e->region), "%s", geo->region);
        snprintf(e->city, sizeof(e->city), "%s", geo->city);
        snprintf(e->timezone, sizeof(e->timezone), "%s", geo->timezone);
        snprintf(e->asn, sizeof(e->asn), "%s", geo->asn);
        snprintf(e->isp, sizeof(e->isp), "%s", geo->isp);
    }
}

void ConnectionArch_SetGeolocation(ConnectionArch *arch, bool senderSide, const GeoInfo *geo)
{
    if (!arch || !geo) return;
    ConnectionEndpoint *e = senderSide ? ConnectionArch_SenderMutable(arch)
                                       : ConnectionArch_ReceiverMutable(arch);
    ApplyGeoToEndpoint(e, geo);
    if (senderSide == false) arch->remoteGeoPending = false;
}

void ConnectionArch_ApplyRemoteGeo(ConnectionArch *arch, const GeoInfo *geo)
{
    if (!arch || !geo) return;
    ApplyGeoToEndpoint(&arch->remote, geo);
    arch->remoteGeoPending = false;
}

void ConnectionArch_ApplyRemoteHostname(ConnectionArch *arch, const char *host)
{
    if (!arch) return;
    snprintf(arch->remote.hostname, sizeof(arch->remote.hostname), "%s", host ? host : "");
    arch->remote.hostnameResolved = (host && host[0] != '\0');
    arch->remote.hostnamePending = false;
    arch->remoteHostnamePending = false;
}

void ConnectionArch_SetHostname(ConnectionArch *arch, bool senderSide, const char *host)
{
    if (!arch) return;
    ConnectionEndpoint *e = senderSide ? ConnectionArch_SenderMutable(arch)
                                       : ConnectionArch_ReceiverMutable(arch);
    snprintf(e->hostname, sizeof(e->hostname), "%s", host ? host : "");
    e->hostnameResolved = (host && host[0] != '\0');
    e->hostnamePending = false;
    if (senderSide == false) arch->remoteHostnamePending = false;
}

void ConnectionArch_SetLocalGeo(ConnectionArch *arch, const GeoInfo *self)
{
    if (!arch || !self) return;
    if (self->status != GEO_OK || !self->hasCoords) return;

    /* The local endpoint has a private address; its position is approximated
     * from the geolocation of the public egress (NAT) address. The endpoint
     * keeps isPublic == false so the UI can explain this.                     */
    ApplyGeoToEndpoint(&arch->local, self);
}

const ConnectionEndpoint *ConnectionArch_Sender(const ConnectionArch *arch)
{
    if (!arch) return NULL;
    return (arch->direction == NET_DIR_INBOUND) ? &arch->remote : &arch->local;
}

const ConnectionEndpoint *ConnectionArch_Receiver(const ConnectionArch *arch)
{
    if (!arch) return NULL;
    return (arch->direction == NET_DIR_INBOUND) ? &arch->local : &arch->remote;
}

ConnectionEndpoint *ConnectionArch_SenderMutable(ConnectionArch *arch)
{
    if (!arch) return NULL;
    return (arch->direction == NET_DIR_INBOUND) ? &arch->remote : &arch->local;
}

ConnectionEndpoint *ConnectionArch_ReceiverMutable(ConnectionArch *arch)
{
    if (!arch) return NULL;
    return (arch->direction == NET_DIR_INBOUND) ? &arch->local : &arch->remote;
}

const ConnectionEndpoint *ConnectionArch_Remote(const ConnectionArch *arch)
{
    return arch ? &arch->remote : NULL;
}

const ConnectionEndpoint *ConnectionArch_Local(const ConnectionArch *arch)
{
    return arch ? &arch->local : NULL;
}

bool ConnectionArch_HasGeographicArc(const ConnectionArch *arch)
{
    if (!arch) return false;
    if (arch->direction == NET_DIR_LISTENER) return false;
    if (arch->remote.ip[0] == '\0') return false;
    return arch->local.hasGeo && arch->remote.hasGeo;
}

bool ConnectionArch_IsPending(const ConnectionArch *arch)
{
    if (!arch) return false;
    return arch->remoteGeoPending || arch->remoteHostnamePending;
}

bool ConnectionArch_IsValid(const ConnectionArch *arch)
{
    if (!arch) return false;
    if (arch->protocol == NET_PROTO_UNKNOWN) return false;
    if (arch->local.ip[0] == '\0' && arch->remote.ip[0] == '\0') return false;
    return true;
}

void ConnectionArch_FormatSummary(const ConnectionArch *arch, char *out, size_t cap)
{
    if (!arch || !out || cap == 0) return;
    const ConnectionEndpoint *s = ConnectionArch_Sender(arch);
    const ConnectionEndpoint *r = ConnectionArch_Receiver(arch);
    snprintf(out, cap, "%s %s  %s:%u -> %s:%u  (%s, %s)",
             NetProtocolName(arch->protocol),
             arch->processName[0] ? arch->processName : "pid:-",
             s->ip[0] ? s->ip : "-", s->port,
             r->ip[0] ? r->ip : "-", r->port,
             NetDirectionName(arch->direction),
             NetConnStateName(arch->state));
}
