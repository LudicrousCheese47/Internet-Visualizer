/*
 * UiText.h - shared typography and overflow-safe text layout helpers.
 *
 * The application uses one monospaced font family (regular + bold) loaded
 * once at startup and released at shutdown; every module draws through these
 * helpers so measurement, truncation and wrapping always use the *actual*
 * font and size being rendered. All helpers are pure drawing - no heap
 * allocation happens per call (stack scratch buffers only).
 *
 * Clipping for panels is done with raylib's scissor mode:
 *   UiText_BeginClip(rect) ... UiText_EndClip();
 * EndClip always restores the state, so pairs must not be nested.
 */
#pragma once

#include "raylib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fonts are owned by the caller (loaded once in main, unloaded before
 * CloseWindow). UiText keeps shallow copies; calling this twice replaces
 * them. Until called, helpers fall back to GetFontDefault().             */
void        UiText_SetFonts(Font regular, Font bold);
Font        UiText_Font(void);      /* regular font in use (never invalid) */
Font        UiText_Bold(void);      /* bold font in use                    */

/* Plain draws -------------------------------------------------------------- */
void        UiText_Draw(const char *text, Vector2 pos, float size, float spacing,
                        Color color);
void        UiText_DrawBold(const char *text, Vector2 pos, float size, float spacing,
                            Color color);
float       UiText_Width(const char *text, float size, float spacing);

/* Single-line truncation ---------------------------------------------------
 * Draws `text` at `pos`, cutting it with a trailing "..." so the drawn width
 * never exceeds maxWidth. Uses MeasureTextEx() against the live font, so it
 * works for any size/fallback font. Returns the width actually drawn.     */
float       UiText_DrawTruncated(const char *text, Vector2 pos, float size,
                                 float spacing, float maxWidth, Color color);

/* Same logic without drawing: writes a NUL-terminated, ellipsised copy of
 * `text` into out (always safe for outCap >= 4).                          */
void        UiText_Truncate(const char *text, float size, float spacing,
                            float maxWidth, char *out, int outCap);

/* Multi-line wrapping ------------------------------------------------------
 * Word-wraps `text` into lines no wider than maxWidth and draws up to
 * maxLines of them (maxLines <= 0 means unlimited). Words that cannot fit on
 * a line by themselves (IPv6 addresses, URLs, unbroken tokens) are hard
 * broken at codepoint boundaries. Returns the y coordinate below the last
 * drawn line. Lines that did not fit within maxLines are elided with "...".
 */
float       UiText_DrawWrapped(const char *text, Vector2 pos, float maxWidth,
                               float size, float lineSpacing, Color color,
                               int maxLines);

/* Panel clipping ----------------------------------------------------------- */
void        UiText_BeginClip(Rectangle r);   /* scissor to rect (screen px) */
void        UiText_EndClip(void);            /* restore scissor state       */

#ifdef __cplusplus
}
#endif
