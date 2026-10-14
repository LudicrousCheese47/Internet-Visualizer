/*
 * MapRenderer.h - Draws the world map texture, the longitude/latitude grid
 * and the coordinate labels.
 *
 * The grid is drawn in geographic coordinates through MapProjection, so the
 * lines always agree with the coastline texture regardless of where the map
 * is placed or resized. The equator and the prime meridian are highlighted.
 */
#pragma once

#include "raylib.h"
#include "MapProjection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapRenderer {
    Texture2D texture;
    bool      loaded;
    int       lonStepDeg;    /* grid spacing in longitude (default 30)        */
    int       latStepDeg;    /* grid spacing in latitude  (default 30)        */
    bool      gridEnabled;   /* graticule visibility (toggled by the "-") btn */
} MapRenderer;

bool  MapRenderer_Init(MapRenderer *r, const char *texturePath);
void  MapRenderer_Shutdown(MapRenderer *r);

/* Draws: ocean panel, coastline texture, graticule (when enabled), coordinate
 * labels and the map frame. Call between BeginDrawing()/EndDrawing().        */
void  MapRenderer_Draw(const MapRenderer *r, const MapProjection *p);

/* Grid spacing in degrees; values < 1 are ignored, > 90 are capped.           */
void  MapRenderer_SetGridSteps(MapRenderer *r, int lonDeg, int latDeg);

/* Change the grid density by delta degrees. Negative delta decreases density
 * (more frequent lines); positive increases it (spaced out). Delta must be in
 * range (-90, 0) for the grid button toggle use-case. We clamp the final steps
 * to the valid range [2, 90]. Call SetGridSteps afterwards to apply. Values
 * out of range are silently ignored.  (The comment in the header is a little
 * old; SetGridSteps now respects the internal fields, so the change function
 * is just a validation + update, no need for a SetGridSteps call here.)
 */
void  MapRenderer_ChangeGridDensity(MapRenderer *r, int delta);

/* Graticule visibility. The grid implementation is never created or destroyed
 * by this - it only decides whether MapRenderer_Draw renders it (lines *and*
 * coordinate labels). The map texture, arcs, markers and packets always draw. */
void  MapRenderer_SetGridVisible(MapRenderer *r, bool visible);
bool  MapRenderer_GridVisible(const MapRenderer *r);

/* Screen-space rectangle of the on-map "-" toggle button (top-right corner of
 * the map viewport). Use it both for hit-testing in the input phase and - via
 * MapRenderer_DrawGridToggle - for rendering it.                              */
Rectangle MapRenderer_GridToggleRect(const MapProjection *p);

/* Draws the "-" toggle button; its fill/border/glyph mirror gridEnabled so the
 * button's visual state always matches whether the grid is showing.           */
void  MapRenderer_DrawGridToggle(const MapRenderer *r, const MapProjection *p,
                                 Vector2 mouse);

/* Draws the small coordinate readout for the hovered map position.            */
void  MapRenderer_DrawCursorReadout(const MapProjection *p, Vector2 mouse);

#ifdef __cplusplus
}
#endif
