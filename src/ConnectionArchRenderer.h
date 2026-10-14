/*
 * ConnectionArchRenderer.h - Draws ConnectionArch objects on the map.
 *
 * Rendering only consumes the shared MapProjection and the ConnectionArch
 * data; it never touches the networking subsystems. Curves are quadratic
 * Béziers sampled into ARCH_ARC_POINTS points per arc, which is fast enough
 * for hundreds of simultaneous connections and doubles as the hit-test
 * geometry for hover/selection.
 *
 * Direction handling: the animated marker travels sender -> receiver, and it
 * is only drawn when the direction is actually known (TCP state based
 * inference in the monitor). Undirected connections are drawn without a
 * marker so the UI never invents a traffic direction.
 */
#pragma once

#include "raylib.h"
#include "MapProjection.h"
#include "ConnectionArch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Recomputes screen geometry for every arc (cached inside each arch).        */
void ConnectionArchRenderer_UpdateGeometry(const MapProjection *p,
                                           ConnectionArch *const *arcs, int count);

/* Draws all arcs + endpoint markers. hovered/selected are arch ids (0 = none). */
void ConnectionArchRenderer_Draw(const MapProjection *p,
                                 ConnectionArch *const *arcs, int count,
                                 double now, uint64_t hoveredId, uint64_t selectedId);

/* Index of the arc under the mouse (within maxDist pixels), or -1.            */
int  ConnectionArchRenderer_Pick(const MapProjection *p,
                                 ConnectionArch *const *arcs, int count,
                                 Vector2 mouse, float maxDist);

/* Right hand side connection list. Returns the arch index hovered by the
 * mouse (or -1); the caller decides what a click means.                       */
int  ConnectionArchRenderer_DrawList(ConnectionArch *const *arcs, int count,
                                     Rectangle panel, int scrollIndex,
                                     uint64_t selectedId, uint64_t hoveredId);

/* Detail card for the selected/hovered connection. `scrollY` offsets the
 * content vertically (the card clips to `panel` and can scroll when the
 * information exceeds the panel height).                                  */
void ConnectionArchRenderer_DrawDetails(const ConnectionArch *arch, Rectangle panel,
                                        float scrollY);

/* Pixel height of the detail card content, for clamping the scroll range. */
float ConnectionArchRenderer_DetailsContentHeight(const ConnectionArch *arch);

#ifdef __cplusplus
}
#endif
