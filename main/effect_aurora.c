#include "effects.h"
#include "render.h"
#include "cube.h"

#include <math.h>
#include <stdint.h>

// Aurora borealis: smooth green/cyan/violet bands sliding slowly across
// the TOP face, spilling gently onto the upper few rows of the four side
// faces so the glow appears to be "hanging off" the top of the cube.
// Intensity is shaped by two overlapping low-frequency sine waves; the
// hue also wobbles within the classic aurora palette. Face-local, not
// orientation-aware — in corner-up mode the glow still sits on TOP but
// still reads as aurora because it's on a wall tilted toward the viewer.

static float s_phase;

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b) {
    h = h - floorf(h);
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    int seg = (int)(h * 6.0f);
    switch (seg) {
        case 0: rp = c; gp = x; bp = 0; break;
        case 1: rp = x; gp = c; bp = 0; break;
        case 2: rp = 0; gp = c; bp = x; break;
        case 3: rp = 0; gp = x; bp = c; break;
        case 4: rp = x; gp = 0; bp = c; break;
        default:rp = c; gp = 0; bp = x; break;
    }
    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

static void aurora_enter(void) { s_phase = 0; }

// Sample aurora intensity and hue at a point (u, v) where u,v ∈ [0, 1].
// u is "across" the aurora, v is "along" the ribbon.
static void sample_aurora(float u, float v, float phase,
                          float *intensity, float *hue) {
    // Two ribbon bands: one travelling east, one travelling south-east,
    // both with slow phase drift. Their product + sum gives sheet-like
    // banding that morphs over time.
    float band1 = sinf((v * 2.3f + phase * 0.5f) * 6.2832f);
    float band2 = sinf((v * 1.7f + u * 0.9f - phase * 0.3f) * 6.2832f);
    // Combine: peaks where either band is near +1.
    float h = 0.5f * (band1 + band2);
    float envelope = 0.5f + 0.5f * sinf((u * 1.1f + phase * 0.2f) * 6.2832f);
    float it = fmaxf(0.0f, h) * envelope;
    it = it * it; // sharpen the ribbon edges
    *intensity = it;

    // Aurora palette: hue 0.33 (green) .. 0.55 (cyan) .. 0.75 (violet).
    float hue_wobble = 0.33f + 0.42f * (0.5f + 0.5f * sinf((v * 1.3f + phase * 0.4f) * 6.2832f));
    *hue = hue_wobble;
}

static void aurora_step(float dt) {
    config_lock();
    uint8_t speed = config_get()->rainbow_speed;
    config_unlock();
    s_phase += dt * (0.08f + speed / 255.0f * 0.20f);

    // Draw on TOP first.
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float u = x / 7.0f;
            float v = y / 7.0f;
            float it, hue;
            sample_aurora(u, v, s_phase, &it, &hue);
            if (it < 0.02f) continue;
            uint8_t r, g, b;
            hsv_to_rgb(hue, 0.95f, it * 0.95f, &r, &g, &b);
            render_set(FACE_TOP, x, y, r, g, b);
        }
    }

    // Spill onto the top 3 rows of each side face with a linear fade.
    // Each side samples the aurora at a u/v that continues naturally from
    // the TOP face so the ribbon reads as continuous across the seam.
    for (int f = 0; f < CUBE_FACE_COUNT; f++) {
        if (f == FACE_TOP || f == FACE_BOTTOM) continue;
        for (int y = 0; y < 3; y++) {
            float row_fade = 1.0f - (y / 3.0f); // brightest at y=0 (near TOP seam)
            row_fade *= row_fade;
            for (int x = 0; x < 8; x++) {
                // Per-face u/v mapping. It doesn't have to be exactly continuous
                // with TOP — any offset just shifts the ribbon slightly, which
                // reads fine for smooth aurora.
                float u = x / 7.0f;
                float v = (y + 1) / 10.0f + f * 0.13f;
                float it, hue;
                sample_aurora(u, v, s_phase, &it, &hue);
                it *= row_fade;
                if (it < 0.02f) continue;
                uint8_t r, g, b;
                hsv_to_rgb(hue, 0.95f, it * 0.8f, &r, &g, &b);
                render_set((cube_face_t)f, x, y, r, g, b);
            }
        }
    }
}

const effect_vtable_t g_effect_aurora = {
    .name = "aurora", .id = EFFECT_AURORA,
    .enter = aurora_enter, .step = aurora_step,
};
