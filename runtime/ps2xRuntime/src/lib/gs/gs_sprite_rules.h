#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct GSSpriteAxis
{
    int first = 0;
    int last = -1;
    float origin = 0.0f;
    float scale = 1.0f;
};

inline GSSpriteAxis gsSpriteAxis(float p0, float p1, float offset)
{
    GSSpriteAxis axis;
    axis.origin = p0 - offset;
    const float end = p1 - offset;
    const float extent = end - axis.origin;
    axis.first = static_cast<int>(std::ceil(std::min(axis.origin, end)));
    axis.last = static_cast<int>(std::ceil(std::max(axis.origin, end))) - 1;
    axis.scale = extent != 0.0f ? 1.0f / extent : 0.0f;
    return axis;
}

inline float gsSpriteParameter(const GSSpriteAxis &axis, int pixel)
{
    return (static_cast<float>(pixel) - axis.origin) * axis.scale;
}

// One axis of a textured FST sprite under a bilinear TEX1: true when every pixel in [first, last] reads texel
// (pixel + c) with a GS filter weight of 0. The GS samples pixel p at p, interpolates the texture coordinate and keeps
// 4 fraction bits of U - 0.5 (truncated): weight 0 makes its bilinear output the top-left texel alone, which is also
// its nearest output (floor(U)). p0/p1: the two vertices minus XYOFFSET in 1/16 pixel; t0/t1: their UV in 1/16 texel.
// floor(16U) - 16p is monotone in p, so the same value at both ends holds for every pixel in between.
inline bool gsSpriteAxisZeroWeight(int64_t p0, int64_t p1, int64_t t0, int64_t t1, int first, int last)
{
    const int64_t den = p1 - p0;
    if (den == 0 || first > last)
        return false;
    const auto offset = [&](int64_t p)
    {
        int64_t num = t0 * den + (t1 - t0) * (p * 16 - p0);
        int64_t d = den;
        if (d < 0)
        {
            num = -num;
            d = -d;
        }
        const int64_t u16 = num >= 0 ? num / d : -((-num + d - 1) / d); // floor(16 U)
        return u16 - 8 - p * 16;                                          // 16 (texel - p) + weight
    };
    const int64_t first16 = offset(first);
    return first16 == offset(last) && (first16 & 15) == 0;
}
