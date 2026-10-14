/*
 * Internet Visualizer - real-time geographic network connection map.
 *
 * Data flow (see README.md for the architecture diagram):
 *
 *   NetworkMonitor (worker thread)      GeoLocation (worker thread)
 *        |  NetSnapshot (mutex)              |  GeoInfo / hostname results
 *        v                                   v
 *   ConnectionTracker  ----> ConnectionArch objects <----+
 *        |                       |                       |
 *        +-----------------------+-----------------------+
 *                                |
 *                                v
 *   MapRenderer + ConnectionArchRenderer (main/Raylib thread)
 *
 * The Raylib thread owns rendering and input only; every blocking OS or
 * network operation happens on one of the two worker threads.
 */
#include <raylib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>

#include "NetTypes.h"
#include "NetPlatform.h"
#include "NetworkMonitor.h"
#include "ConnectionTracker.h"
#include "ConnectionArch.h"
#include "GeoLocation.h"
#include "MapProjection.h"
#include "MapRenderer.h"
#include "ConnectionArchRenderer.h"
#include "UiText.h"

/* ---------------------------------------------------------------- layout -- */

static constexpr int   WIN_W    = 1440;
static constexpr int   WIN_H    = 810;
static constexpr float MAP_X    = 24.0f;
static constexpr float MAP_Y    = 62.0f;
static constexpr float MAP_W    = 1080.0f;
static constexpr float MAP_H    = 600.0f;
static constexpr float LIST_X   = 1120.0f;
static constexpr float LIST_Y   = 62.0f;
static constexpr float LIST_W   = 304.0f;
static constexpr float LIST_H   = 166.0f;
static constexpr float PROCESS_Y = 236.0f;
static constexpr float PROCESS_H = 244.0f;
static constexpr float DET_Y    = 488.0f;
static constexpr float DET_H    = 174.0f;
static constexpr float STATUS_Y = 678.0f;

static constexpr int MAX_ARCS_DRAWN = 512;
static constexpr int MAX_PROCESS_ROWS = NET_MAX_CONNS;
static constexpr float PROCESS_ROW_H = 48.0f;

/* Graticule keyboard limits. lonStep/latStep hold the grid-line SPACING in
 * degrees, so a LARGER number means wider spacing and fewer lines. The "+"
 * key widens the spacing toward GRID_STEP_MAX and "-" narrows it toward
 * GRID_STEP_MIN. The range [10, 30] is the finest/coarsest step that still
 * divides 180/90 evenly, so the 0 meridians always land on a drawn line. */
static constexpr int GRID_STEP_MIN = 10;   /* finest spacing  (most lines)   */
static constexpr int GRID_STEP_MAX = 30;   /* coarsest spacing (fewest lines) */

/* Network adapter selector (dropdown shown next to the "interface:" label in
 * the status bar). The menu opens upwards so it never leaves the window.      */
static constexpr float ADAPTER_BTN_W  = 300.0f;
static constexpr float ADAPTER_BTN_H  = 22.0f;
static constexpr float ADAPTER_MENU_W = 560.0f;
static constexpr float ADAPTER_ITEM_H = 34.0f;
static constexpr int   ADAPTER_MAX_VISIBLE = 9;

struct ProcessRow {
    uint32_t pid;
    uint64_t processStartTime;
    char name[NET_STR_SMALL];
    int connectionCount;
    int measuredConnections;
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t previousReceived;
    uint64_t previousSent;
    bool hasPreviousCounters;
    float pulse;        /* current glow intensity 0..1 (smoothed, dt based) */
    float pulseHold;    /* seconds left before the glow is allowed to fade  */
    Color pulseColor;
};

static const Color PROCESS_PULSE_PALETTE[] = {
    {255, 0, 210, 255}, {0, 255, 245, 255}, {255, 35, 35, 255},
    {255, 120, 0, 255}, {165, 35, 255, 255}, {255, 235, 0, 255},
    {35, 255, 75, 255}
};

/* Pulse timing. Activity holds the glow at full brightness for a moment, then
 * the intensity eases back to zero. Rates are per second and applied with the
 * frame delta, so the animation is identical at any frame rate and a repeat
 * activity event simply reverses the fade instead of snapping the glow.     */
static constexpr float PULSE_HOLD_SEC  = 0.45f;   /* full-brightness hold     */
static constexpr float PULSE_ATTACK    = 7.0f;    /* intensity units / second */
static constexpr float PULSE_RELEASE   = 1.3f;    /* fade-out speed           */
static constexpr float PULSE_RECOLOR   = 0.45f;   /* below this: pick a new
                                                      colour on retrigger     */
static constexpr uint64_t PULSE_ACTIVITY_BYTES = 128;  /* per poll, per dir  */

/* --------------------------------------------------------------- helpers -- */

struct SortedIndex {
    ConnectionArch **items;
    int count;
};

static bool SameProcess(const ProcessRow &a, const ProcessRow &b)
{
    return a.pid == b.pid && a.processStartTime == b.processStartTime;
}

static uint64_t SaturatingAdd(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static void UpdateProcessRows(const NetSnapshot &snapshot, ProcessRow *rows, int *rowCount)
{
    ProcessRow current[MAX_PROCESS_ROWS] = {};
    int currentCount = 0;

    for (int i = 0; i < snapshot.connectionCount; i++) {
        const NetConnection &conn = snapshot.connections[i];
        ProcessRow key{};
        key.pid = conn.pid;
        key.processStartTime = conn.processStartTime;
        int index = 0;
        while (index < currentCount && !SameProcess(current[index], key)) index++;
        if (index == currentCount) {
            if (currentCount >= MAX_PROCESS_ROWS) continue;
            current[index] = key;
            snprintf(current[index].name, sizeof(current[index].name), "%s",
                     conn.processName[0] ? conn.processName : "unknown");
            currentCount++;
        }
        ProcessRow &row = current[index];
        row.connectionCount++;
        if (conn.countersValid) {
            row.measuredConnections++;
            row.bytesReceived = SaturatingAdd(row.bytesReceived, conn.bytesReceived);
            row.bytesSent = SaturatingAdd(row.bytesSent, conn.bytesSent);
        }
    }

    for (int i = 0; i < currentCount; i++) {
        ProcessRow &row = current[i];
        int previousIndex = 0;
        while (previousIndex < *rowCount && !SameProcess(row, rows[previousIndex])) previousIndex++;
        if (previousIndex < *rowCount) {
            const ProcessRow &old = rows[previousIndex];
            row.pulse = old.pulse;
            row.pulseHold = old.pulseHold;
            row.pulseColor = old.pulseColor;
            row.previousReceived = old.previousReceived;
            row.previousSent = old.previousSent;
            row.hasPreviousCounters = old.hasPreviousCounters;
            /* Two independent, real activity signals for this process:
             *  1. TCP byte counters (ESTATS): exact per-connection traffic,
             *     but Windows exposes them only to elevated callers, so they
             *     are usually unavailable on a normal run.
             *  2. Connection churn: the process gained TCP/UDP endpoints since
             *     the previous poll. Opening a socket is real network work (not
             *     a fabricated timer) and this signal works without elevation.
             * Either signal (re)arms the glow; neither fires while a connection
             * merely sits open and idle. */
            bool byteActive = false;
            if (row.measuredConnections > 0) {
                if (row.hasPreviousCounters && row.bytesReceived >= row.previousReceived &&
                    row.bytesSent >= row.previousSent) {
                    const uint64_t rxDelta = row.bytesReceived - row.previousReceived;
                    const uint64_t txDelta = row.bytesSent - row.previousSent;
                    byteActive = (rxDelta >= PULSE_ACTIVITY_BYTES ||
                                  txDelta >= PULSE_ACTIVITY_BYTES);
                }
                row.previousReceived = row.bytesReceived;
                row.previousSent = row.bytesSent;
                row.hasPreviousCounters = true;
            }
            const bool connActive = (row.connectionCount > old.connectionCount);

            /* PID 0 owns the kernel's TIME_WAIT/closed sockets; those churn
             * constantly and would glow the "System Idle" row for no reason. */
            if (row.pid != 0 && (byteActive || connActive)) {
                /* A fresh burst while still bright keeps its colour (no pop);
                 * a burst from a dim/faded row re-picks a random saturated
                 * colour and re-arms the pulse. */
                if (row.pulse < PULSE_RECOLOR) {
                    int choice = GetRandomValue(0, (int)(sizeof(PROCESS_PULSE_PALETTE) /
                                                         sizeof(PROCESS_PULSE_PALETTE[0])) - 1);
                    row.pulseColor = PROCESS_PULSE_PALETTE[choice];
                }
                row.pulseHold = PULSE_HOLD_SEC;
            }
        } else if (row.measuredConnections > 0) {
            row.previousReceived = row.bytesReceived;
            row.previousSent = row.bytesSent;
            row.hasPreviousCounters = true;
        }
    }

    for (int i = 0; i < currentCount; i++) {
        if (current[i].measuredConnections == 0) {
            current[i].hasPreviousCounters = false;
            current[i].previousReceived = 0;
            current[i].previousSent = 0;
        }
    }

    memcpy(rows, current, sizeof(ProcessRow) * (size_t)currentCount);
    *rowCount = currentCount;
}

static int CompareProcessRows(const void *a, const void *b)
{
    const ProcessRow *x = (const ProcessRow *)a;
    const ProcessRow *y = (const ProcessRow *)b;
    /* Deliberately ignores the pulse state: sorting pulsing rows to the top
     * would make rows jump around while the user watches the animation.    */
    if (x->connectionCount != y->connectionCount)
        return x->connectionCount > y->connectionCount ? -1 : 1;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    if (x->processStartTime != y->processStartTime)
        return x->processStartTime < y->processStartTime ? -1 : 1;
    return 0;
}

static int PickConnectionListRow(Rectangle panel, int count, int scrollIndex, Vector2 mouse)
{
    const float headerH = 22.0f;
    const float rowH = 32.0f;
    const int visible = (int)((panel.height - headerH - 8.0f) / rowH);
    float x = panel.x + 8.0f;
    float width = panel.width - 16.0f;
    if (!CheckCollisionPointRec(mouse, {x, panel.y + headerH, width,
                                       visible * rowH})) return -1;
    int row = (int)((mouse.y - (panel.y + headerH)) / rowH);
    int index = scrollIndex + row;
    return index < count ? index : -1;
}

/* Brightens a text colour towards white so labels stay readable on top of a
 * pulsing (saturated) row background. t = 0 -> original colour, 1 -> white.  */
static Color LiftTowardWhite(Color c, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return Color{(unsigned char)((float)c.r + (255.0f - (float)c.r) * t),
                 (unsigned char)((float)c.g + (255.0f - (float)c.g) * t),
                 (unsigned char)((float)c.b + (255.0f - (float)c.b) * t),
                 c.a};
}

static int PickProcessPanelRow(Rectangle panel, int count, int scrollIndex, Vector2 mouse)
{
    const float top = panel.y + 32.0f;
    const int visible = (int)((panel.y + panel.height - top - 5.0f) / PROCESS_ROW_H);
    if (!CheckCollisionPointRec(mouse, {panel.x + 2.0f, top, panel.width - 4.0f,
                                       visible * PROCESS_ROW_H})) return -1;
    int index = scrollIndex + (int)((mouse.y - top) / PROCESS_ROW_H);
    return index < count ? index : -1;
}

static void DrawProcessPanel(const ProcessRow *rows, int count, Rectangle panel,
                             int scrollIndex, int hoveredIndex)
{
    const Color blue = {0, 0, 255, 255};
    DrawRectangleRounded(panel, 0.04f, 6, Color{7, 10, 27, 255});
    DrawRectangleRoundedLines(panel, 0.04f, 6, Color{0, 0, 255, 190});

    char title[64];
    snprintf(title, sizeof(title), "ACTIVE PROCESSES  (%d)", count);
    UiText_DrawTruncated(title, {panel.x + 10.0f, panel.y + 7.0f},
                         12.5f, 1.0f, panel.width - 20.0f, {125, 150, 255, 255});
    DrawLineEx({panel.x + 9.0f, panel.y + 27.0f},
               {panel.x + panel.width - 9.0f, panel.y + 27.0f},
               1.0f, {0, 0, 255, 120});

    const float top = panel.y + 32.0f;
    const int visible = (int)((panel.y + panel.height - top - 5.0f) / PROCESS_ROW_H);
    UiText_BeginClip({panel.x + 2.0f, top, panel.width - 4.0f, panel.y + panel.height - top - 3.0f});
    for (int line = 0; line < visible; line++) {
        int index = scrollIndex + line;
        if (index >= count) break;
        const ProcessRow &row = rows[index];
        float x = panel.x + 7.0f;
        float y = top + line * PROCESS_ROW_H;
        float width = panel.width - 14.0f;
        Rectangle card = {x, y + 1.0f, width, PROCESS_ROW_H - 3.0f};

        /* per-process activity glow: each row animates on its own, colours
         * come from PROCESS_PULSE_PALETTE and the intensity is already
         * eased by the delta-time update in the main loop                   */
        float glow = row.pulse * row.pulse;
        if (glow > 0.01f) {
            Color halo = row.pulseColor;
            halo.a = (unsigned char)(16.0f + 58.0f * glow);
            DrawRectangleRounded({x - 3.0f, y - 1.0f, width + 6.0f, PROCESS_ROW_H + 1.0f},
                                 0.12f, 4, halo);
            Color wash = row.pulseColor;
            wash.a = (unsigned char)(26.0f * glow);
            DrawRectangleRounded(card, 0.12f, 4, wash);
            Color edge = row.pulseColor;
            edge.a = (unsigned char)(55.0f + 150.0f * glow);
            DrawRectangleRoundedLines(card, 0.12f, 4, edge);
        } else if (index == hoveredIndex) {
            DrawRectangleRounded(card, 0.12f, 4, Color{0, 0, 255, 34});
        }

        char pid[32];
        snprintf(pid, sizeof(pid), "PID %u", row.pid);
        /* brighten the labels while the row glows so they stay readable
         * against the saturated pulse colour                               */
        const float lit = glow * 0.9f;
        UiText_DrawTruncated(row.name[0] ? row.name : "unknown", {x + 7.0f, y + 4.0f},
                             12.5f, 1.0f, width - 82.0f,
                             LiftTowardWhite({225, 232, 255, 255}, lit));
        float pidWidth = UiText_Width(pid, 10.5f, 1.0f);
        UiText_Draw(pid, {x + width - pidWidth - 6.0f, y + 6.0f},
                    10.5f, 1.0f, LiftTowardWhite({120, 140, 220, 255}, lit));

        char traffic[160];
        if (row.measuredConnections > 0) {
            char rx[32], tx[32];
            int64_t received = row.bytesReceived > (uint64_t)INT64_MAX
                                   ? INT64_MAX : (int64_t)row.bytesReceived;
            int64_t sent = row.bytesSent > (uint64_t)INT64_MAX
                               ? INT64_MAX : (int64_t)row.bytesSent;
            Net_FormatBytes(received, rx, sizeof(rx));
            Net_FormatBytes(sent, tx, sizeof(tx));
            snprintf(traffic, sizeof(traffic), "%dc  TCP %d/%d  RX %s  TX %s",
                     row.connectionCount, row.measuredConnections, row.connectionCount, rx, tx);
        } else {
            snprintf(traffic, sizeof(traffic),
                     "%d connections  byte counters n/a (run as admin)",
                     row.connectionCount);
        }
        UiText_DrawTruncated(traffic, {x + 7.0f, y + 24.0f}, 10.5f, 1.0f,
                             width - 14.0f, LiftTowardWhite({145, 165, 215, 255}, lit));
    }
    UiText_EndClip();

    int visibleCount = visible > 0 ? visible : 1;
    int maxScroll = count - visibleCount;
    if (maxScroll < 0) maxScroll = 0;
    if (count > visibleCount && maxScroll > 0) {
        float trackY = top + 2.0f;
        float trackH = panel.y + panel.height - trackY - 7.0f;
        float thumbH = trackH * (float)visibleCount / (float)count;
        if (thumbH < 16.0f) thumbH = 16.0f;
        float thumbY = trackY + (trackH - thumbH) * (float)scrollIndex / (float)maxScroll;
        DrawRectangleRounded({panel.x + panel.width - 5.0f, thumbY, 2.0f, thumbH},
                             0.5f, 2, blue);
    }
}

static int CompareArches(const void *a, const void *b)
{
    const ConnectionArch *x = *(ConnectionArch *const *)a;
    const ConnectionArch *y = *(ConnectionArch *const *)b;
    if (x->active != y->active) return x->active ? -1 : 1;
    if (x->lastActivity != y->lastActivity) return (x->lastActivity > y->lastActivity) ? -1 : 1;
    return (x->id < y->id) ? -1 : ((x->id > y->id) ? 1 : 0);
}

/* --------------------------------------------- adapter selector (dropdown -- */

struct AdapterDropdown {
    bool open = false;
    int  hover = -1;
    int  scroll = 0;
};

/* Button immediately right of the "interface:" label in the status bar.       */
static Rectangle AdapterButtonRect()
{
    const float labelW = UiText_Width("interface:", 13.5f, 1.0f);
    return Rectangle{MAP_X + 14.0f + labelW + 8.0f, STATUS_Y + 6.0f,
                     ADAPTER_BTN_W, ADAPTER_BTN_H};
}

/* The popup panel, anchored to the button and opening upwards.                */
static Rectangle AdapterMenuRect(int count)
{
    Rectangle btn = AdapterButtonRect();
    int visible = count < ADAPTER_MAX_VISIBLE ? count : ADAPTER_MAX_VISIBLE;
    if (visible < 1) visible = 1;
    float h = ADAPTER_ITEM_H * (float)visible + 12.0f;
    return Rectangle{btn.x, btn.y - h, ADAPTER_MENU_W, h};
}

static int AdapterMenuHoveredIndex(const Rectangle &menu, int count, int scroll,
                                   Vector2 mouse)
{
    if (count <= 0 || !CheckCollisionPointRec(mouse, menu)) return -1;
    /* items are drawn from menu.y + 9; keep the hit rows aligned with them */
    int row = (int)((mouse.y - (menu.y + 9.0f)) / ADAPTER_ITEM_H);
    int idx = scroll + row;
    return (idx < count) ? idx : -1;
}

/* Draws the selector button and, when open, the adapter list.                 */
static void DrawAdapterSelector(const NetSnapshot &snap, const AdapterDropdown &dd,
                                Vector2 mouse)
{
    Rectangle btn = AdapterButtonRect();
    const Color colText = Color{222, 232, 255, 255};
    const Color colDim  = Color{145, 165, 225, 255};

    const NetInterfaceInfo *sel = (snap.selectedInterface >= 0 &&
                                   snap.selectedInterface < snap.interfaceCount)
        ? &snap.interfaces[snap.selectedInterface] : NULL;

    char label[NET_STR_NAME + 48];
    if (snap.selectedIsSpecific && sel) {
        snprintf(label, sizeof(label), "%s  (monitoring)", sel->name);
    } else if (sel) {
        snprintf(label, sizeof(label), "All adapters  [primary: %s]", sel->name);
    } else {
        snprintf(label, sizeof(label), "No adapter available");
    }

    DrawRectangleRounded(btn, 0.14f, 4, Color{9, 14, 36, 255});
    DrawRectangleRoundedLines(btn, 0.14f, 4, Color{0, 0, 255, 190});
    UiText_DrawTruncated(label, {btn.x + 8.0f, btn.y + 5.0f}, 11.5f, 1.0f,
                         btn.width - 44.0f, colText);

    /* chevron drawn as triangles (the monospaced font has no glyphs for
     * geometric arrows, and raylib's default font neither) */
    Vector2 c = {btn.x + btn.width - 16.0f, btn.y + 11.0f};
    if (dd.open) {
        DrawTriangle({c.x, c.y + 5.5f}, {c.x - 5.0f, c.y - 4.5f},
                     {c.x + 5.0f, c.y - 4.5f}, colDim);
    } else {
        DrawTriangle({c.x, c.y - 4.5f}, {c.x - 5.0f, c.y + 5.5f},
                     {c.x + 5.0f, c.y + 5.5f}, colDim);
    }

    if (!dd.open) return;

    Rectangle menu = AdapterMenuRect(snap.interfaceCount);
    DrawRectangleRounded(menu, 0.06f, 6, Color{8, 12, 30, 255});
    DrawRectangleRoundedLines(menu, 0.06f, 6, Color{0, 0, 255, 190});

    if (snap.interfaceCount == 0) {
        UiText_Draw("no usable network adapters found",
                    {menu.x + 12.0f, menu.y + 12.0f}, 12.5f, 1.0f,
                    Color{230, 180, 140, 255});
        return;
    }

    const int visible = snap.interfaceCount < ADAPTER_MAX_VISIBLE
        ? snap.interfaceCount : ADAPTER_MAX_VISIBLE;
    UiText_BeginClip({menu.x + 2.0f, menu.y + 2.0f, menu.width - 4.0f, menu.height - 4.0f});
    for (int v = 0; v < visible; v++) {
        int idx = dd.scroll + v;
        if (idx >= snap.interfaceCount) break;
        const NetInterfaceInfo &it = snap.interfaces[idx];
        Rectangle item = {menu.x + 3.0f, menu.y + 11.0f + v * ADAPTER_ITEM_H - 2.0f,
                          menu.width - 6.0f, ADAPTER_ITEM_H - 2.0f};
        bool hovered = ((v + dd.scroll) == dd.hover);
        bool current = it.isSelected;
        if (hovered) DrawRectangleRounded(item, 0.08f, 4, Color{16, 28, 66, 255});
        else if (current) DrawRectangleRounded(item, 0.08f, 4, Color{0, 0, 255, 60});
        if (current) DrawRectangleRoundedLines(item, 0.08f, 4, Color{0, 0, 255, 180});

        char ip[NET_STR_IP];
        NetAddr_ToString(&it.ipv4, ip, sizeof(ip));
        char l1[NET_STR_NAME + 24];
        snprintf(l1, sizeof(l1), "%s%s%s",
                 it.name[0] ? it.name : it.description,
                 it.isPrimary ? "  [primary]" : "",
                 it.isUp ? "" : "  [disconnected]");
        UiText_DrawTruncated(l1, {item.x + 8.0f, item.y + 3.0f}, 12.0f, 1.0f,
                             item.width - 16.0f, colText);
        char l2[NET_STR_NAME + NET_STR_IP + 8];
        snprintf(l2, sizeof(l2), "%s   %s", it.typeName, ip[0] ? ip : "no ip");
        UiText_DrawTruncated(l2, {item.x + 8.0f, item.y + 19.0f}, 10.5f, 1.0f,
                             item.width - 16.0f, colDim);
    }
    UiText_EndClip();

    if (snap.interfaceCount > visible) {
        float trackH = menu.height - 14.0f;
        float thumbH = trackH * (float)visible / (float)snap.interfaceCount;
        if (thumbH < 16.0f) thumbH = 16.0f;
        int maxScroll = snap.interfaceCount - visible;
        if (maxScroll < 0) maxScroll = 0;
        float thumbY = menu.y + 7.0f + (trackH - thumbH) * (float)dd.scroll / (float)maxScroll;
        DrawRectangleRounded({menu.x + menu.width - 5.0f, thumbY, 2.0f, thumbH},
                             0.5f, 2, colDim);
    }
}

/* When a specific adapter is monitored, keep only the connections that could
 * be attributed to it (local address == that adapter's unicast address). The
 * remaining connections, process rows and arcs therefore describe exactly the
 * selected interface, and the UI copies of the snapshot never lie.           */
static void ApplyIfaceFilter(NetSnapshot &snapshot)
{
    if (!snapshot.selectedIsSpecific) return;
    int w = 0;
    for (int r = 0; r < snapshot.connectionCount; r++) {
        if (!snapshot.connections[r].onSelectedIface) continue;
        if (w != r) snapshot.connections[w] = snapshot.connections[r];
        w++;
    }
    snapshot.connectionCount = w;
}

static void DrawStatusBar(const NetSnapshot *snap, GeoLocation *geo,
                          const ConnectionTracker *tracker, bool geoEnabled)
{
    Rectangle bar = {MAP_X, STATUS_Y, WIN_W - 2 * MAP_X, WIN_H - STATUS_Y - 16.0f};
    DrawRectangleRounded(bar, 0.08f, 6, Color{7, 10, 27, 255});
    DrawRectangleRoundedLines(bar, 0.08f, 6, Color{0, 0, 255, 175});

    float x = bar.x + 14.0f;
    float y = bar.y + 10.0f;
    const float maxW = bar.width - 28.0f;
    const Color label = Color{145, 165, 225, 255};
    const Color value = Color{222, 232, 255, 255};
    char line[512];

    const NetInterfaceInfo *iface = NULL;
    if (snap && snap->selectedInterface >= 0 &&
        snap->selectedInterface < snap->interfaceCount)
        iface = &snap->interfaces[snap->selectedInterface];

    if (iface) {
        char ip[NET_STR_IP], gw[NET_STR_IP], dns[256];
        NetAddr_ToString(&iface->ipv4, ip, sizeof(ip));
        NetAddr_ToString(&iface->gateway4, gw, sizeof(gw));
        dns[0] = '\0';
        for (int i = 0; i < iface->dnsCount; i++) {
            if (i) strncat(dns, ", ", sizeof(dns) - strlen(dns) - 1);
            strncat(dns, iface->dnsServers[i], sizeof(dns) - strlen(dns) - 1);
        }
        /* line 1: the "interface:" label; the selector button (drawn later by
         * DrawAdapterSelector) sits exactly between the label and this suffix */
        UiText_DrawTruncated("interface:", Vector2{x, y}, 13.5f, 1.0f, maxW, value);
        snprintf(line, sizeof(line), "(%s, %s%s)", iface->typeName,
                 iface->isUp ? "up" : "down", iface->isTunnel ? ", tunnel" : "");
        Rectangle btn = AdapterButtonRect();
        UiText_DrawTruncated(line,
                             Vector2{btn.x + btn.width + 10.0f, y}, 13.5f, 1.0f,
                             bar.x + bar.width - (btn.x + btn.width + 10.0f) - 12.0f, label);
        y += 20.0f;
        snprintf(line, sizeof(line), "ipv4: %s/%d     gateway: %s     dns: %s",
                 ip, iface->prefix4, gw[0] ? gw : "-", dns[0] ? dns : "-");
        UiText_DrawTruncated(line, Vector2{x, y}, 12.5f, 1.0f, maxW, label);
        y += 18.0f;

        char down[32], up[32], totIn[32], totOut[32];
        Net_FormatRate(iface->rateIn, down, sizeof(down));
        Net_FormatRate(iface->rateOut, up, sizeof(up));
        Net_FormatBytes((int64_t)iface->bytesReceived, totIn, sizeof(totIn));
        Net_FormatBytes((int64_t)iface->bytesSent, totOut, sizeof(totOut));
        snprintf(line, sizeof(line), "traffic: %s down / %s up     totals: %s in / %s out (interface counters)",
                 down, up, totIn, totOut);
        UiText_DrawTruncated(line, Vector2{x, y}, 12.5f, 1.0f, maxW, label);
        y += 18.0f;

        /* monitoring scope note: a specific adapter limits the observed
         * connections to those attributable to it; otherwise process activity
         * is machine-wide and never tied to a single interface               */
        if (snap->selectedIsSpecific) {
            snprintf(line, sizeof(line),
                     "adapter view: only connections whose local IP belongs to this adapter");
        } else if (snap->interfaceCount > 0) {
            snprintf(line, sizeof(line),
                     "adapter view: all %d adapters - process activity is global, not per-interface",
                     snap->interfaceCount);
        } else {
            snprintf(line, sizeof(line), "adapter view: none");
        }
        UiText_DrawTruncated(line, Vector2{x, y}, 11.5f, 1.0f, maxW, Color{150, 190, 235, 255});
    } else {
        UiText_DrawTruncated("no active network interface detected", Vector2{x, y}, 13.5f, 1.0f,
                             maxW, Color{230, 180, 140, 255});
        y += 20.0f;
    }

    /* second row: public identity + counters */
    y = bar.y + 84.0f;
    GeoInfo self;
    char selfIp[NET_STR_IP] = {0};
    if (geo && GeoLocation_GetSelf(geo, &self, selfIp, sizeof(selfIp))) {
        snprintf(line, sizeof(line), "public ip: %s  (%s%s%s)   %s", selfIp,
                 self.city, (self.city[0] && self.country[0]) ? ", " : "", self.country,
                 geoEnabled ? "geolocation: on" : "geolocation: OFF (G)");
        UiText_DrawTruncated(line, Vector2{x, y}, 12.5f, 1.0f, maxW * 0.62f, value);
    } else {
        snprintf(line, sizeof(line), "public ip: resolving...   %s",
                 geoEnabled ? "geolocation: on" : "geolocation: OFF (G)");
        UiText_DrawTruncated(line, Vector2{x, y}, 12.5f, 1.0f, maxW * 0.62f, label);
    }

    int total = ConnectionTracker_Count(tracker);
    int withArc = 0, pending = 0;
    ConnectionArch *tmp[MAX_ARCS_DRAWN];
    int n = ConnectionTracker_GetAll(tracker, tmp, MAX_ARCS_DRAWN);
    for (int i = 0; i < n; i++) {
        if (ConnectionArch_HasGeographicArc(tmp[i])) withArc++;
        else if (ConnectionArch_IsPending(tmp[i])) pending++;
    }
    char counts[256];
    snprintf(counts, sizeof(counts), "tracked: %d     geo arcs: %d     pending: %d     %s",
             total, withArc, pending, snap ? snap->status : "waiting for first poll");
    UiText_DrawTruncated(counts, Vector2{x + maxW * 0.62f, y}, 12.5f, 1.0f, maxW * 0.38f, label);
}

/* ------------------------------------------------------------------ main -- */

int main()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(WIN_W, WIN_H, "Internet Visualizer");
    SetTargetFPS(60);

    /* ---- monospaced terminal font: bundled JetBrains Mono, with graceful
     * fallbacks (system Consolas, then raylib's default) ---- */
    Font regFont = LoadFontEx("assets/JetBrainsMono-Regular.ttf", 20, NULL, 0);
    Font boldFont = LoadFontEx("assets/JetBrainsMono-Bold.ttf", 20, NULL, 0);
    bool regOk = regFont.texture.id != 0 && regFont.glyphs != NULL;
    bool boldOk = boldFont.texture.id != 0 && boldFont.glyphs != NULL;
    if (!regOk) {
        regFont = LoadFontEx("C:/Windows/Fonts/consola.ttf", 20, NULL, 0);
        regOk = regFont.texture.id != 0 && regFont.glyphs != NULL;
        if (!regOk) regFont = GetFontDefault();
    }
    if (!boldOk) boldFont = regFont;      /* fall back to regular weight      */
    SetTextureFilter(regFont.texture, TEXTURE_FILTER_BILINEAR);
    if (boldFont.texture.id != regFont.texture.id)
        SetTextureFilter(boldFont.texture, TEXTURE_FILTER_BILINEAR);
    UiText_SetFonts(regFont, boldFont);

    /* ---- map + projection (single shared transform for everything) ---- */
    MapRenderer map;
    MapRenderer_Init(&map, "assets/wm.png");

    Rectangle mapRect = {MAP_X, MAP_Y, MAP_W, MAP_H};
    MapProjection projection;
    MapProjection_InitWithDefaults(&projection, mapRect);

    /* ---- networking stack ---- */
    NetworkMonitor *monitor = NetworkMonitor_Create();
    GeoLocation    *geo     = GeoLocation_Create();
    ConnectionTracker *tracker = ConnectionTracker_Create(6.0 /* seconds */);
    if (!monitor || !geo || !tracker) {
        CloseWindow();
        return 1;
    }

    /* ~1/4 s so arcs react as soon as the OS reports a connection opening
     * or closing (the worker sleeps in small slices, so worst-case latency
     * is interval + one slice). The table/bridge cost of this rate is tiny. */
    NetworkMonitor_SetPollInterval(monitor, 0.25);
    NetworkMonitor_PollOnce(monitor);          /* first frame already has data */
    NetworkMonitor_Start(monitor);
    GeoLocation_Start(geo);

    ConnectionArch *added[NET_MAX_CONNS];
    ConnectionArch *removed[NET_MAX_CONNS];
    NetSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.primaryInterface = -1;
    double lastSnapshotTime = -1.0;

    GeoInfo selfGeo;
    memset(&selfGeo, 0, sizeof(selfGeo));
    bool selfApplied = false;

    uint64_t selectedId = 0;
    uint64_t hoveredId  = 0;
    int scrollIndex = 0;
    float detailScroll = 0.0f;
    bool geoEnabled = true;
    int lonStep = 30, latStep = 30;
    ProcessRow processRows[MAX_PROCESS_ROWS] = {};
    int processCount = 0;
    int processScrollIndex = 0;

    AdapterDropdown adapterDd;
    char lastMonitorKey[NET_STR_NAME + 16] = "";

    ConnectionArch *sortedList[MAX_ARCS_DRAWN];

    while (!WindowShouldClose()) {
        /* ---------------------------------------------------- 1. poll data */
        if (NetworkMonitor_GetSnapshot(monitor, &snapshot)) {
            if (snapshot.timestamp != lastSnapshotTime) {
                lastSnapshotTime = snapshot.timestamp;

                /* When the monitored adapter changes, the whole observed
                 * connection set changes too: drop the old adapter's arches
                 * immediately instead of letting them fade out over the
                 * tracker's expiry window.                                     */
                char monitorKey[NET_STR_NAME + 16] = "";
                if (snapshot.selectedIsSpecific && snapshot.selectedInterface >= 0) {
                    snprintf(monitorKey, sizeof(monitorKey), "sel:%s",
                             snapshot.interfaces[snapshot.selectedInterface].id);
                } else {
                    snprintf(monitorKey, sizeof(monitorKey), "all");
                }
                if (strcmp(monitorKey, lastMonitorKey) != 0) {
                    ConnectionTracker_Clear(tracker);
                    selectedId = 0;
                    hoveredId = 0;
                    snprintf(lastMonitorKey, sizeof(lastMonitorKey), "%s", monitorKey);
                }

                /* narrow the snapshot to the monitored adapter (no-op in the
                 * "all adapters" mode) - connections, process rows and arcs
                 * that reach the UI are then exactly the selected interface */
                ApplyIfaceFilter(snapshot);
                UpdateProcessRows(snapshot, processRows, &processCount);

                int addedCount = 0, removedCount = 0;
                ConnectionTracker_Update(tracker, &snapshot, added, &addedCount,
                                         removed, &removedCount, NET_MAX_CONNS);

                /* new connections: ask the resolver for their remote endpoint */
                for (int i = 0; i < addedCount; i++) {
                    const ConnectionEndpoint *rem = ConnectionArch_Remote(added[i]);
                    if (rem && rem->isPublic) {
                        GeoLocation_RequestLookup(geo, rem->ip);
                    }
                    if (selfApplied) ConnectionArch_SetLocalGeo(added[i], &selfGeo);
                }
                for (int i = 0; i < removedCount; i++) {
                    if (removed[i]->id == selectedId) selectedId = 0;
                    if (removed[i]->id == hoveredId) hoveredId = 0;
                }
            }
        }

        float frameDelta = GetFrameTime();
        for (int i = 0; i < processCount; i++) {
            ProcessRow &row = processRows[i];
            /* per-process, delta-time driven: hold at full brightness while
             * PULSE_HOLD_SEC is left, then ease back down to the idle look  */
            if (row.pulseHold > 0.0f) {
                row.pulseHold -= frameDelta;
                if (row.pulseHold < 0.0f) row.pulseHold = 0.0f;
            }
            const float target = row.pulseHold > 0.0f ? 1.0f : 0.0f;
            const float rate = (target > row.pulse) ? PULSE_ATTACK : PULSE_RELEASE;
            const float step = rate * frameDelta;
            if (row.pulse < target) {
                row.pulse += step;
                if (row.pulse > target) row.pulse = target;
            } else if (row.pulse > target) {
                row.pulse -= step;
                if (row.pulse < target) row.pulse = target;
            }
        }
        qsort(processRows, (size_t)processCount, sizeof(ProcessRow), CompareProcessRows);

        /* ------------------------------------------------ 2. resolver results */
        {
            char ip[NET_STR_IP];
            char host[NET_STR_HOST];
            GeoInfo info;
            while (GeoLocation_PollResult(geo, ip, sizeof(ip), &info, host, sizeof(host))) {
                int n = ConnectionTracker_GetAll(tracker, sortedList, MAX_ARCS_DRAWN);
                for (int i = 0; i < n; i++) {
                    ConnectionArch *a = sortedList[i];
                    const ConnectionEndpoint *rem = ConnectionArch_Remote(a);
                    if (!rem || strcmp(rem->ip, ip) != 0) continue;
                    if (info.status != GEO_NONE) ConnectionArch_ApplyRemoteGeo(a, &info);
                    if (host[0]) ConnectionArch_ApplyRemoteHostname(a, host);
                }
            }
            if (!selfApplied && GeoLocation_GetSelf(geo, &selfGeo, ip, sizeof(ip))) {
                selfApplied = true;
                int n = ConnectionTracker_GetAll(tracker, sortedList, MAX_ARCS_DRAWN);
                for (int i = 0; i < n; i++) ConnectionArch_SetLocalGeo(sortedList[i], &selfGeo);
            }
        }

        /* ------------------------------------------------------ 3. input */
        Vector2 mouse = GetMousePosition();

        Rectangle listRec = {LIST_X, LIST_Y, LIST_W, LIST_H};
        Rectangle processRec = {LIST_X, PROCESS_Y, LIST_W, PROCESS_H};

        /* ---- network adapter selector input ---- */
        Rectangle adapterBtn = AdapterButtonRect();
        Rectangle adapterMenu = adapterDd.open
            ? AdapterMenuRect(snapshot.interfaceCount)
            : Rectangle{0, 0, 0, 0};
        bool menuConsumedClick = false;
        if (adapterDd.open) {
            adapterDd.hover = AdapterMenuHoveredIndex(adapterMenu,
                                                      snapshot.interfaceCount,
                                                      adapterDd.scroll, mouse);
            if (IsKeyPressed(KEY_ESCAPE)) adapterDd.open = false;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                menuConsumedClick = true;
                if (CheckCollisionPointRec(mouse, adapterMenu)) {
                    int row = (int)((mouse.y - (adapterMenu.y + 9.0f)) / ADAPTER_ITEM_H);
                    int idx = adapterDd.scroll + row;
                    if (idx >= 0 && idx < snapshot.interfaceCount) {
                        NetworkMonitor_SetSelectedInterface(monitor,
                                                           snapshot.interfaces[idx].id);
                        adapterDd.hover = idx;
                    }
                    adapterDd.open = false;
                } else {
                    adapterDd.open = false;   /* clicking elsewhere dismisses */
                }
            }
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                   CheckCollisionPointRec(mouse, adapterBtn)) {
            adapterDd.open = true;            /* open the picker */
        }

        /* the on-map "-" button: hovering/clicking it must not select arcs;
         * an open adapter menu also takes precedence over map hover */
        Rectangle gridBtn = MapRenderer_GridToggleRect(&projection);
        bool overGridBtn = CheckCollisionPointRec(mouse, gridBtn);
        bool mouseOverMap = !overGridBtn && !adapterDd.open &&
                            CheckCollisionPointRec(mouse, projection.bounds);

        /* build the sorted display list (active connections first) */
        int totalCount = ConnectionTracker_GetAll(tracker, sortedList, MAX_ARCS_DRAWN);
        qsort(sortedList, (size_t)totalCount, sizeof(ConnectionArch *), CompareArches);
        int drawCount = totalCount < MAX_ARCS_DRAWN ? totalCount : MAX_ARCS_DRAWN;

        ConnectionArchRenderer_UpdateGeometry(&projection, sortedList, drawCount);

        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) selectedId = 0;

        if (IsKeyPressed(KEY_G)) {
            geoEnabled = !geoEnabled;
            GeoLocation_SetEnabled(geo, geoEnabled);
        }
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
            /* "+": widen the spacing (fewer grid lines), stepping by 15 deg,
             * or 5 deg when leaving the finest step, clamped at GRID_STEP_MAX.
             * SetGridSteps applies immediately and keeps the map in sync.   */
            if (lonStep < GRID_STEP_MAX) {
                const int step = (lonStep == GRID_STEP_MIN) ? 5 : 15;
                lonStep += step;
                if (lonStep > GRID_STEP_MAX) lonStep = GRID_STEP_MAX;
                latStep = lonStep;
                MapRenderer_SetGridSteps(&map, lonStep, latStep);
            }
        }
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
            /* "-": narrow the spacing (more grid lines), stepping by 15 deg,
             * or 5 deg when leaving the coarsest step, clamped at
             * GRID_STEP_MIN. The grid is never hidden at the minimum.       */
            if (lonStep > GRID_STEP_MIN) {
                const int step = (lonStep == GRID_STEP_MAX) ? 15 : 5;
                lonStep -= step;
                if (lonStep < GRID_STEP_MIN) lonStep = GRID_STEP_MIN;
                latStep = lonStep;
                MapRenderer_SetGridSteps(&map, lonStep, latStep);
            }
        }
        if (GetMouseWheelMove() != 0.0f) {
            float wheel = GetMouseWheelMove();
            int wheelSteps = (int)wheel;
            if (wheelSteps == 0) wheelSteps = wheel > 0.0f ? 1 : -1;

            if (adapterDd.open && snapshot.interfaceCount > ADAPTER_MAX_VISIBLE &&
                CheckCollisionPointRec(mouse, adapterMenu)) {
                adapterDd.scroll -= wheelSteps;
                int maxScroll = snapshot.interfaceCount - ADAPTER_MAX_VISIBLE;
                if (maxScroll < 0) maxScroll = 0;
                if (adapterDd.scroll < 0) adapterDd.scroll = 0;
                if (adapterDd.scroll > maxScroll) adapterDd.scroll = maxScroll;
            } else {
            Rectangle detRec = {LIST_X, DET_Y + 22.0f, LIST_W, DET_H - 22.0f};
            ConnectionArch *cur = selectedId ? ConnectionTracker_FindById(tracker, selectedId)
                                             : (hoveredId ? ConnectionTracker_FindById(tracker, hoveredId) : NULL);
            if (CheckCollisionPointRec(mouse, processRec)) {
                processScrollIndex -= wheelSteps * 2;
                int visible = (int)((PROCESS_H - 37.0f) / PROCESS_ROW_H);
                int maxScroll = processCount - visible;
                if (maxScroll < 0) maxScroll = 0;
                if (processScrollIndex < 0) processScrollIndex = 0;
                if (processScrollIndex > maxScroll) processScrollIndex = maxScroll;
            } else if (cur && CheckCollisionPointRec(mouse, detRec)) {
                /* wheel over the detail card scrolls its (clipped) content */
                float contentH = ConnectionArchRenderer_DetailsContentHeight(cur);
                float maxScroll = contentH - (DET_H - 22.0f);
                if (maxScroll < 0.0f) maxScroll = 0.0f;
                detailScroll += -wheel * 18.0f;
                if (detailScroll < 0.0f) detailScroll = 0.0f;
                if (detailScroll > maxScroll) detailScroll = maxScroll;
            } else {
                scrollIndex -= wheelSteps * 3;
                if (scrollIndex < 0) scrollIndex = 0;
                int visible = (int)((LIST_H - 22.0f - 8.0f) / 32.0f);
                int maxScroll = totalCount - visible;
                if (maxScroll < 0) maxScroll = 0;
                if (scrollIndex > maxScroll) scrollIndex = maxScroll;
                }
            }
        }

        /* --------------------------------------------------- 4. hover/pick */
        hoveredId = 0;
        int listHover = -1;
        if (mouseOverMap) {
            int idx = ConnectionArchRenderer_Pick(&projection, sortedList, drawCount, mouse, 9.0f);
            if (idx >= 0) hoveredId = sortedList[idx]->id;
        }
        listHover = PickConnectionListRow(listRec, drawCount, scrollIndex, mouse);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !menuConsumedClick) {
            if (overGridBtn) {
                /* "-" button: change grid density (coarser when grid visible, finer when hidden) */
                int delta = MapRenderer_GridVisible(&map) ? 15 : 5;
                MapRenderer_ChangeGridDensity(&map, delta);
            } else if (mouseOverMap && hoveredId != 0) {
                selectedId = hoveredId;
            } else if (listHover >= 0) {
                selectedId = sortedList[listHover]->id;
            }
        }
        if (listHover >= 0 && hoveredId == 0)
            hoveredId = sortedList[listHover]->id;
        int processHover = PickProcessPanelRow(processRec, processCount, processScrollIndex, mouse);

        ConnectionArch *selected = selectedId
            ? ConnectionTracker_FindById(tracker, selectedId) : NULL;
        ConnectionArch *hovered = hoveredId
            ? ConnectionTracker_FindById(tracker, hoveredId) : NULL;

        /* ---------------------------------------------------- 5. render */
        BeginDrawing();
            ClearBackground(Color{3, 5, 16, 255});

            UiText_DrawBold("INTERNET VISUALIZER", Vector2{MAP_X, 20}, 19.0f, 1.5f,
                            Color{125, 150, 255, 255});
            UiText_Draw("real-time geographic connection map", Vector2{MAP_X + 300.0f, 26},
                        12.5f, 1.0f, Color{90, 115, 235, 255});
            char fps[64];
            snprintf(fps, sizeof(fps), "%d FPS", GetFPS());
            UiText_Draw(fps, Vector2{WIN_W - 80.0f, 26}, 12.5f, 1.0f,
                        Color{90, 115, 235, 255});

            DrawRectangleRounded(listRec, 0.04f, 6, Color{7, 10, 27, 255});
            DrawRectangleRoundedLines(listRec, 0.04f, 6, Color{0, 0, 255, 190});
            ConnectionArchRenderer_DrawList(sortedList, drawCount, listRec,
                                            scrollIndex, selectedId, hoveredId);
            DrawProcessPanel(processRows, processCount, processRec,
                             processScrollIndex, processHover);

            MapRenderer_Draw(&map, &projection);
            /* NetTime_Now() is the same clock as arch->startTime/lastActivity,
             * which the renderer compares against for entry/exit fades.       */
            ConnectionArchRenderer_Draw(&projection, sortedList, drawCount, NetTime_Now(),
                                        hoveredId, selectedId);
            /* drawn after the arcs so the "-" button always stays on top */
            MapRenderer_DrawGridToggle(&map, &projection, mouse);
            if (mouseOverMap) MapRenderer_DrawCursorReadout(&projection, mouse);

            DrawStatusBar(&snapshot, geo, tracker, geoEnabled);
            /* adapter selector drawn last so the (upward opening) menu stays
             * on top of the map and panels */
            DrawAdapterSelector(snapshot, adapterDd, mouse);

            /* right hand panels */
            /* reset the detail-card scroll whenever the shown connection changes */
            {
                static uint64_t lastDetailId = 0;
                uint64_t detailId = selected ? selected->id : (hovered ? hovered->id : 0);
                if (detailId != lastDetailId) { detailScroll = 0.0f; lastDetailId = detailId; }
            }
            DrawRectangleRounded({LIST_X, DET_Y, LIST_W, DET_H}, 0.08f, 6, Color{7, 10, 27, 255});
            DrawRectangleRoundedLines({LIST_X, DET_Y, LIST_W, DET_H}, 0.08f, 6, Color{0, 0, 255, 190});
            UiText_DrawTruncated("CONNECTION DETAILS", Vector2{LIST_X + 10.0f, DET_Y + 7.0f},
                                 12.5f, 1.0f, LIST_W - 20.0f, Color{125, 150, 255, 255});
            ConnectionArchRenderer_DrawDetails(selected ? selected : hovered,
                                               {LIST_X, DET_Y + 22.0f, LIST_W, DET_H - 22.0f},
                                               detailScroll);

            UiText_Draw("LMB select   RMB clear   wheel scroll   G geo   +/- grid step   map [-] grid on/off   adapter: click in status bar",
                         Vector2{MAP_X, WIN_H - 15.0f}, 11.5f, 1.0f, Color{90, 115, 235, 255});
        EndDrawing();
    }

    /* ----------------------------------------------------------- shutdown */
    /* Shutdown */
    NetworkMonitor_Destroy(monitor);
    GeoLocation_Destroy(geo);
    ConnectionTracker_Destroy(tracker);
    MapRenderer_Shutdown(&map);
    /* release fonts before CloseWindow (they share GL textures) */
    if (regFont.texture.id != 0 && regFont.texture.id != GetFontDefault().texture.id)
        UnloadFont(regFont);
    if (boldFont.texture.id != 0 && boldFont.texture.id != regFont.texture.id &&
        boldFont.texture.id != GetFontDefault().texture.id)
        UnloadFont(boldFont);
    CloseWindow();
    return 0;
}
