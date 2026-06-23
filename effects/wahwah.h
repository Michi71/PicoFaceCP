// wahwah.h
//
// Standalone, header-only MK8 touch-wah (Mk8WahCore + WahWah), extracted
// verbatim from the MK8 reference effect_chain.h so the Reface CP chain does
// not have to pull in effect_chain.h (which drags <iostream>/<vector> and the
// whole MK8 EffectChain declaration into the firmware).
//
// Extracted verbatim via glm-5.2:cloud; formally reviewed by the orchestrator.

#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "dsp_fastmath.h"
#include "cp_hot.h"

// Provide clamp for C++ < C++17 if not available
#if !defined(__cpp_lib_clamp) && __cplusplus < 201703L
namespace std {
    template <class T>
    inline constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Mk8WahCore {
public:
    Mk8WahCore() { reset(); }

    void setParams(float cutoff, float Q, float sr) {
        cutoff = std::clamp(cutoff, 50.0f, sr * 0.45f);
        Q = std::clamp(Q, 0.5f, 18.0f);
        g = fastTan(float(M_PI) * cutoff / sr);
        k = 1.0f / Q;
        a1 = 1.0f / (1.0f + g * (g + k));
    }

    float process(float x) {
        float hp = (x - (k + g) * s1 - s2) * a1;
        float bp = g * hp + s1;
        float lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;
        float out = lp * 0.30f + bp * 10.0f;
        return fastTanh(out * 0.8f) / 0.8f;
    }

    void reset() { s1 = s2 = 0.0f; }

private:
    float s1{0.0f}, s2{0.0f};
    float g{0.0f}, k{1.0f}, a1{0.0f};
};

class WahWah
{
public:
    WahWah() { reset(); }

    void init(float sr)
    {
        sampleRate = (sr > 0.0f) ? sr : 48000.0f;
        lastFreqL  = lastFreqR = 300.0f;
        freqSmoothCoeff = 1.0f - std::exp(-1.0f / (sampleRate * 0.005f));
        wahCounter = 0;
    }

    void setMidFreqNorm(float n)    { midFreqNorm = std::clamp(n, 0.0f, 1.0f);
                                       _baseFreqCached = mapToLog(midFreqNorm, 200.0f, 450.0f); }
    void setMidGainNorm(float n)    { midGainNorm = std::clamp(n, 0.0f, 1.0f); }
    void setEnvelopeAmount(float n) { envAmount   = std::clamp(n, 0.0f, 1.0f); }

    void CP_HOT(processStereo)(float& inL, float& inR, float envL, float envR)
    {
        if (envAmount <= 0.01f) return;
        float dryL = inL;
        float dryR = inR;
        float baseFreq = _baseFreqCached;
        float curveL = std::sqrt(std::clamp(envL, 0.0f, 1.0f));
        float curveR = std::sqrt(std::clamp(envR, 0.0f, 1.0f));
        float targetFreqL = baseFreq + (3000.0f - baseFreq) * curveL * envAmount;
        float targetFreqR = baseFreq + (3000.0f - baseFreq) * curveR * envAmount;
        targetFreqL = std::clamp(targetFreqL, 150.0f, 3200.0f);
        targetFreqR = std::clamp(targetFreqR, 150.0f, 3200.0f);
        freqSmoothL += freqSmoothCoeff * (targetFreqL - freqSmoothL);
        freqSmoothR += freqSmoothCoeff * (targetFreqR - freqSmoothR);
        lastFreqL = freqSmoothL;
        lastFreqR = freqSmoothR;
        float QL = 3.5f + (10.5f * midGainNorm * curveL);
        float QR = 3.5f + (10.5f * midGainNorm * curveR);
        if ((wahCounter++ & 7) == 0) {
            coreL.setParams(freqSmoothL, QL, sampleRate);
            coreR.setParams(freqSmoothR, QR, sampleRate);
        }
        float satL = fastTanh(inL * 1.05f) / 1.05f;
        float satR = fastTanh(inR * 1.05f) / 1.05f;
        float wetL = coreL.process(satL);
        float wetR = coreR.process(satR);
        float wetMix = 0.12f + 0.76f * curveL * envAmount;
        float wetMixR = 0.12f + 0.76f * curveR * envAmount;
        inL = dryL * (1.0f - wetMix)  + wetL * wetMix;
        inR = dryR * (1.0f - wetMixR) + wetR * wetMixR;
    }

    void reset()
    {
        coreL.reset();
        coreR.reset();
        lastFreqL = lastFreqR = 300.0f;
        freqSmoothL = freqSmoothR = 300.0f;
        lastQL = lastQR = 1.5f;
        wahCounter = 0;
        freqSmoothCoeff = 0.0f;
    }

public:
    float lastFreqL{300.0f};
    float lastFreqR{300.0f};

private:
    float sampleRate{48000.0f};
    Mk8WahCore coreL;
    Mk8WahCore coreR;
    float midFreqNorm{0.5f};
    float midGainNorm{0.5f};
    float envAmount{1.0f};
    float _baseFreqCached{300.0f};
    float freqSmoothL{300.0f};
    float freqSmoothR{300.0f};
    float freqSmoothCoeff{0.0f};
    float lastQL{1.5f};
    float lastQR{1.5f};
    int   wahCounter{0};
    static float mapToLog(float n, float min, float max)
    {
        return min * std::exp(std::log(max / min) * n);
    }
};
