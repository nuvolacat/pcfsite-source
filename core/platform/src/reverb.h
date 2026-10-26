// PCF2000 — reverberación estéreo para la música MIDI (dmusic.cpp): Freeverb (Jezar at Dreampoint, dominio público),
// 8 filtros peine + 4 paso-todo por canal, con las afinaciones clásicas para 44,1 kHz. Y un limitador suave.
#pragma once
#include <cmath>
#include <cstring>
#include <vector>

struct Freeverb {
    struct Comb { std::vector<float> buf; size_t i = 0; float store = 0; };
    struct Allpass { std::vector<float> buf; size_t i = 0; };
    Comb comb[2][8];
    Allpass ap[2][4];
    float feedback = 0.84f, damp = 0.4f, wet = 0.22f, width = 1.0f;
    Freeverb() {
        static const int ct[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, at[4] = {556, 441, 341, 225}, spread = 23;
        for (int ch = 0; ch < 2; ch++) {
            for (int k = 0; k < 8; k++) comb[ch][k].buf.assign((size_t)(ct[k] + ch * spread), 0.f);
            for (int k = 0; k < 4; k++) ap[ch][k].buf.assign((size_t)(at[k] + ch * spread), 0.f);
        }
    }
    // procesa n fotogramas estéreo entrelazados in situ
    void process(float *io, int n) {
        float wet1 = wet * (width / 2 + 0.5f), wet2 = wet * ((1 - width) / 2), dry = 1.0f;
        for (int s = 0; s < n; s++) {
            float in = (io[2 * s] + io[2 * s + 1]) * 0.015f, out[2] = {0, 0};
            for (int ch = 0; ch < 2; ch++) {
                for (Comb &c : comb[ch]) {
                    float y = c.buf[c.i];
                    c.store = y * (1 - damp) + c.store * damp;
                    c.buf[c.i] = in + c.store * feedback;
                    if (++c.i >= c.buf.size()) c.i = 0;
                    out[ch] += y;
                }
                for (Allpass &a : ap[ch]) {
                    float b = a.buf[a.i], y = -out[ch] + b;
                    a.buf[a.i] = out[ch] + b * 0.5f;
                    if (++a.i >= a.buf.size()) a.i = 0;
                    out[ch] = y;
                }
            }
            float l = io[2 * s] * dry + out[0] * wet1 + out[1] * wet2, r = io[2 * s + 1] * dry + out[1] * wet1 + out[0] * wet2;
            io[2 * s] = soft(l); io[2 * s + 1] = soft(r);
        }
    }
    static float soft(float v) { return v > 0.8f || v < -0.8f ? std::tanh(v) : v; }   // limitador suave
};
