/* MapRenderer.c - coastline texture + geographic graticule rendering.
 *
 * The bundled asset is green land on a transparent background. For the
 * terminal look it is recoloured ONCE at load time to pure neon green
 * RGB(0,255,0), keeping the original per-pixel alpha so coastlines stay
 * crisp. Each frame the (static) texture is drawn three times - two cheap
 * scaled-up low-alpha passes for the outer glow, then the sharp layer -
 * so the glow costs three textured quads, not per-pixel work.
 */
#include "MapRenderer.h"

#include "UiText.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool MapRenderer_Init(MapRenderer *r, const char *texturePath)
{
    if (!r) return false;
    r->loaded = false;
    r->lonStepDeg = 30;
    r->latStepDeg = 30;
    r->gridEnabled = true;    /* graticule starts visible                    */
    r->texture.id = 0;

    Image img = LoadImage(texturePath);
    if (img.data) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        Color *px = (Color *)img.data;
        const int nPixels = img.width * img.height;
        for (int i = 0; i < nPixels; i++) {
            const Color c = px[i];
            /* Keep every nontransparent part of the known land-mask asset. */
            if (c.a > 40) {
                px[i] = (Color){0, 255, 0, c.a};   /* neon green, alpha kept */
            }
        }
        r->texture = LoadTextureFromImage(img);
        UnloadImage(img);
        SetTextureFilter(r->texture, TEXTURE_FILTER_BILINEAR);
        r->loaded = (r->texture.id != 0);
    }

    if (!r->loaded) {
        TraceLog(LOG_WARNING, "MapRenderer: could not load '%s'", texturePath);
        return false;
    }
    return true;
}

void MapRenderer_Shutdown(MapRenderer *r)
{
    if (!r) return;
    if (r->loaded) {
        UnloadTexture(r->texture);
        r->loaded = false;
    }
}

void MapRenderer_SetGridSteps(MapRenderer *r, int lonDeg, int latDeg)
{
    if (!r) return;
    if (lonDeg >= 1 && lonDeg <= 180) r->lonStepDeg = lonDeg;
    if (latDeg >= 1 && latDeg <= 90)  r->latStepDeg = latDeg;
}

void MapRenderer_ChangeGridDensity(MapRenderer *r, int delta)
{
    if (!r) return;

    int newLon = r->lonStepDeg - delta;
    int newLat = r->latStepDeg - delta;

    if (newLon < 2) newLon = 2;
    if (newLat < 2) newLat = 2;

    r->lonStepDeg = newLon;
    r->latStepDeg = newLat;
}

void MapRenderer_SetGridVisible(MapRenderer *r, bool visible)
{
    if (!r) return;
    r->gridEnabled = visible;   /* pure render switch: geometry is untouched */
}

bool MapRenderer_GridVisible(const MapRenderer *r)
{
    return r ? r->gridEnabled : false;
}

/* The "-" button lives in the top-right corner of the map viewport, below the
 * row of longitude labels (the easternmost label ends well left of it), so it
 * never covers a grid label or any other panel.                              */
#define GRID_TOGGLE_W 44.0f
#define GRID_TOGGLE_H 22.0f

Rectangle MapRenderer_GridToggleRect(const MapProjection *p)
{
    if (!p) return (Rectangle){0.0f, 0.0f, 0.0f, 0.0f};
    const Rectangle b = p->bounds;
    return (Rectangle){b.x + b.width - GRID_TOGGLE_W - 8.0f,
                       b.y + 8.0f, GRID_TOGGLE_W, GRID_TOGGLE_H};
}

static void FormatLongitude(double lon, char *out, size_t cap)
{
    double v = lon;
    while (v > 180.0) v -= 360.0;
    while (v < -180.0) v += 360.0;
    if (fabs(v) < 0.001) snprintf(out, cap, "0");
    else if (v > 0)      snprintf(out, cap, "%.0f" "\xC2\xB0" "E", v);
    else                 snprintf(out, cap, "%.0f" "\xC2\xB0" "W", -v);
}

static void FormatLatitude(double lat, char *out, size_t cap)
{
    if (fabs(lat) < 0.001) snprintf(out, cap, "0");
    else if (lat > 0)      snprintf(out, cap, "%.0f" "\xC2\xB0" "N", lat);
    else                   snprintf(out, cap, "%.0f" "\xC2\xB0" "S", -lat);
}

static double CeilToStep(double v, double step)
{
    return ceil(v / step - 1e-9) * step;
}

/* Draw one graticule line with a subtle outer glow: two low-alpha passes
 * slightly wider than the crisp core line. */
static void GlowedLine(Vector2 a, Vector2 b, float thickness, Color core)
{
    Color glow = {core.r, core.g, core.b, (uint8_t)(core.a / 4)};
    DrawLineEx(a, b, thickness + 3.0f, glow);
    DrawLineEx(a, b, thickness + 1.2f, (Color){core.r, core.g, core.b, (uint8_t)(core.a / 2)});
    DrawLineEx(a, b, thickness, core);
}

void MapRenderer_Draw(const MapRenderer *r, const MapProjection *p)
{
    if (!r || !p) return;
    const Rectangle b = p->bounds;

    /* dark terminal background behind the (transparent) coastline texture */
    DrawRectangleRounded(b, 0.04f, 6, (Color){4, 12, 8, 255});
    DrawRectangleRoundedLines(b, 0.04f, 6, (Color){0, 0, 255, 180});

    if (r->loaded) {
        Rectangle src = {0.0f, 0.0f, (float)r->texture.width, (float)r->texture.height};
        /* outer glow: two scaled-up, faded copies of the neon-green layer   */
        const Vector2 c = {b.x + b.width * 0.5f, b.y + b.height * 0.5f};
        for (int g = 0; g < 2; g++) {
            float pad = (g == 0) ? 6.0f : 2.5f;
            Rectangle dst = {b.x - pad, b.y - pad, b.width + pad * 2.0f, b.height + pad * 2.0f};
            uint8_t a = (g == 0) ? 40 : 80;
            DrawTexturePro(r->texture, src, dst, (Vector2){0.0f, 0.0f}, 0.0f,
                           (Color){0, 255, 0, a});
            (void)c;
        }
        DrawTexturePro(r->texture, src, b, (Vector2){0.0f, 0.0f}, 0.0f,
                       (Color){0, 255, 0, 255});
    }

    /* ---- graticule (real geographic coordinates via the shared projection) ----
     * Guarded by gridEnabled so the "-" button can hide *only* the longitude/
     * latitude lines and their labels; the ocean panel, coastlines, arcs,
     * markers and data packets drawn around this call are unaffected.         */
    if (r->gridEnabled) {
        const Color minor = (Color){0, 0, 255, 55};
        const Color major = (Color){0, 0, 255, 120};

        char label[24];

        /* meridians */
        for (int lon = -180; lon <= 180; lon += r->lonStepDeg) {
            if (lon < p->lonMin || lon >= p->lonMax) continue;   /* outside the asset */
            Vector2 a = MapProjection_LonLatToScreen(p, lon, p->latTop);
            Vector2 z = MapProjection_LonLatToScreen(p, lon, p->latBottom);
            bool prime = (lon == 0);
            GlowedLine(a, z, prime ? 1.5f : 1.0f, prime ? major : minor);

            FormatLongitude(lon, label, sizeof(label));
            float tw = UiText_Width(label, 11.0f, 1.0f);
            UiText_Draw(label, (Vector2){a.x - tw / 2.0f + 1.0f, b.y + 5.0f}, 11.0f, 1.0f,
                        prime ? (Color){170, 190, 255, 235} : (Color){80, 100, 255, 190});
        }

        /* parallels */
        for (int lat = -90; lat <= 90; lat += r->latStepDeg) {
            if (lat < p->latBottom || lat > p->latTop) continue;
            Vector2 a = MapProjection_LonLatToScreen(p, p->lonMin, lat);
            Vector2 z = MapProjection_LonLatToScreen(p, p->lonMax, lat);
            bool equator = (lat == 0);
            GlowedLine(a, z, equator ? 1.5f : 1.0f, equator ? major : minor);

            FormatLatitude(lat, label, sizeof(label));
            UiText_Draw(label, (Vector2){b.x + 5.0f, a.y - 4.0f}, 11.0f, 1.0f,
                        equator ? (Color){170, 190, 255, 235} : (Color){80, 100, 255, 190});
        }
    }

    /* frame on top so grid lines never stick out */
    DrawRectangleLinesEx(b, 1.0f, (Color){0, 0, 255, 210});
}

void MapRenderer_DrawGridToggle(const MapRenderer *r, const MapProjection *p, Vector2 mouse)
{
    if (!r || !p) return;

    Rectangle btn = MapRenderer_GridToggleRect(p);
    const bool hot = CheckCollisionPointRec(mouse, btn);
    const bool on  = r->gridEnabled;

    Color fill, border, glyph;
    if (on) {                       /* grid visible -> lit, "active" state    */
        fill   = hot ? (Color){0, 0, 255, 155} : (Color){0, 0, 255, 105};
        border = (Color){45, 70, 255, 245};
        glyph  = hot ? (Color){238, 244, 255, 255} : (Color){205, 218, 255, 255};
    } else {                        /* grid hidden -> dim, "inactive" state   */
        fill   = hot ? (Color){9, 13, 34, 250} : (Color){6, 9, 24, 230};
        border = (Color){0, 0, 255, 125};
        glyph  = (Color){95, 115, 215, 235};
    }

    DrawRectangleRounded(btn, 0.35f, 5, fill);
    DrawRectangleRoundedLines(btn, 0.35f, 5, border);

    const char *sym = "-";
    float tw = UiText_Width(sym, 17.0f, 1.0f);
    UiText_DrawBold(sym, (Vector2){btn.x + (btn.width - tw) * 0.5f, btn.y + 1.0f},
                    17.0f, 1.0f, glyph);

    /* small state lamp: bright while the graticule is rendered, dark when off */
    Color lamp = on ? (Color){0, 255, 170, 255} : (Color){0, 0, 255, 130};
    DrawCircle((int)(btn.x + btn.width - 6.0f), (int)(btn.y + 5.0f), 2.5f, lamp);
}

void MapRenderer_DrawCursorReadout(const MapProjection *p, Vector2 mouse)
{
    double lon = 0.0, lat = 0.0;
    if (!MapProjection_ScreenToLonLat(p, mouse, &lon, &lat)) return;

    char lonStr[24], latStr[24], text[64];
    FormatLongitude(lon, lonStr, sizeof(lonStr));
    FormatLatitude(lat, latStr, sizeof(latStr));
    snprintf(text, sizeof(text), "%s   %s", lonStr, latStr);

    float w = UiText_Width(text, 12.5f, 1.0f) + 16.0f;
    Rectangle box = {mouse.x + 14, mouse.y - 26, w, 22.0f};
    if (box.x + box.width > p->bounds.x + p->bounds.width) box.x = mouse.x - box.width - 14;
    DrawRectangleRounded(box, 0.4f, 4, (Color){6, 9, 24, 240});
    DrawRectangleRoundedLines(box, 0.4f, 4, (Color){0, 0, 255, 200});
    UiText_Draw(text, (Vector2){box.x + 8, box.y + 5}, 12.5f, 1.0f, (Color){130, 155, 255, 255});
}
