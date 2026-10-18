#version 440

// Spectrogram tile rendering. One tile of the spectral cache is a greyscale texture,
// width = STFT columns (time), height = bins (row 0 is DC, the last row Nyquist), one
// byte per texel holding dB from floorDb (0) to 0 dBFS (255); see
// engine/src/spectral/tiles.h. The GPU does the time decimation and the frequency
// mapping, so the same tile draws linear or logarithmic, at any zoom, in any palette,
// without touching the cache -- which is what "smooth zoom without rebuilding the
// display synchronously" in brief section 8 comes down to.
//
// Baked for "300 es,150" like wave.frag: texelFetch is what makes the decimation exact.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float columns;        // texture width
    float bins;           // texture height
    float itemWidth;
    float itemHeight;
    float logScale;       // 0 = linear, 1 = logarithmic
    float minHz;          // bottom of the log scale
    float nyquist;
    float floorDb;        // what texel 0 means (-120)
    float rangeDb;        // the palette spans -rangeDb .. 0 dB
    float gainDb;         // brightness, added before the palette
    vec4 c0;              // palette stops, darkest first
    vec4 c1;
    vec4 c2;
    vec4 c3;
    vec4 c4;
};

layout(binding = 1) uniform sampler2D tileTex;

float binAt(float up) {
    // `up` is 0 at the bottom of the lane, 1 at the top.
    float hz;
    if (logScale > 0.5)
        hz = minHz * pow(nyquist / minHz, up);
    else
        hz = up * nyquist;
    return hz / nyquist * (bins - 1.0);
}

// The stops sit high: most of a mix lives 30-60 dB under full scale and should
// read as the dark end of the ramp, with the bright colours kept for what is
// actually loud. Uniform stops paint everything cyan.
vec4 palette(float t) {
    if (t < 0.45) return mix(c0, c1, t / 0.45);
    if (t < 0.72) return mix(c1, c2, (t - 0.45) / 0.27);
    if (t < 0.9)  return mix(c2, c3, (t - 0.72) / 0.18);
    return mix(c3, c4, (t - 0.9) / 0.1);
}

void main() {
    // Obsolete model requests carry a 1x1 transparent placeholder, not a tile.
    if (columns < 1.0 || bins < 2.0
        || any(notEqual(textureSize(tileTex, 0), ivec2(columns, bins)))) {
        fragColor = vec4(0.0);
        return;
    }
    // Time: the columns this pixel spans, at most 8 taps (the pyramid keeps
    // columns-per-pixel under 8 at every zoom).
    float columnsPerPixel = columns / max(1.0, itemWidth);
    float startCol = qt_TexCoord0.x * columns;
    float endCol = startCol + columnsPerPixel;
    float colStep = max(1.0, columnsPerPixel / 8.0);

    // Frequency: the bins between this pixel row and the next, at most 8 taps.
    float up0 = 1.0 - qt_TexCoord0.y;
    float up1 = up0 + 1.0 / max(1.0, itemHeight);
    float bin0 = binAt(clamp(up0, 0.0, 1.0));
    float bin1 = binAt(clamp(up1, 0.0, 1.0));
    float binStep = max(1.0, (bin1 - bin0) / 8.0);

    float best = 0.0;
    int w = int(columns);
    int h = int(bins);
    for (int t = 0; t < 8; ++t) {
        float col = floor(startCol) + float(t) * colStep;
        if (col >= endCol || col >= columns)
            break;
        int ci = clamp(int(col), 0, w - 1);
        for (int u = 0; u < 8; ++u) {
            float bin = floor(bin0) + float(u) * binStep;
            if (u > 0 && bin > bin1)
                break;
            int bi = clamp(int(bin), 0, h - 1);
            best = max(best, texelFetch(tileTex, ivec2(ci, bi), 0).r);
        }
    }

    float db = floorDb + best * (0.0 - floorDb) + gainDb;
    float t = clamp((db + rangeDb) / rangeDb, 0.0, 1.0);
    fragColor = palette(t) * qt_Opacity;
}
