#include "hg/prompt_cells.h"

#include "hg/prompt_art.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

namespace hg::prompt_cells
{
    namespace
    {
        namespace art = hg::prompt_art;

        constexpr int N = kCellSize;
        constexpr int kTexels = N * N;
        constexpr int kSub = 8; // supersamples per axis for shapes

        struct Rgb
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
        };

        Rgb grey(int v)
        {
            const float f = static_cast<float>(v) / 255.0f;
            return {f, f, f};
        }

        // Premultiplied RGBA, 0..1, 16x16.
        struct Canvas
        {
            float px[kTexels * 4] = {};

            void paint(const float *cov, Rgb c, float alpha = 1.0f)
            {
                for (int i = 0; i < kTexels; ++i)
                {
                    const float a = std::clamp(cov[i] * alpha, 0.0f, 1.0f);
                    if (a <= 0.0f)
                        continue;
                    float *d = &px[i * 4];
                    d[0] = c.r * a + d[0] * (1.0f - a);
                    d[1] = c.g * a + d[1] * (1.0f - a);
                    d[2] = c.b * a + d[2] * (1.0f - a);
                    d[3] = a + d[3] * (1.0f - a);
                }
            }

            // Vertical gradient top -> bottom between y0 and y1.
            void paintGradient(const float *cov, Rgb top, Rgb bottom, float y0, float y1)
            {
                for (int y = 0; y < N; ++y)
                {
                    const float t = std::clamp((static_cast<float>(y) + 0.5f - y0) / std::max(1.0f, y1 - y0), 0.0f, 1.0f);
                    const Rgb c{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t, top.b + (bottom.b - top.b) * t};
                    float row[kTexels] = {};
                    std::memcpy(row + y * N, cov + y * N, sizeof(float) * N);
                    paint(row, c);
                }
            }
        };

        template <class Inside>
        void cover(float *cov, Inside inside)
        {
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    int hits = 0;
                    for (int sy = 0; sy < kSub; ++sy)
                        for (int sx = 0; sx < kSub; ++sx)
                            hits += inside(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSub,
                                           static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSub)
                                        ? 1
                                        : 0;
                    cov[y * N + x] = static_cast<float>(hits) / static_cast<float>(kSub * kSub);
                }
        }

        // Rounded rectangle with one radius per corner (top-left, top-right, bottom-right, bottom-left).
        auto roundRect(float x0, float y0, float x1, float y1, float tl, float tr, float br, float bl)
        {
            return [=](float x, float y) {
                if (x < x0 || x > x1 || y < y0 || y > y1)
                    return false;
                const auto corner = [&](float cx, float cy, float r) {
                    const float dx = x - cx, dy = y - cy;
                    return dx * dx + dy * dy <= r * r;
                };
                if (x < x0 + tl && y < y0 + tl)
                    return corner(x0 + tl, y0 + tl, tl);
                if (x > x1 - tr && y < y0 + tr)
                    return corner(x1 - tr, y0 + tr, tr);
                if (x > x1 - br && y > y1 - br)
                    return corner(x1 - br, y1 - br, br);
                if (x < x0 + bl && y > y1 - bl)
                    return corner(x0 + bl, y1 - bl, bl);
                return true;
            };
        }

        auto roundRect(float x0, float y0, float x1, float y1, float r)
        {
            return roundRect(x0, y0, x1, y1, r, r, r, r);
        }

        auto circle(float cx, float cy, float r)
        {
            return [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
        }

        struct Pt
        {
            float x, y;
        };

        bool inPolygon(const Pt *p, int n, float x, float y)
        {
            bool in = false;
            for (int i = 0, j = n - 1; i < n; j = i++)
                if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
                    in = !in;
            return in;
        }

        float segDist2(Pt a, Pt b, float x, float y)
        {
            const float vx = b.x - a.x, vy = b.y - a.y, wx = x - a.x, wy = y - a.y;
            const float l2 = vx * vx + vy * vy;
            const float t = l2 > 0.0f ? std::clamp((wx * vx + wy * vy) / l2, 0.0f, 1.0f) : 0.0f;
            const float dx = wx - t * vx, dy = wy - t * vy;
            return dx * dx + dy * dy;
        }

        // Open polyline of width w with square ends (end caps are clipped by the caller's design, not rounded).
        bool onPolyline(const Pt *p, int n, float w, float x, float y)
        {
            for (int i = 0; i + 1 < n; ++i)
            {
                // square stroke: within w/2 of the segment, measured as a box around it (axis-aligned segments)
                const float minx = std::min(p[i].x, p[i + 1].x) - 0.5f * w, maxx = std::max(p[i].x, p[i + 1].x) + 0.5f * w;
                const float miny = std::min(p[i].y, p[i + 1].y) - 0.5f * w, maxy = std::max(p[i].y, p[i + 1].y) + 0.5f * w;
                if (p[i].x == p[i + 1].x || p[i].y == p[i + 1].y)
                {
                    if (x >= minx && x <= maxx && y >= miny && y <= maxy)
                        return true;
                }
                else if (segDist2(p[i], p[i + 1], x, y) <= 0.25f * w * w)
                    return true;
            }
            return false;
        }

        void palette16(const uint8_t *clut, int csa, uint8_t rgba[16][4])
        {
            for (int i = 0; i < 16; ++i)
            {
                const int x = (i & 7) + (csa & 1) * 8;
                const int y = (i >> 3) + (csa >> 1) * 2;
                std::memcpy(rgba[i], clut + static_cast<size_t>(y * 16 + x) * 4u, 4);
            }
        }

        uint8_t texel(const uint8_t *image, int x, int y)
        {
            const size_t i = static_cast<size_t>(y) * kSheetWidth + static_cast<size_t>(x);
            return static_cast<uint8_t>((image[i >> 1] >> ((i & 1u) * 4u)) & 0xFu);
        }

        void setTexel(uint8_t *image, int x, int y, uint8_t v)
        {
            const size_t i = static_cast<size_t>(y) * kSheetWidth + static_cast<size_t>(x);
            const unsigned shift = static_cast<unsigned>((i & 1u) * 4u);
            image[i >> 1] = static_cast<uint8_t>((image[i >> 1] & ~(0xFu << shift)) | ((v & 0xFu) << shift));
        }

        // Canvas -> indices: transparent below half coverage (the palettes have only alpha 0 and 0x80), else the
        // nearest allowed entry to the un-premultiplied colour.
        void quantize(const Canvas &c, const uint8_t pal[16][4], const bool allowed[16], uint8_t out[kTexels])
        {
            for (int i = 0; i < kTexels; ++i)
            {
                const float a = c.px[i * 4 + 3];
                if (a < 0.5f)
                {
                    out[i] = 0;
                    continue;
                }
                const float r = c.px[i * 4] / a * 255.0f, g = c.px[i * 4 + 1] / a * 255.0f, b = c.px[i * 4 + 2] / a * 255.0f;
                float best = 1e30f;
                uint8_t bestIndex = 0;
                for (int k = 1; k < 16; ++k)
                {
                    if (!allowed[k])
                        continue;
                    // weighted RGB (eye: green most)
                    const float dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
                    const float d = 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
                    if (d < best)
                    {
                        best = d;
                        bestIndex = static_cast<uint8_t>(k);
                    }
                }
                out[i] = bestIndex;
            }
        }

        // ---- labels ----

        enum class Symbol
        {
            None,
            Enter,
            Backspace,
            Space,
            Shift,
            Tab,
            Caps,
            Menu // the keyboard's context-menu key ("Menu" is 4 letters: unreadable in 16 texels)
        };

        // What a 16-texel cap shows for a compact label: a key symbol, or text of at most 3 characters.
        Symbol symbolOf(std::string_view s)
        {
            if (s == "Ent" || s == "Enter" || s == "NEnt")
                return Symbol::Enter;
            if (s == "Bksp")
                return Symbol::Backspace;
            if (s == "Spc" || s == "Space")
                return Symbol::Space;
            if (s == "Shft" || s == "Shift" || s == "RShf")
                return Symbol::Shift;
            if (s == "Tab")
                return Symbol::Tab;
            if (s == "Caps")
                return Symbol::Caps;
            if (s == "Menu")
                return Symbol::Menu;
            return Symbol::None;
        }

        size_t glyphCount(std::string_view s)
        {
            size_t n = 0;
            for (unsigned char ch : s)
                n += (ch & 0xC0u) != 0x80u ? 1u : 0u;
            return n;
        }

        // At most three characters fit a 16-texel cell. Known four-letter names have a short form; anything else
        // (a name the runtime may add later) keeps its first three characters, never cutting a UTF-8 sequence.

        std::string microText(std::string_view s)
        {
            struct Short
            {
                std::string_view from, to;
            };
            static constexpr Short kShort[] = {
                {"Ctrl", "Ctl"}, {"RCtl", "RCt"}, {"RCtrl", "RCt"}, {"Paus", "Pau"}, {"NmLk", "NLk"}, {"ScLk", "SLk"},
                {"TPad", "TP"}, {"Touch", "TP"}, {"Misc", "Msc"}, {"RShf", "RSh"}, {"RAlt", "RAl"},
            };
            for (const Short &k : kShort)
                if (s == k.from)
                    return std::string(k.to);
            if (glyphCount(s) <= 3u)
                return std::string(s);
            size_t end = 0u, glyphs = 0u;
            while (end < s.size())
            {
                if ((static_cast<unsigned char>(s[end]) & 0xC0u) != 0x80u && ++glyphs > 3u)
                    break;
                ++end;
            }
            return std::string(s.substr(0, end));
        }

        // Text label (via prompt_art, GDI) into the box; false when GDI drew nothing.
        bool textLabel(Canvas &c, std::string_view text, float cx, float cy, float width, float cap, bool bold, Rgb colour)
        {
            art::TextBox box;
            box.cx = cx;
            box.cy = cy;
            box.width = width;
            box.capHeight = cap;
            box.weight = bold ? art::Weight::Bold : art::Weight::Semibold;
            box.snap = true;
            float cov[kTexels];
            if (art::textCoverage(text, box, N, N, cov) <= 0.0f)
                return false;
            c.paint(cov, colour);
            return true;
        }

        // Key symbols, drawn inside the cap face (x 2..14, y 1..12.5).
        void symbolLabel(Canvas &c, Symbol s, Rgb ink, Rgb face)
        {
            float cov[kTexels];
            switch (s)
            {
            case Symbol::Enter:
            {
                // the return arrow: down the right side, along the bottom to the left, head pointing left
                const Pt line[] = {{11.0f, 2.5f}, {11.0f, 8.0f}, {6.0f, 8.0f}};
                cover(cov, [&](float x, float y) { return onPolyline(line, 3, 1.6f, x, y); });
                c.paint(cov, ink);
                const Pt head[] = {{2.6f, 8.0f}, {6.6f, 4.4f}, {6.6f, 11.6f}};
                cover(cov, [&](float x, float y) { return inPolygon(head, 3, x, y); });
                c.paint(cov, ink);
                break;
            }
            case Symbol::Backspace:
            {
                // erase-to-the-left key: a filled tag pointing left with the cross cut out
                const Pt tag[] = {{2.0f, 6.75f}, {6.0f, 2.5f}, {14.0f, 2.5f}, {14.0f, 11.0f}, {6.0f, 11.0f}};
                cover(cov, [&](float x, float y) { return inPolygon(tag, 5, x, y); });
                c.paint(cov, ink);
                const Pt a0{7.3f, 4.6f}, a1{11.7f, 8.9f}, b0{11.7f, 4.6f}, b1{7.3f, 8.9f};
                cover(cov, [&](float x, float y) { return segDist2(a0, a1, x, y) <= 0.56f || segDist2(b0, b1, x, y) <= 0.56f; });
                c.paint(cov, face);
                break;
            }
            case Symbol::Space:
            {
                const Pt line[] = {{3.5f, 5.5f}, {3.5f, 9.0f}, {12.5f, 9.0f}, {12.5f, 5.5f}};
                cover(cov, [&](float x, float y) { return onPolyline(line, 4, 1.5f, x, y); });
                c.paint(cov, ink);
                break;
            }
            case Symbol::Shift:
            case Symbol::Caps:
            {
                // hollow up arrow (shift); with a bar under it (caps lock)
                const float yb = s == Symbol::Caps ? 8.0f : 10.5f;
                const Pt arrow[] = {{8.0f, 2.0f}, {13.0f, 7.0f}, {10.25f, 7.0f}, {10.25f, yb}, {5.75f, yb}, {5.75f, 7.0f}, {3.0f, 7.0f}};
                cover(cov, [&](float x, float y) {
                    if (!inPolygon(arrow, 7, x, y))
                        return false;
                    for (int i = 0, j = 6; i < 7; j = i++)
                        if (segDist2(arrow[j], arrow[i], x, y) <= 0.625f * 0.625f)
                            return true;
                    return false;
                });
                c.paint(cov, ink);
                if (s == Symbol::Caps)
                {
                    cover(cov, [&](float x, float y) { return x >= 5.75f && x <= 10.25f && y >= 9.5f && y <= 11.0f; });
                    c.paint(cov, ink);
                }
                break;
            }
            case Symbol::Tab:
            {
                const Pt shaft[] = {{2.5f, 6.75f}, {9.0f, 6.75f}};
                cover(cov, [&](float x, float y) { return onPolyline(shaft, 2, 1.5f, x, y); });
                c.paint(cov, ink);
                const Pt head[] = {{12.0f, 6.75f}, {8.3f, 3.2f}, {8.3f, 10.3f}};
                cover(cov, [&](float x, float y) { return inPolygon(head, 3, x, y); });
                c.paint(cov, ink);
                cover(cov, [&](float x, float y) { return x >= 12.25f && x <= 13.75f && y >= 3.0f && y <= 10.5f; });
                c.paint(cov, ink);
                break;
            }
            case Symbol::Menu:
                // three bars, as the Xbox Menu art, centred on the cap face
                cover(cov, [](float x, float y) {
                    if (x < 4.0f || x > 12.0f)
                        return false;
                    return (y >= 3.0f && y <= 4.5f) || (y >= 6.0f && y <= 7.5f) || (y >= 9.0f && y <= 10.5f);
                });
                c.paint(cov, ink);
                break;
            case Symbol::None:
                break;
            }
        }

        // ---- looks ----

        struct CapTones
        {
            int outline, lip, faceTop, faceBottom, rim, label;
        };
        // The approved keyboard look (2026-10-18): mid-grey caps, white labels (preview variant "keys").
        constexpr CapTones kMidCap{0x40, 0x56, 0x7C, 0x70, 0xA8, 0xFF};

        // A keyboard key cap filling the cell: dark outline, lip below, lit face with a bright top rim, label.
        bool drawKeyCap(Canvas &c, std::string_view text, const uint8_t pal[16][4], bool allowed[16])
        {
            const CapTones t = kMidCap;
            float cov[kTexels], tmp[kTexels];
            cover(cov, roundRect(0.0f, 0.0f, 16.0f, 16.0f, 3.5f));
            c.paint(cov, grey(t.outline));
            cover(cov, roundRect(1.0f, 1.0f, 15.0f, 15.0f, 2.5f));
            c.paint(cov, grey(t.lip));
            const float fx0 = 1.5f, fy0 = 1.0f, fx1 = 14.5f, fy1 = 12.5f;
            cover(cov, roundRect(fx0, fy0, fx1, fy1, 2.0f));
            c.paintGradient(cov, grey(t.faceTop), grey(t.faceBottom), fy0, fy1);
            cover(tmp, roundRect(fx0, fy0 + 1.0f, fx1, fy1 + 1.0f, 2.0f));
            for (int i = 0; i < kTexels; ++i)
                tmp[i] = std::max(0.0f, cov[i] - tmp[i]);
            c.paint(tmp, grey(t.rim), 0.85f);

            for (int k = 1; k < 16; ++k)
                allowed[k] = pal[k][0] == pal[k][1] && pal[k][1] == pal[k][2]; // the grey ramp
            const Rgb ink = grey(t.label);
            const Symbol symbol = symbolOf(text);
            const Rgb face = grey((t.faceTop + t.faceBottom) / 2);
            if (symbol != Symbol::None)
            {
                symbolLabel(c, symbol, ink, face);
                return true;
            }
            const std::string micro = microText(text);
            const size_t n = glyphCount(micro);
            const float cap = n <= 1 ? 9.0f : (n == 2 ? 8.0f : 7.0f);
            if (micro == "M1" || micro == "M2" || micro == "M3") // prompt_art draws a mouse with that button lit (box >= 10 tall)
                return textLabel(c, micro, 8.0f, 0.5f * (fy0 + fy1), 8.0f, 10.0f, true, ink);
            return textLabel(c, micro, 8.0f, 0.5f * (fy0 + fy1), 12.0f, cap, true, ink);
        }

        bool isGrey(const uint8_t e[4])
        {
            return e[0] == e[1] && e[1] == e[2];
        }

        // ---- Xbox-style art. The shape follows the bound element (ps2_host_input::prompts Binding::compact), the
        // cell only decides the palette: A/B/X/Y round face button, LB/RB bumper, LT/RT trigger, View, Menu, anything
        // else (LS, RS, D-pad, paddles, stick halves) its text on a round button. ----

        enum class PadShape
        {
            Face,
            Bumper,
            Trigger,
            View,
            Menu,
            Other
        };

        PadShape padShape(std::string_view s)
        {
            if (s == "A" || s == "B" || s == "X" || s == "Y")
                return PadShape::Face;
            if (s == "LB" || s == "RB")
                return PadShape::Bumper;
            if (s == "LT" || s == "RT" || s == "LT-" || s == "RT-")
                return PadShape::Trigger;
            if (s == "View")
                return PadShape::View;
            if (s == "Menu")
                return PadShape::Menu;
            return PadShape::Other;
        }

        // The game's own round button (palettes 4/5 face cells) with the PS symbol (indices 11-15) painted out from
        // its neighbours, outside in.
        void ownRoundButton(Canvas &c, const uint8_t *origIdx, const uint8_t pal[16][4])
        {
            float col[kTexels][3];
            bool known[kTexels];
            for (int i = 0; i < kTexels; ++i)
            {
                const uint8_t v = origIdx[i];
                known[i] = v <= 10;
                for (int ch = 0; ch < 3; ++ch)
                    col[i][ch] = pal[v][ch] / 255.0f;
            }
            for (int pass = 0; pass < 32; ++pass)
            {
                bool any = false;
                bool next[kTexels];
                std::memcpy(next, known, sizeof(next));
                for (int y = 0; y < N; ++y)
                    for (int x = 0; x < N; ++x)
                    {
                        const int i = y * N + x;
                        if (known[i])
                            continue;
                        float sum[3] = {}, w = 0.0f;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int xx = x + dx, yy = y + dy;
                                if ((dx || dy) && xx >= 0 && yy >= 0 && xx < N && yy < N && known[yy * N + xx] && origIdx[yy * N + xx] != 0)
                                {
                                    for (int ch = 0; ch < 3; ++ch)
                                        sum[ch] += col[yy * N + xx][ch];
                                    w += 1.0f;
                                }
                            }
                        if (w > 0.0f)
                        {
                            for (int ch = 0; ch < 3; ++ch)
                                col[i][ch] = sum[ch] / w;
                            next[i] = true;
                            any = true;
                        }
                    }
                std::memcpy(known, next, sizeof(known));
                if (!any)
                    break;
            }
            for (int i = 0; i < kTexels; ++i)
            {
                if (origIdx[i] == 0)
                    continue;
                c.px[i * 4] = col[i][0];
                c.px[i * 4 + 1] = col[i][1];
                c.px[i * 4 + 2] = col[i][2];
                c.px[i * 4 + 3] = 1.0f;
            }
            // a little darker in the middle so a light letter reads (the PS face is mid grey under a coloured symbol)
            float cov[kTexels];
            cover(cov, circle(7.75f, 7.5f, 5.6f));
            c.paint(cov, grey(0x51), 0.55f);
        }

        // The approved Xbox look (2026-10-18, preview variant "xbox"): every letter and symbol white.
        bool drawXbox(Canvas &c, int cell, std::string_view text, const uint8_t *origIdx, const uint8_t pal[16][4], bool allowed[16])
        {
            for (int k = 1; k < 16; ++k)
                allowed[k] = isGrey(pal[k]);
            const Rgb outline = grey(0x2A), top = grey(0x7E), bottom = grey(0x4B), rim = grey(0xB1), ink = grey(0xFF);
            float cov[kTexels], tmp[kTexels];
            const auto body = [&](auto shape, auto inner, float y0, float y1) {
                cover(cov, shape);
                c.paint(cov, outline);
                cover(cov, inner);
                c.paintGradient(cov, top, bottom, y0, y1);
                for (int y = 1; y < N; ++y)
                    for (int x = 0; x < N; ++x)
                        tmp[y * N + x] = std::max(0.0f, cov[y * N + x] - cov[(y - 1) * N + x]);
                for (int x = 0; x < N; ++x)
                    tmp[x] = cov[x];
                c.paint(tmp, rim, 0.6f); // top rim
            };
            const auto roundButton = [&] { body(circle(8.0f, 8.0f, 8.0f), circle(8.0f, 8.0f, 7.0f), 1.0f, 15.0f); };
            const bool left = !text.empty() && text[0] == 'L';
            switch (padShape(text))
            {
            case PadShape::Face:
            {
                const bool own = kCellCsa[cell] != 6; // the face cells keep the game's own button
                if (own)
                    ownRoundButton(c, origIdx, pal);
                else
                    roundButton();
                return own ? textLabel(c, text, 7.75f, 7.25f, 9.0f, 8.0f, true, ink) : textLabel(c, text, 8.0f, 8.0f, 9.0f, 8.0f, true, ink);
            }
            case PadShape::Bumper:
            {
                // wide, the outer top corner strongly rounded
                const float big = 5.5f, small = 1.5f;
                body(roundRect(0.0f, 2.5f, 16.0f, 13.5f, left ? big : small, left ? small : big, small, small),
                     roundRect(1.0f, 3.5f, 15.0f, 12.5f, left ? big - 1.0f : small - 0.5f, left ? small - 0.5f : big - 1.0f, small - 0.5f, small - 0.5f),
                     3.5f, 12.5f);
                return textLabel(c, text, 8.0f, 8.25f, 12.5f, 6.0f, true, ink);
            }
            case PadShape::Trigger:
                // tall, rounded top
                body(roundRect(1.5f, 0.0f, 14.5f, 16.0f, 5.5f, 5.5f, 1.5f, 1.5f), roundRect(2.5f, 1.0f, 13.5f, 15.0f, 4.5f, 4.5f, 1.0f, 1.0f), 1.0f, 15.0f);
                return textLabel(c, text.substr(0, 2), 8.0f, 9.0f, 10.0f, 6.0f, true, ink);
            case PadShape::View:
                // two overlapping windows: the back one as an outline, the front one filled
                roundButton();
                cover(cov, [](float x, float y) {
                    const bool outer = x >= 3.5f && x <= 10.0f && y >= 3.5f && y <= 10.0f;
                    const bool inner = x >= 4.75f && x <= 8.75f && y >= 4.75f && y <= 8.75f;
                    return outer && !inner;
                });
                c.paint(cov, ink);
                cover(cov, [](float x, float y) { return x >= 5.75f && x <= 13.25f && y >= 5.75f && y <= 13.25f; });
                c.paint(cov, grey(0x4B));
                cover(cov, [](float x, float y) { return x >= 7.0f && x <= 12.5f && y >= 7.0f && y <= 12.5f; });
                c.paint(cov, ink);
                return true;
            case PadShape::Menu:
                roundButton();
                cover(cov, [](float x, float y) {
                    if (x < 4.0f || x > 12.0f)
                        return false;
                    return (y >= 4.5f && y <= 6.0f) || (y >= 7.25f && y <= 8.75f) || (y >= 10.0f && y <= 11.5f);
                });
                c.paint(cov, ink);
                return true;
            case PadShape::Other:
            {
                roundButton();
                const std::string micro = microText(text); // "Misc" -> "Msc", "TPad" -> "TP"; at most 3 characters
                return textLabel(c, micro, 8.0f, 8.0f, 11.0f, glyphCount(micro) <= 1 ? 8.0f : 6.0f, true, ink);
            }
            }
            return false;
        }
    }

    void palette(const uint8_t *clut, int csa, uint8_t rgba[16][4])
    {
        palette16(clut, csa, rgba);
    }

    void cellIndices(const uint8_t *image, int cell, uint8_t indices[kCellSize * kCellSize])
    {
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                indices[y * N + x] = texel(image, cell * N + x, kIconRow + y);
    }

    void cellBytes(const uint8_t *image, uint8_t out[kCellBlockBytes])
    {
        const size_t width = kCellRowBytes * CellCount; // 80
        for (int y = 0; y < N; ++y)
            std::memcpy(out + static_cast<size_t>(y) * width, image + static_cast<size_t>(kIconRow + y) * kRowBytes, width);
    }

    void putCellBytes(uint8_t *image, const uint8_t block[kCellBlockBytes])
    {
        const size_t width = kCellRowBytes * CellCount;
        for (int y = 0; y < N; ++y)
            std::memcpy(image + static_cast<size_t>(kIconRow + y) * kRowBytes, block + static_cast<size_t>(y) * width, width);
    }

    bool cellsEqual(const uint8_t *image, const uint8_t block[kCellBlockBytes])
    {
        const size_t width = kCellRowBytes * CellCount;
        for (int y = 0; y < N; ++y)
            if (std::memcmp(image + static_cast<size_t>(kIconRow + y) * kRowBytes, block + static_cast<size_t>(y) * width, width) != 0)
                return false;
        return true;
    }

    uint64_t fnv1a64(const uint8_t *data, size_t size, uint64_t seed)
    {
        uint64_t h = seed;
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 0x100000001B3ull;
        return h;
    }

    uint64_t fnvOutsideCells(const uint8_t *image)
    {
        const size_t cellsWidth = kCellRowBytes * CellCount; // 80
        uint64_t h = 0xCBF29CE484222325ull;
        for (int row = 0; row < kSheetWidth; ++row)
        {
            const uint8_t *r = image + static_cast<size_t>(row) * kRowBytes;
            if (row >= kIconRow && row < kIconRow + kCellSize)
                h = fnv1a64(r + cellsWidth, kRowBytes - cellsWidth, h);
            else
                h = fnv1a64(r, kRowBytes, h);
        }
        return h;
    }

    bool buildCells(const uint8_t *origImage, const uint8_t *clut, const Labels &labels, uint8_t *outImage)
    {
        if (!origImage || !clut || !outImage)
            return false;
        std::vector<uint8_t> work(origImage, origImage + kImageBytes);
        for (int cell = 0; cell < CellCount; ++cell)
        {
            const std::string &text = labels.text[cell];
            if (text.empty())
                continue;
            uint8_t pal[16][4];
            palette16(clut, kCellCsa[cell], pal);
            uint8_t orig[kTexels];
            cellIndices(origImage, cell, orig);
            Canvas c;
            bool allowed[16] = {};
            bool ok = false;
            if (labels.style == Style::Keys)
                ok = drawKeyCap(c, text, pal, allowed);
            else
                ok = drawXbox(c, cell, text, orig, pal, allowed);
            if (!ok)
                return false;
            uint8_t idx[kTexels];
            quantize(c, pal, allowed, idx);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                    setTexel(work.data(), cell * N + x, kIconRow + y, idx[y * N + x]);
        }
        std::memcpy(outImage, work.data(), kImageBytes);
        return true;
    }
}
