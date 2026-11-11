// src/tools/ui_render.h
//
// Stage 5 (goal 4): tiny, dependency-free RGBA raster helpers used to composite
// a DiabloUI screen's real decoded art into a 640x480 canvas and to rasterize
// the element-rectangle OVERLAY (boxes + number tags). Header-only, pure pixels
// (no nlohmann/json, no engine, no decoders) so tests/unit_tests.cpp can test it
// by #include-ing it directly.
//
// Deliberately does NOT reuse scene_tools.cpp's private putPixel/line helpers:
// those live in an anonymous namespace; duplicating a self-contained version
// here avoids touching already-verified scene rendering code.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tools/ui_layout.h"

namespace ui_render {

// ---------------------------------------------------------------------------
// Canvas: row-major RGBA8, alpha 0 = transparent.
// ---------------------------------------------------------------------------
struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;

    Canvas() = default;
    Canvas(int W, int H, uint32_t fill) : w(W), h(H), rgba(static_cast<size_t>(W) * H * 4) {
        fillAll(fill);
    }
    void fillAll(uint32_t c) {
        const uint8_t r = ui_layout::overlayR(c), g = ui_layout::overlayG(c);
        const uint8_t b = ui_layout::overlayB(c), a = ui_layout::overlayA(c);
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
        }
    }
    size_t pixelCount() const { return static_cast<size_t>(w) * h; }
};

// ---------------------------------------------------------------------------
// putPixel — clipped, src-over compositing. `c` is packed RGBA (R<<24).
// ---------------------------------------------------------------------------
inline void putPixel(Canvas& cv, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) return;
    const uint8_t sa = ui_layout::overlayA(c);
    if (sa == 0) return;
    const size_t o = (static_cast<size_t>(y) * cv.w + x) * 4;
    const uint8_t sr = ui_layout::overlayR(c), sg = ui_layout::overlayG(c),
                  sb = ui_layout::overlayB(c);
    if (sa == 255) {
        cv.rgba[o] = sr; cv.rgba[o + 1] = sg; cv.rgba[o + 2] = sb; cv.rgba[o + 3] = 255;
    } else {
        const int da = cv.rgba[o + 3];
        const int a = sa + da * (255 - sa) / 255;
        if (a == 0) { cv.rgba[o] = cv.rgba[o + 1] = cv.rgba[o + 2] = cv.rgba[o + 3] = 0; return; }
        const int wa = sa * 255, wd = da * (255 - sa);
        cv.rgba[o]     = static_cast<uint8_t>((sr * wa + cv.rgba[o]     * wd) / (a * 255));
        cv.rgba[o + 1] = static_cast<uint8_t>((sg * wa + cv.rgba[o + 1] * wd) / (a * 255));
        cv.rgba[o + 2] = static_cast<uint8_t>((sb * wa + cv.rgba[o + 2] * wd) / (a * 255));
        cv.rgba[o + 3] = static_cast<uint8_t>(a);
    }
}

inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) | a;
}

// ---------------------------------------------------------------------------
// blit — copy a decoded RGBA frame at (dx,dy), skipping transparent pixels.
// `src` is sw*sh*4 bytes. Everything is clipped to the canvas.
// ---------------------------------------------------------------------------
inline void blit(Canvas& cv, int dx, int dy, const uint8_t* src, int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < sh; ++y) {
        const int cy = dy + y;
        if (cy < 0 || cy >= cv.h) continue;
        for (int x = 0; x < sw; ++x) {
            const int cx = dx + x;
            if (cx < 0 || cx >= cv.w) continue;
            const size_t so = (static_cast<size_t>(y) * sw + x) * 4;
            const uint8_t a = src[so + 3];
            if (a == 0) continue;
            putPixel(cv, cx, cy, packRGBA(src[so], src[so + 1], src[so + 2], a));
        }
    }
}

// ---------------------------------------------------------------------------
// drawRect — hollow rectangle outline, `thickness` px, clipped.
// For dashed panels we pass `dashed=true` (skip every other 4px run).
// ---------------------------------------------------------------------------
inline void drawRect(Canvas& cv, const ui_layout::Rect& r, uint32_t c,
                     int thickness = 1, bool dashed = false) {
    const int x0 = r.x, y0 = r.y, x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    auto span = [&](int x, int y, int len, bool horiz) {
        for (int i = 0; i < len; ++i) {
            if (dashed && ((i / 4) & 1)) continue; // 4px dash / 4px gap
            for (int t = 0; t < thickness; ++t)
                putPixel(cv, horiz ? x + i : x, horiz ? y : y + i, c);
        }
    };
    span(x0, y0, r.w, true);
    span(x0, y1, r.w, true);
    span(x0, y0, r.h, false);
    span(x1, y0, r.h, false);
}

// ---------------------------------------------------------------------------
// Minimal 5x7 bitmap font for the debug NUMBER TAGS only (NOT the game font).
// Covers '0'..'9', 'A'..'Z', 'T', '-', ':', ' '. Each glyph = 5 bytes (columns),
// bit 0 = top row. This is intentionally a debug annotation, documented as such.
// ---------------------------------------------------------------------------
namespace detail {

// 5 columns per glyph; each byte is a column bitmap (bit r = row r).
struct Glyph { char ch; uint8_t col[5]; };

inline const std::vector<Glyph>& font() {
    static const std::vector<Glyph> f = {
        {'0',{0x1F,0x11,0x11,0x11,0x1F}}, {'1',{0x00,0x00,0x1F,0x00,0x00}},
        {'2',{0x1D,0x15,0x15,0x15,0x17}}, {'3',{0x15,0x15,0x15,0x15,0x0F}},
        {'4',{0x07,0x05,0x05,0x05,0x1F}}, {'5',{0x17,0x15,0x15,0x15,0x0D}},
        {'6',{0x1F,0x15,0x15,0x15,0x0D}}, {'7',{0x01,0x01,0x01,0x01,0x1F}},
        {'8',{0x1F,0x15,0x15,0x15,0x1F}}, {'9',{0x07,0x15,0x15,0x15,0x1F}},
        {'A',{0x1E,0x05,0x05,0x05,0x1E}}, {'B',{0x1F,0x15,0x15,0x15,0x0A}},
        {'C',{0x0E,0x11,0x11,0x11,0x0A}}, {'D',{0x1F,0x11,0x11,0x11,0x0E}},
        {'E',{0x1F,0x15,0x15,0x15,0x11}}, {'F',{0x1F,0x05,0x05,0x05,0x01}},
        {'G',{0x0E,0x11,0x15,0x15,0x0D}}, {'H',{0x1F,0x04,0x04,0x04,0x1F}},
        {'I',{0x11,0x11,0x1F,0x11,0x11}}, {'J',{0x08,0x10,0x10,0x10,0x0F}},
        {'K',{0x1F,0x04,0x0A,0x11,0x00}}, {'L',{0x1F,0x10,0x10,0x10,0x10}},
        {'M',{0x1F,0x02,0x04,0x02,0x1F}}, {'N',{0x1F,0x02,0x04,0x08,0x1F}},
        {'O',{0x0E,0x11,0x11,0x11,0x0E}}, {'P',{0x1F,0x05,0x05,0x05,0x02}},
        {'Q',{0x0E,0x11,0x15,0x09,0x16}}, {'R',{0x1F,0x05,0x05,0x0D,0x12}},
        {'S',{0x12,0x15,0x15,0x15,0x09}}, {'T',{0x01,0x01,0x1F,0x01,0x01}},
        {'U',{0x0F,0x10,0x10,0x10,0x0F}}, {'V',{0x07,0x08,0x10,0x08,0x07}},
        {'W',{0x1F,0x08,0x04,0x08,0x1F}}, {'X',{0x11,0x0A,0x04,0x0A,0x11}},
        {'Y',{0x01,0x02,0x1C,0x02,0x01}}, {'Z',{0x11,0x19,0x15,0x13,0x11}},
        {'-',{0x04,0x04,0x04,0x04,0x04}}, {':',{0x00,0x0A,0x00,0x0A,0x00}},
        {' ',{0x00,0x00,0x00,0x00,0x00}},
    };
    return f;
}

inline const uint8_t* glyph(char ch) {
    for (const auto& g : font())
        if (g.ch == ch) return g.col;
    return nullptr;
}

} // namespace detail

// Draw a short ASCII label with the built-in 5x7 font at (x,y). `scale` is the
// integer pixel scale. `bg` (if a>=1) paints an opaque backing block first.
inline void drawLabel(Canvas& cv, int x, int y, const std::string& text,
                      uint32_t fg, uint32_t bg, int scale = 1) {
    const int gw = 6 * scale;             // 5 columns + 1 spacing
    const int gh = 7 * scale;
    if (bg & 0xFFu) {
        for (int j = -1; j <= gh; ++j)
            for (int i = -1; i < static_cast<int>(text.size()) * gw; ++i)
                putPixel(cv, x + i, y + j, bg);
    }
    for (size_t k = 0; k < text.size(); ++k) {
        const uint8_t* g = detail::glyph(text[k]);
        if (!g) continue;
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (g[col] & (1u << row))
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx)
                            putPixel(cv, x + static_cast<int>(k) * gw + col * scale + sx,
                                     y + row * scale + sy, fg);
    }
}

// ---------------------------------------------------------------------------
// rasterizeOverlay — transparent canvas with panel (dashed gray) + element
// rectangles (per-type colour) and optional number tags in the top-left corner.
// Number tags are a debugging annotation in a built-in 5x7 font (NOT the game
// bitmap font). Boxes extending beyond the canvas are clipped by putPixel.
// ---------------------------------------------------------------------------
inline Canvas rasterizeOverlay(int W, int H,
                               const std::vector<ui_layout::OverlayBox>& boxes,
                               bool withNumbers = true, bool withPanels = true) {
    Canvas cv(W, H, 0x00000000u); // fully transparent
    const uint32_t tagFg = packRGBA(255, 235, 120, 255);
    const uint32_t tagBg = packRGBA(0, 0, 0, 220);
    for (const auto& b : boxes) {
        if (b.is_panel) {
            if (withPanels) drawRect(cv, b.rect, b.rgba, 1, /*dashed=*/true);
            continue;
        }
        drawRect(cv, b.rect, b.rgba, 1, false);
        if (withNumbers && !b.tag.empty()) {
            const int tx = b.rect.x + 2;
            const int ty = b.rect.y + 2;
            drawLabel(cv, tx, ty, b.tag, tagFg, tagBg, 1);
        }
    }
    return cv;
}

} // namespace ui_render
