/* MapProjection.c - equirectangular projection shared by every map consumer. */
#include "MapProjection.h"

#include <math.h>

void MapProjection_Init(MapProjection *p, Rectangle bounds,
                        double lonMin, double lonMax, double latTop, double latBottom)
{
    p->bounds = bounds;
    p->lonMin = lonMin;
    p->lonMax = lonMax;
    p->latTop = latTop;
    p->latBottom = latBottom;
    if (p->lonMax - p->lonMin < 1.0) p->lonMax = p->lonMin + 1.0;
    if (p->latTop - p->latBottom < 1.0) p->latBottom = p->latTop - 1.0;
}

void MapProjection_InitWithDefaults(MapProjection *p, Rectangle bounds)
{
    const float aspect = (float)((MAP_ASSET_LON_MAX - MAP_ASSET_LON_MIN) /
                                 (MAP_ASSET_LAT_TOP - MAP_ASSET_LAT_BOT));
    if (bounds.width / bounds.height > aspect) {
        float width = bounds.height * aspect;
        bounds.x += (bounds.width - width) * 0.5f;
        bounds.width = width;
    } else {
        float height = bounds.width / aspect;
        bounds.y += (bounds.height - height) * 0.5f;
        bounds.height = height;
    }
    MapProjection_Init(p, bounds, MAP_ASSET_LON_MIN, MAP_ASSET_LON_MAX,
                       MAP_ASSET_LAT_TOP, MAP_ASSET_LAT_BOT);
}

void MapProjection_SetBounds(MapProjection *p, Rectangle bounds)
{
    p->bounds = bounds;
}

static double WrapLongitude(const MapProjection *p, double lon)
{
    const double span = 360.0;
    while (lon < p->lonMin) lon += span;
    while (lon >= p->lonMin + span) lon -= span;
    return lon;
}

void MapProjection_ClampLonLat(const MapProjection *p, double *lon, double *lat)
{
    if (lon) *lon = WrapLongitude(p, *lon);
    if (lat) {
        if (*lat > p->latTop) *lat = p->latTop;
        if (*lat < p->latBottom) *lat = p->latBottom;
    }
}

Vector2 MapProjection_LonLatToScreenUnwrapped(const MapProjection *p, double lon, double lat)
{
    const double lonSpan = p->lonMax - p->lonMin;
    const double latSpan = p->latTop - p->latBottom;
    if (lat > p->latTop) lat = p->latTop;
    if (lat < p->latBottom) lat = p->latBottom;

    Vector2 v;
    v.x = (float)(p->bounds.x + (lon - p->lonMin) * p->bounds.width / lonSpan);
    v.y = (float)(p->bounds.y + (p->latTop - lat) * p->bounds.height / latSpan);
    return v;
}

Vector2 MapProjection_LonLatToScreen(const MapProjection *p, double lon, double lat)
{
    double lonW = WrapLongitude(p, lon);
    return MapProjection_LonLatToScreenUnwrapped(p, lonW, lat);
}

bool MapProjection_ScreenToLonLat(const MapProjection *p, Vector2 pos, double *lon, double *lat)
{
    if (pos.x < p->bounds.x || pos.x > p->bounds.x + p->bounds.width) return false;
    if (pos.y < p->bounds.y || pos.y > p->bounds.y + p->bounds.height) return false;

    const double lonSpan = p->lonMax - p->lonMin;
    const double latSpan = p->latTop - p->latBottom;
    if (lon) *lon = p->lonMin + (pos.x - p->bounds.x) * lonSpan / p->bounds.width;
    if (lat) *lat = p->latTop - (pos.y - p->bounds.y) * latSpan / p->bounds.height;
    return true;
}

bool MapProjection_IsCovered(const MapProjection *p, double lon, double lat)
{
    if (lat < p->latBottom || lat > p->latTop) return false;
    return (lon >= p->lonMin && lon < p->lonMax);
}
