#version 440

// Waveform rendering. Peaks live in a texture and the GPU does the decimation.
//
// Phase 0 measured this against Canvas 2D and Qt Quick Shapes on the target machine
// (docs/phase0-results.md): Canvas dropped ~5% of frames at every zoom, Shapes cost up
// to 8ms of UI thread per frame, and this costs no measurable UI time and does not
// care how far out the view is zoomed.
//
// Texture layout is written by io/peak_texture.cpp: RGBA8, two texels per peak -- the
// first holds the 16-bit max in R/G, the second the min -- in `1 + channels` blocks
// of `peakCount` peaks: the combined envelope, then each channel on its own. Nothing is read from alpha,
// because Qt premultiplies it on upload and would scale the data. Texels wrap across
// rows because a 30-minute take has more peaks than the maximum texture width.
//
// texelFetch does not exist in GLSL ES 100, so this is baked for "300 es,150" and
// there is no GLES2 fallback. Irrelevant on the desktop target in BRIEF.md section 10.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float offset;           // peak index at the left edge
    float peaksPerPixel;
    float itemWidth;
    float texWidth;
    float texHeight;
    float peakCount;
    float block;            // 0 = combined envelope, 1..n = one channel (Phase 2)
    vec4 waveColor;
};

layout(binding = 1) uniform sampler2D peakTex;

float decode16(float hi, float lo) {
    float q = hi * 255.0 * 256.0 + lo * 255.0;
    return (q / 65535.0) * 2.0 - 1.0;
}

void main() {
    // A retired generation resolves to a transparent 1x1 image. Never read it
    // using the dimensions of its previous waveform model.
    if (texWidth < 1.0 || texHeight < 1.0
        || any(notEqual(textureSize(peakTex, 0), ivec2(texWidth, texHeight)))) {
        fragColor = vec4(0.0);
        return;
    }
    float column = floor(qt_TexCoord0.x * itemWidth);
    float start = offset + column * peaksPerPixel;
    float end = start + peaksPerPixel;
    // Blocks of peakCount peaks: the combined envelope first, then each channel.
    float blockBase = block * peakCount;

    float lo = 1.0;
    float hi = -1.0;
    bool any = false;

    // Bounded tap count. Phase 2's multi-resolution peak cache keeps peaksPerPixel
    // inside this bound at every zoom level (BRIEF.md section 3, powers of 8), which
    // is why 8 is the ceiling rather than a guess. Phase 1 has one level, so a fully
    // zoomed-out view undersamples rather than stalling -- a deliberate trade until
    // the pyramid exists.
    for (int t = 0; t < 8; ++t) {
        float idx = floor(start) + float(t) * max(1.0, peaksPerPixel / 8.0);
        if (idx >= end || idx >= peakCount || idx < 0.0)
            break;

        int maxTexel = int(blockBase + idx) * 2;
        int minTexel = maxTexel + 1;
        int w = int(texWidth);

        vec4 hiTexel = texelFetch(peakTex, ivec2(maxTexel % w, maxTexel / w), 0);
        vec4 loTexel = texelFetch(peakTex, ivec2(minTexel % w, minTexel / w), 0);

        hi = max(hi, decode16(hiTexel.r, hiTexel.g));
        lo = min(lo, decode16(loTexel.r, loTexel.g));
        any = true;
    }

    if (!any) {
        fragColor = vec4(0.0);
        return;
    }

    // Item-space amplitude at this fragment.
    float amp = (0.5 - qt_TexCoord0.y) / 0.46;

    // Half-pixel feather so quiet passages stay visible and do not shimmer while
    // scrolling. Room tone is near-silent but should still draw a line.
    float halfPx = (1.0 / max(1.0, itemWidth)) * 0.5 + 0.005;
    float inside = smoothstep(lo - halfPx, lo + halfPx, amp)
                 * (1.0 - smoothstep(hi - halfPx, hi + halfPx, amp));

    fragColor = waveColor * inside * qt_Opacity;
}
