/* ConnectionArchRenderer.c - arc/marker/list rendering for ConnectionArch. */
#include "ConnectionArchRenderer.h"

#include "UiText.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* How long (in seconds) an arc keeps fading after the OS stopped reporting
 * its connection (arch->active flipped false). The clock passed to Draw is
 * NetTime_Now(), the same clock as arch->lastActivity.                       */
#define ARCH_EXIT_FADE_SEC 0.35

/* Screen width of one full 360 deg of longitude. Arcs that take the short way
 * across the antimeridian are stored unwrapped (one endpoint shifted by
 * +/-360 deg), so they must be drawn once per 360 deg copy to appear on both
 * sides of the seam instead of running off the map edge.                     */
static float MapWorldWidth(const MapProjection *p)
{
    double span = p->lonMax - p->lonMin;
    if (span <= 1.0) return p->bounds.width;
    return (float)(p->bounds.width * 360.0 / span);
}

/* ----------------------------------------------------------------- arc ---- */

static void ComputeArchGeometry(const MapProjection *p, ConnectionArch *arch)
{
    arch->hasScreenPos = false;
    arch->arcPointCount = 0;

    const ConnectionEndpoint *s = ConnectionArch_Sender(arch);
    const ConnectionEndpoint *r = ConnectionArch_Receiver(arch);
    if (!s || !r || !s->hasGeo || !r->hasGeo) return;

    double lonA = s->longitude, latA = s->latitude;
    double lonB = r->longitude, latB = r->latitude;

    /* take the short way around the antimeridian */
    if (lonB - lonA > 180.0)       lonB -= 360.0;
    else if (lonA - lonB > 180.0)  lonB += 360.0;

    Vector2 a = MapProjection_LonLatToScreenUnwrapped(p, lonA, latA);
    Vector2 b = MapProjection_LonLatToScreenUnwrapped(p, lonB, latB);

    float dx = b.x - a.x, dy = b.y - a.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 4.0f) dist = 4.0f;

    /* quadratic bezier: control point lifted towards the north pole */
    float lift = dist * arch->curvature;
    if (lift > 220.0f) lift = 220.0f;
    Vector2 ctrl = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - lift};

    for (int i = 0; i < ARCH_ARC_POINTS; i++) {
        float t = (float)i / (float)(ARCH_ARC_POINTS - 1);
        float u = 1.0f - t;
        arch->arcX[i] = u * u * a.x + 2.0f * u * t * ctrl.x + t * t * b.x;
        arch->arcY[i] = u * u * a.y + 2.0f * u * t * ctrl.y + t * t * b.y;
    }
    arch->arcPointCount = ARCH_ARC_POINTS;
    arch->senderX = a.x;
    arch->senderY = a.y;
    arch->receiverX = b.x;
    arch->receiverY = b.y;
    arch->hasScreenPos = true;
}

void ConnectionArchRenderer_UpdateGeometry(const MapProjection *p,
                                           ConnectionArch *const *arcs, int count)
{
    if (!p || !arcs) return;
    for (int i = 0; i < count; i++) {
        if (arcs[i]) ComputeArchGeometry(p, arcs[i]);
    }
}

static void DirectionColor(const ConnectionArch *arch, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (arch->direction) {
        case NET_DIR_OUTBOUND: *r = 110; *g = 225; *b = 150; break;  /* green  */
        case NET_DIR_INBOUND:  *r = 245; *g = 175; *b =  85; break;  /* amber  */
        case NET_DIR_LISTENER: *r = 150; *g = 160; *b = 200; break;
        default:               *r = 150; *g = 185; *b = 225; break;  /* unknown*/
    }
}

/* Smooth, time-based RGB colour for map arcs: a full hue cycle at a slow,
 * deliberate rate (~14 s per cycle). Each arch gets a stable phase offset
 * from its animPhase so arcs never pulse in lockstep, and the animation is
 * a pure function of elapsed time, hence frame-rate independent. No state,
 * no allocation.                                                            */
static void AnimatedArcColor(double now, double phase, uint8_t *r, uint8_t *g, uint8_t *b)
{
    double h = now * 0.07 + phase;
    h -= floor(h);
    float sector = (float)h * 6.0f;
    int   i = (int)sector;
    float f = sector - (float)i;
    float rr = 0.0f, gg = 0.0f, bb = 0.0f;

    switch (i) {
        case 0:  rr = 1.0f; gg = f;      bb = 0.0f; break;
        case 1:  rr = 1.0f - f; gg = 1.0f; bb = 0.0f; break;
        case 2:  rr = 0.0f; gg = 1.0f; bb = f;      break;
        case 3:  rr = 0.0f; gg = 1.0f - f; bb = 1.0f; break;
        case 4:  rr = f;      gg = 0.0f; bb = 1.0f; break;
        default: rr = 1.0f; gg = 0.0f; bb = 1.0f - f; break;
    }
    /* keep the arc luminous enough to read against the dark map            */
    *r = (uint8_t)(rr * 235.0f + 20.0f);
    *g = (uint8_t)(gg * 235.0f + 20.0f);
    *b = (uint8_t)(bb * 235.0f + 20.0f);
}

static void DrawOneArc(const ConnectionArch *arch, double now,
                       bool emphasis, int totalCount, float xOffset)
{
    if (!arch->hasScreenPos || arch->arcPointCount < 2) return;

    uint8_t cr, cg, cb;
    AnimatedArcColor(now, arch->animPhase, &cr, &cg, &cb);
    if (emphasis) {
        cr = (uint8_t)(cr + (255 - cr) * 0.30f);
        cg = (uint8_t)(cg + (255 - cg) * 0.30f);
        cb = (uint8_t)(cb + (255 - cb) * 0.30f);
    }

    float thickness = arch->thickness + (emphasis ? 1.4f : 0.0f);
    float alpha = arch->alpha * (emphasis ? 1.0f : 0.62f);
    if (totalCount > 120) alpha *= 0.75f;          /* keep overlaps readable */
    if (totalCount > 300) alpha *= 0.75f;

    /* lost connection: the tracker marked it inactive on the first poll that
     * no longer saw it - fade out at once instead of drawing a ghost arc    */
    if (!arch->active) {
        double gone = now - (double)arch->lastActivity;
        if (gone >= ARCH_EXIT_FADE_SEC) return;
        if (gone < 0.0) gone = 0.0;
        alpha *= (float)(1.0 - gone / ARCH_EXIT_FADE_SEC);
        if (alpha <= 0.02f) return;
    }

    /* three layered passes: dark halo keeps every arc readable against the
     * neon green map, the coloured pass is its glow, then the sharp core   */
    for (int pass = 0; pass < 3; pass++) {
        float w;
        Color col;
        if (pass == 0) {
            w = thickness + 3.2f;
            col = (Color){2, 10, 6, (uint8_t)(115.0f * alpha)};
        } else if (pass == 1) {
            w = thickness + 1.7f;
            col = (Color){cr, cg, cb, (uint8_t)(75.0f * alpha)};
        } else {
            w = thickness;
            col = (Color){cr, cg, cb, (uint8_t)(255.0f * alpha)};
        }

        for (int i = 0; i + 1 < arch->arcPointCount; i++) {
            float tMid = ((float)i + 0.5f) / (float)(arch->arcPointCount - 1);
            float fade = 0.30f + 0.70f * sinf(3.14159265f * tMid);
            Color seg = {col.r, col.g, col.b, (uint8_t)((float)col.a * fade)};
            Vector2 a = {arch->arcX[i] + xOffset, arch->arcY[i]};
            Vector2 b = {arch->arcX[i + 1] + xOffset, arch->arcY[i + 1]};
            DrawLineEx(a, b, w, seg);
        }
    }

    /* animated cyan data packet - only when the direction is actually known */
    if (arch->direction == NET_DIR_OUTBOUND || arch->direction == NET_DIR_INBOUND) {
        double t = fmod(now * 0.42 + arch->animPhase, 1.0);
        float idx = t * (float)(arch->arcPointCount - 1);
        int i0 = (int)idx;
        int i1 = (i0 + 1 < arch->arcPointCount) ? i0 + 1 : i0;
        float f = idx - (float)i0;
        Vector2 pos = {arch->arcX[i0] * (1.0f - f) + arch->arcX[i1] * f + xOffset,
                       arch->arcY[i0] * (1.0f - f) + arch->arcY[i1] * f};

        /* sine pulse (smooth, elapsed-time driven) modulates glow + size   */
        const float TAU = 6.2831853f;
        float pulse = 0.5f + 0.5f * sinf((float)now * TAU * 0.8f +
                                        (float)(arch->animPhase * TAU));
        float a = alpha * (emphasis ? 1.0f : 0.9f);
        DrawCircleV(pos, 10.5f + 3.0f * pulse,
                    (Color){0, 255, 255, (uint8_t)((18.0f + 20.0f * pulse) * a)});
        DrawCircleV(pos, 5.6f + 1.6f * pulse,
                    (Color){0, 255, 255, (uint8_t)((55.0f + 50.0f * pulse) * a)});
        DrawCircleV(pos, 2.4f + 0.8f * pulse,
                    (Color){205, 255, 255, (uint8_t)(250.0f * a)});
    }

    /* endpoint markers follow the arc colour so they read as one object    */
    float sSize = emphasis ? 5.0f : 3.4f;
    float rSize = emphasis ? 4.2f : 2.8f;
    Vector2 sp = {arch->senderX + xOffset, arch->senderY};
    Vector2 rp = {arch->receiverX + xOffset, arch->receiverY};
    DrawCircleV(sp, sSize + 2.0f, (Color){cr, cg, cb, (uint8_t)(60.0f * alpha)});
    DrawCircleV(sp, sSize, (Color){cr, cg, cb, (uint8_t)(240.0f * alpha)});
    DrawCircleLines((int)sp.x, (int)sp.y, sSize + 2.5f, (Color){cr, cg, cb, (uint8_t)(120.0f * alpha)});
    DrawCircleV(rp, rSize + 1.6f, (Color){cr, cg, cb, (uint8_t)(50.0f * alpha)});
    DrawCircleV(rp, rSize, (Color){cr, cg, cb, (uint8_t)(220.0f * alpha)});
}

void ConnectionArchRenderer_Draw(const MapProjection *p,
                                 ConnectionArch *const *arcs, int count,
                                 double now, uint64_t hoveredId, uint64_t selectedId)
{
    if (!p || !arcs || count <= 0) return;

    const float worldW = MapWorldWidth(p);
    const float bx0 = p->bounds.x;
    const float bx1 = p->bounds.x + p->bounds.width;
    const bool clip = worldW > 0.0f;

    /* Clip arcs to the map's horizontal span so a wrapped copy (or a plain arc
     * that bulges sideways) never bleeds over the side panels or off-window.
     * Vertical overflow is left alone so tall arcs keep their current look.   */
    if (clip) BeginScissorMode((int)bx0, 0, (int)p->bounds.width, GetScreenHeight());

    /* first pass: plain arcs; second pass: emphasised ones on top */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            const ConnectionArch *arch = arcs[i];
            if (!arch || !arch->visible) continue;
            bool emphasis = (arch->id == hoveredId || arch->id == selectedId);
            if (pass == 0 && emphasis) continue;
            if (pass == 1 && !emphasis) continue;
            if (arch->arcPointCount < 2) continue;

            /* extent of the stored (unwrapped) arc */
            float minX = arch->arcX[0], maxX = arch->arcX[0];
            for (int k = 1; k < arch->arcPointCount; k++) {
                if (arch->arcX[k] < minX) minX = arch->arcX[k];
                if (arch->arcX[k] > maxX) maxX = arch->arcX[k];
            }
            /* draw every 360 deg copy that can land inside the map */
            for (int rep = -1; rep <= 1; rep++) {
                float off = (float)rep * worldW;
                if (maxX + off < bx0 || minX + off > bx1) continue;
                DrawOneArc(arch, now, emphasis, count, off);
            }
        }
    }

    if (clip) EndScissorMode();
}

/* ----------------------------------------------------------------- pick --- */

int ConnectionArchRenderer_Pick(const MapProjection *p,
                                ConnectionArch *const *arcs, int count,
                                Vector2 mouse, float maxDist)
{
    if (!arcs || count <= 0) return -1;
    float bestD = maxDist * maxDist;
    int best = -1;
    const float worldW = MapWorldWidth(p);

    for (int i = 0; i < count; i++) {
        const ConnectionArch *arch = arcs[i];
        if (!arch || !arch->visible || !arch->hasScreenPos) continue;
        if (arch->arcPointCount < 1) continue;

        /* the arc may be drawn as a wrapped 360 deg copy; test the cursor
         * against the unwrapped geometry by undoing each copy's offset       */
        float minX = arch->arcX[0], maxX = arch->arcX[0];
        for (int k = 1; k < arch->arcPointCount; k++) {
            if (arch->arcX[k] < minX) minX = arch->arcX[k];
            if (arch->arcX[k] > maxX) maxX = arch->arcX[k];
        }
        for (int rep = -1; rep <= 1; rep++) {
            float mx = mouse.x - (float)rep * worldW;
            if (mx < minX - maxDist || mx > maxX + maxDist) continue;

            for (int j = 0; j < arch->arcPointCount; j++) {
                float dx = arch->arcX[j] - mx;
                float dy = arch->arcY[j] - mouse.y;
                float d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; best = i; }
            }
            /* endpoints count too */
            float dx = arch->senderX - mx, dy = arch->senderY - mouse.y;
            float d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = i; }
            dx = arch->receiverX - mx; dy = arch->receiverY - mouse.y;
            d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = i; }
        }
    }
    return best;
}

/* ----------------------------------------------------------------- list --- */

static void TwoLineRow(float x, float y, float w, const char *line1, const char *line2,
                       Color accent, bool hovered, bool selected)
{
    Rectangle row = {x, y, w, 30.0f};
    if (selected)      DrawRectangleRounded(row, 0.35f, 3, (Color){0, 0, 105, 235});
    else if (hovered)  DrawRectangleRounded(row, 0.35f, 3, (Color){0, 0, 62, 220});
    DrawCircle((int)(x + 11), (int)(y + 10), 3.5f, accent);
    /* rows are fixed height: both lines truncate (never wrap) so rows can
     * never grow past their allotted 30 px                                */
    const float textW = w - 26.0f;
    UiText_DrawTruncated(line1, (Vector2){x + 20, y + 3}, 13.0f, 1.0f, textW,
                         (Color){225, 232, 255, 255});
    UiText_DrawTruncated(line2, (Vector2){x + 20, y + 16}, 12.0f, 1.0f, textW,
                         (Color){145, 165, 220, 255});
}

int ConnectionArchRenderer_DrawList(ConnectionArch *const *arcs, int count,
                                    Rectangle panel, int scrollIndex,
                                    uint64_t selectedId, uint64_t hoveredId)
{
    const float rowH = 32.0f;
    const float headerH = 22.0f;
    int visible = (int)((panel.height - headerH - 8.0f) / rowH);
    if (visible < 1) return -1;

    /* clip list rows to the panel so scrolled-off rows never bleed out */
    UiText_BeginClip(panel);

    char title[64];
    snprintf(title, sizeof(title), "CONNECTIONS  (%d)", count);
    UiText_DrawTruncated(title, (Vector2){panel.x + 10, panel.y + 6}, 12.5f, 1.0f,
                          panel.width - 20.0f, (Color){125, 150, 255, 255});
    DrawLineEx((Vector2){panel.x + 9.0f, panel.y + 22.0f},
               (Vector2){panel.x + panel.width - 9.0f, panel.y + 22.0f},
               1.0f, (Color){0, 0, 255, 120});

    Vector2 mouse = GetMousePosition();
    int hoveredRow = -1;

    for (int row = 0; row < visible; row++) {
        int idx = scrollIndex + row;
        if (idx >= count) break;
        const ConnectionArch *arch = arcs[idx];
        if (!arch) continue;

        float x = panel.x + 8.0f;
        float y = panel.y + headerH + row * rowH;
        float w = panel.width - 16.0f;
        Rectangle hit = {x, y, w, rowH};
        bool hot = CheckCollisionPointRec(mouse, hit);
        if (hot) hoveredRow = idx;

        uint8_t cr, cg, cb;
        DirectionColor(arch, &cr, &cg, &cb);
        Color accent = {cr, cg, cb, 255};

        const ConnectionEndpoint *rem = ConnectionArch_Remote(arch);
        char l1[128], l2[128];
        const char *target = (rem && rem->ip[0]) ? rem->ip : "(no remote endpoint)";
        if (rem && rem->hasGeo) {
            snprintf(l2, sizeof(l2), "%s:%u  %s", target, rem->port,
                     rem->countryCode[0] ? rem->countryCode : rem->country);
        } else {
            snprintf(l2, sizeof(l2), "%s:%u", target, rem ? rem->port : 0);
        }
        /* Label the row with a human "website" name when we have one: the
         * provider's network domain first (e.g. "youtube.com"), then a
         * reverse-DNS hostname. The exact requested host is not recoverable for
         * shared CDN IPs; only fall back to the owning process when nothing
         * better is known. */
        const char *site = NULL;
        if (rem) {
            if (rem->domain[0])        site = rem->domain;
            else if (rem->hostname[0]) site = rem->hostname;
        }
        if (site) {
            snprintf(l1, sizeof(l1), "%s %s", NetProtocolName(arch->protocol), site);
        } else {
            snprintf(l1, sizeof(l1), "%s %s", NetProtocolName(arch->protocol),
                     arch->processName[0] ? arch->processName : "pid:-");
        }

        TwoLineRow(x, y, w, l1, l2, accent, hot, arch->id == selectedId);
        (void)hoveredId;
    }

    if (count > visible) {
        char more[32];
        snprintf(more, sizeof(more), "+%d more (wheel)", count - visible);
        UiText_Draw(more, (Vector2){panel.x + 10, panel.y + panel.height - 17}, 11.5f, 1.0f,
                     (Color){120, 140, 215, 255});
    }
    UiText_EndClip();
    return hoveredRow;
}

/* -------------------------------------------------------------- details --- */

/* One label+value row inside the detail card. The value is truncated (never
 * wrapped) so a row is always exactly one line tall and labels/values never
 * collide; the panel itself clips anything that would still overflow.       */
static void DetailRow(float x, float y, float labelW, float valueW,
                      const char *label, const char *value, Color valueColor)
{
    if (label && label[0]) {
        UiText_DrawTruncated(label, (Vector2){x, y}, 11.5f, 1.0f, labelW - 4.0f,
                              (Color){130, 150, 220, 255});
    }
    UiText_DrawTruncated(value, (Vector2){x + labelW, y}, 11.5f, 1.0f, valueW,
                          valueColor);
}

void ConnectionArchRenderer_DrawDetails(const ConnectionArch *arch, Rectangle panel,
                                        float scrollY)
{
    /* clip everything to the panel so nothing can bleed outside its bounds */
    UiText_BeginClip(panel);

    float x = panel.x + 10.0f;
    float y = panel.y + 26.0f - scrollY;
    const float labelW = 78.0f;
    const float valueW = panel.width - 20.0f - labelW - 6.0f;
    const float step = 15.0f;
    Color valueCol = (Color){222, 232, 255, 255};
    char tmp[192];

    if (!arch) {
        UiText_Draw("Select a connection", (Vector2){x, panel.y + 26}, 13.0f, 1.0f,
                    (Color){145, 165, 220, 255});
        UiText_Draw("Hover an arc on the map or a", (Vector2){x, panel.y + 46}, 11.5f, 1.0f,
                    (Color){110, 125, 185, 255});
        UiText_Draw("row in the list, then click.", (Vector2){x, panel.y + 60}, 11.5f, 1.0f,
                    (Color){110, 125, 185, 255});
        UiText_EndClip();
        return;
    }

    /* heading scrolls together with the rows so they can never overlap it.
     * Prefer the website/domain label, exactly like the list rows. */
    const char *siteName = NULL;
    {
        const ConnectionEndpoint *rem = ConnectionArch_Remote(arch);
        if (rem) {
            if (rem->domain[0])        siteName = rem->domain;
            else if (rem->hostname[0]) siteName = rem->hostname;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s - %s", NetProtocolName(arch->protocol),
             siteName ? siteName : (arch->processName[0] ? arch->processName : "unknown"));
    UiText_DrawTruncated(tmp, (Vector2){x, panel.y + 7.0f - scrollY}, 12.5f, 1.0f,
                          panel.width - 20.0f, (Color){155, 175, 255, 255});

    const ConnectionEndpoint *s = ConnectionArch_Sender(arch);
    const ConnectionEndpoint *r = ConnectionArch_Receiver(arch);

    /* sender */
    snprintf(tmp, sizeof(tmp), "%s:%u", s->ip[0] ? s->ip : "-", s->port);
    DetailRow(x, y, labelW, valueW, "sender", tmp, valueCol); y += step;
    if (s->hostname[0]) {
        DetailRow(x, y, labelW, valueW, "", s->hostname, (Color){180, 205, 190, 255}); y += step;
    }
    if (s->hasGeo) {
        snprintf(tmp, sizeof(tmp), "%s%s%s", s->city[0] ? s->city : "",
                 (s->city[0] && s->country[0]) ? ", " : "", s->country);
        DetailRow(x, y, labelW, valueW, "", tmp, (Color){180, 205, 190, 255}); y += step;
        snprintf(tmp, sizeof(tmp), "%.2f, %.2f", s->latitude, s->longitude);
        DetailRow(x, y, labelW, valueW, "", tmp, (Color){150, 175, 160, 255}); y += step;
    } else if (s->isLocal) {
        DetailRow(x, y, labelW, valueW, "", "(this machine, public egress)", (Color){150, 175, 160, 255});
        y += step;
    }
    y += 4.0f;

    /* receiver */
    snprintf(tmp, sizeof(tmp), "%s:%u", r->ip[0] ? r->ip : "-", r->port);
    DetailRow(x, y, labelW, valueW, "receiver", tmp, valueCol); y += step;
    if (r->hostname[0]) {
        DetailRow(x, y, labelW, valueW, "", r->hostname, (Color){180, 205, 190, 255}); y += step;
    }
    if (r->hasGeo) {
        snprintf(tmp, sizeof(tmp), "%s%s%s", r->city[0] ? r->city : "",
                 (r->city[0] && r->country[0]) ? ", " : "", r->country);
        DetailRow(x, y, labelW, valueW, "", tmp, (Color){180, 205, 190, 255}); y += step;
        snprintf(tmp, sizeof(tmp), "%.2f, %.2f", r->latitude, r->longitude);
        DetailRow(x, y, labelW, valueW, "", tmp, (Color){150, 175, 160, 255}); y += step;
    } else {
        DetailRow(x, y, labelW, valueW, "geo", GeoStatusName(r->geoStatus), (Color){190, 175, 140, 255});
        y += step;
    }
    if (r->isp[0]) {
        DetailRow(x, y, labelW, valueW, "", r->isp, (Color){160, 185, 170, 255}); y += step;
    }
    if (r->asn[0]) {
        DetailRow(x, y, labelW, valueW, "", r->asn, (Color){160, 185, 170, 255}); y += step;
    }
    y += 4.0f;

    DetailRow(x, y, labelW, valueW, "direction", NetDirectionName(arch->direction), valueCol); y += step;
    DetailRow(x, y, labelW, valueW, "state", NetConnStateName(arch->state), valueCol); y += step;

    snprintf(tmp, sizeof(tmp), "%.0f s", arch->lastActivity - arch->startTime);
    DetailRow(x, y, labelW, valueW, "observed", tmp, valueCol); y += step;

    char rx[32], tx[32], rate[32];
    Net_FormatBytes(arch->bytesReceived, rx, sizeof(rx));
    Net_FormatBytes(arch->bytesTransmitted, tx, sizeof(tx));
    snprintf(tmp, sizeof(tmp), "rx %s / tx %s", rx, tx);
    DetailRow(x, y, labelW, valueW, "per-conn", tmp, (Color){150, 170, 158, 255}); y += step;

    if (arch->hasRate) {
        Net_FormatRate((int64_t)arch->rateBytesPerSec, rate, sizeof(rate));
        DetailRow(x, y, labelW, valueW, "rate", rate, valueCol); y += step;
    } else {
        DetailRow(x, y, labelW, valueW, "rate", "n/a (not measurable)", (Color){150, 170, 158, 255});
        y += step;
    }
    y += 4.0f;

    snprintf(tmp, sizeof(tmp), "pid %u", arch->pid);
    DetailRow(x, y, labelW, valueW, "process", tmp, (Color){160, 185, 170, 255});

    UiText_EndClip();
}

/* Height of the detail card content (for scroll clamping). */
float ConnectionArchRenderer_DetailsContentHeight(const ConnectionArch *arch)
{
    if (!arch) return 80.0f;
    float h = 26.0f;                                   /* heading + top pad */
    const ConnectionEndpoint *s = ConnectionArch_Sender(arch);
    const ConnectionEndpoint *r = ConnectionArch_Receiver(arch);
    h += 15.0f;                                        /* sender ip        */
    if (s->hostname[0]) h += 15.0f;
    if (s->hasGeo)      h += 30.0f;
    else if (s->isLocal)h += 15.0f;
    h += 4.0f;
    h += 15.0f;                                        /* receiver ip      */
    if (r->hostname[0]) h += 15.0f;
    h += r->hasGeo ? 30.0f : 15.0f;
    if (r->isp[0]) h += 15.0f;
    if (r->asn[0]) h += 15.0f;
    h += 4.0f;
    h += 45.0f;                                        /* direction/state/observed */
    h += 30.0f;                                        /* per-conn + rate  */
    h += 4.0f;
    h += 15.0f;                                        /* process          */
    return h;
}
