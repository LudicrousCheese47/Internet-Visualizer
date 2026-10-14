/*
 * MapProjection.h - The single geographic -> screen coordinate transform.
 *
 * Equirectangular (plate carrée) projection over a configurable geographic
 * window. Everything that touches the map - the coastline texture, the
 * longitude/latitude grid, endpoint markers and connection arcs - must use
 * these functions so that a given lon/lat always lands on the same pixel.
 *
 * The bundled Natural Earth land mask is plate carrée and spans the exact
 * world bounds: longitude -180..180 and latitude +90..-90.
 */
#pragma once

#include "raylib.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapProjection {
    Rectangle bounds;      /* screen rectangle the map occupies (pixels)      */
    double lonMin;         /* longitude at bounds.x                           */
    double lonMax;         /* longitude at bounds.x + bounds.width            */
    double latTop;         /* latitude  at bounds.y                           */
    double latBottom;      /* latitude  at bounds.y + bounds.height           */
} MapProjection;

/* Geographic edges of the equirectangular assets/wm.png texture. */
#define MAP_ASSET_LON_MIN   (-180.0)
#define MAP_ASSET_LON_MAX   (180.0)
#define MAP_ASSET_LAT_TOP   (90.0)
#define MAP_ASSET_LAT_BOT   (-90.0)

void    MapProjection_Init(MapProjection *p, Rectangle bounds,
                           double lonMin, double lonMax, double latTop, double latBottom);
void    MapProjection_InitWithDefaults(MapProjection *p, Rectangle bounds);
/* Moves/resizes the map area without changing the geographic window.          */
void    MapProjection_SetBounds(MapProjection *p, Rectangle bounds);

/* Longitude/latitude -> screen pixels. Latitude is clamped into the mapped
 * window; longitude is wrapped into [lonMin, lonMin+360) so endpoints close to
 * the antimeridian still project sensibly.                                    */
Vector2 MapProjection_LonLatToScreen(const MapProjection *p, double lon, double lat);

/* Same transform without wrapping/clamping: used to draw arcs that take the
 * short way across the antimeridian (may land slightly outside the rect).     */
Vector2 MapProjection_LonLatToScreenUnwrapped(const MapProjection *p, double lon, double lat);

/* Screen pixels -> longitude/latitude. Returns false when the point lies
 * outside the map rectangle.                                                  */
bool    MapProjection_ScreenToLonLat(const MapProjection *p, Vector2 pos,
                                     double *lon, double *lat);

/* True when the coordinate falls inside the mapped geographic window.         */
bool    MapProjection_IsCovered(const MapProjection *p, double lon, double lat);

void    MapProjection_ClampLonLat(const MapProjection *p, double *lon, double *lat);

#ifdef __cplusplus
}
#endif
