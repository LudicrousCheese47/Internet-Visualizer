/* UiText.c - shared typography and overflow-safe text layout helpers. */
#include "UiText.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- fonts ----- */

static Font sRegular;
static Font sBold;
static int  sSet = 0;

void UiText_SetFonts(Font regular, Font bold)
{
    sRegular = regular;
    sBold    = bold;
    sSet     = 1;
}

Font UiText_Font(void) { return sSet ? sRegular : GetFontDefault(); }
Font UiText_Bold(void) { return sSet ? sBold    : GetFontDefault(); }

/* --------------------------------------------------------- utf-8 utils --- */

/* Byte length of the codepoint starting at s (>= 1 always). */
static int U8Len(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;                       /* invalid lead byte: consume one      */
}

/* Byte offset of the n-th codepoint (n == 0 -> 0). */
static int PrefixBytes(const char *text, int n)
{
    int i = 0, cp = 0, len = (int)strlen(text);
    while (i < len && cp < n) { i += U8Len(text + i); cp++; }
    return i;
}

/* Copy at most src bytes into dst (cap incl. NUL), never splitting a
 * codepoint. Returns the byte length written (without NUL).               */
static int CopySafe(const char *src, int srcBytes, char *dst, int cap)
{
    if (cap <= 0) return 0;
    int n = srcBytes;
    if (n > cap - 1) n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;  /* continuation */
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
    return n;
}

/* ----------------------------------------------------------- measuring --- */

float UiText_Width(const char *text, float size, float spacing)
{
    if (!text || !text[0]) return 0.0f;
    return MeasureTextEx(UiText_Font(), text, size, spacing).x;
}

void UiText_Draw(const char *text, Vector2 pos, float size, float spacing, Color color)
{
    if (!text || !text[0]) return;
    DrawTextEx(UiText_Font(), text, pos, size, spacing, color);
}

void UiText_DrawBold(const char *text, Vector2 pos, float size, float spacing, Color color)
{
    if (!text || !text[0]) return;
    DrawTextEx(UiText_Bold(), text, pos, size, spacing, color);
}

/* --------------------------------------------------------- truncation ----- */

void UiText_Truncate(const char *text, float size, float spacing,
                     float maxWidth, char *out, int outCap)
{
    if (!out || outCap <= 0) return;
    out[0] = '\0';
    if (!text || !text[0]) return;
    if (outCap < 4) return;

    Font f = UiText_Font();
    int len = (int)strlen(text);

    if (maxWidth <= 0.0f) { CopySafe("...", 3, out, outCap); return; }

    char buf[512];
    int capped = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, text, (size_t)capped);
    buf[capped] = '\0';

    if (MeasureTextEx(f, buf, size, spacing).x <= maxWidth) {
        CopySafe(buf, capped, out, outCap);
        return;
    }

    const char *suffix = "...";
    float suffixW = MeasureTextEx(f, suffix, size, spacing).x;
    if (suffixW > maxWidth) { CopySafe(suffix, 3, out, outCap); return; }
    float budget = maxWidth - suffixW;

    /* binary search: longest prefix (whole codepoints) that fits the budget */
    int totalCp = 0;
    for (int i = 0; i < capped; ) { totalCp++; i += U8Len(buf + i); }

    int lo = 0, hi = totalCp;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        int bytes = PrefixBytes(buf, mid);
        buf[bytes] = '\0';
        if (MeasureTextEx(f, buf, size, spacing).x <= budget) lo = mid;
        else hi = mid - 1;
        /* restore full buffer for the next probe */
        memcpy(buf, text, (size_t)capped);
        buf[capped] = '\0';
    }

    int cut = PrefixBytes(text, lo);
    int n = CopySafe(text, cut, out, outCap - 3);
    /* append ellipsis (respecting outCap; CopySafe keeps codepoints whole)  */
    int room = outCap - 1 - n;
    if (room >= 3) memcpy(out + n, "...", 3);
    else if (room > 0) out[n] = '\0';
    out[outCap - 1] = '\0';
}

float UiText_DrawTruncated(const char *text, Vector2 pos, float size,
                           float spacing, float maxWidth, Color color)
{
    if (!text || !text[0]) return 0.0f;
    char buf[512];
    UiText_Truncate(text, size, spacing, maxWidth, buf, (int)sizeof(buf));
    if (!buf[0]) return 0.0f;
    Vector2 w = MeasureTextEx(UiText_Font(), buf, size, spacing);
    DrawTextEx(UiText_Font(), buf, pos, size, spacing, color);
    return w.x;
}

/* ------------------------------------------------------------ wrapping ---- */

float UiText_DrawWrapped(const char *text, Vector2 pos, float maxWidth,
                         float size, float lineSpacing, Color color, int maxLines)
{
    if (!text || !text[0]) return pos.y;
    if (maxWidth < 8.0f) maxWidth = 8.0f;

    Font f = UiText_Font();
    float y = pos.y;
    int drawn = 0;
    int truncated = 0;
    float lastEndX = pos.x;
    float lastY = pos.y;

    char line[512];
    int lineBytes = 0;
    char probe[512];

    /* draw the current line; returns 0 when the line budget is exhausted   */
    #define UIW_FLUSH() do {                                                     \
            if (lineBytes > 0) {                                                \
                if (maxLines > 0 && drawn >= maxLines) { truncated = 1; goto done; } \
                line[lineBytes] = '\0';                                         \
                DrawTextEx(f, line, (Vector2){pos.x, y}, size, spacing, color);  \
                lastEndX = pos.x + MeasureTextEx(f, line, size, spacing).x;      \
                lastY = y;                                                      \
                drawn++;                                                        \
                y += lineSpacing;                                               \
                lineBytes = 0;                                                  \
            }                                                                   \
        } while (0)

    float spacing = 1.0f;
    const char *p = text;

    while (*p) {
        if (*p == '\n') { UIW_FLUSH(); p++; continue; }
        if (*p == ' ')  { p++; continue; }          /* trim line-trailing spaces */

        /* collect one word */
        const char *wStart = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        int wordBytes = (int)(p - wStart);

        /* try appending the word to the current line */
        for (;;) {
            int need = lineBytes + wordBytes;
            if (need < (int)sizeof(probe) - 1) {
                memcpy(probe, line, (size_t)lineBytes);
                memcpy(probe + lineBytes, wStart, (size_t)wordBytes);
                probe[need] = '\0';
                if (MeasureTextEx(f, probe, size, spacing).x <= maxWidth) {
                    memcpy(line, probe, (size_t)need);
                    lineBytes = need;
                    break;
                }
            }
            if (lineBytes > 0) { UIW_FLUSH(); continue; }   /* retry on fresh line */

            /* line is empty and the word still does not fit: hard break it  */
            {
                int taken = 0;
                while (taken < wordBytes) {
                    int cpLen = U8Len(wStart + taken);
                    if (taken + cpLen > (int)sizeof(probe) - 1) break;
                    memcpy(probe, wStart, (size_t)(taken + cpLen));
                    probe[taken + cpLen] = '\0';
                    if (MeasureTextEx(f, probe, size, spacing).x > maxWidth) break;
                    taken += cpLen;
                }
                if (taken <= 0) taken = U8Len(wStart);       /* never stall */
                memcpy(line, wStart, (size_t)taken);
                lineBytes = taken;
                UIW_FLUSH();
                wStart  += taken;
                wordBytes -= taken;
                if (wordBytes <= 0) break;
            }
        }
    }
    UIW_FLUSH();

done:
    if (truncated) {
        const char *dots = "...";
        DrawTextEx(f, dots, (Vector2){lastEndX + 2.0f, lastY}, size, spacing, color);
    }
    #undef UIW_FLUSH
    return y;
}

/* ------------------------------------------------------------- clipping -- */

void UiText_BeginClip(Rectangle r)
{
    int x = (int)r.x, y = (int)r.y;
    int w = (int)r.width, h = (int)r.height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    BeginScissorMode(x, y, w, h);
}

void UiText_EndClip(void)
{
    EndScissorMode();
}
