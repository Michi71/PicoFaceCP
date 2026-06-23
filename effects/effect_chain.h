/*
 * Rhodes MK8 Effect Chain
 * 
 * This is an authentic implementation of the Rhodes MK8 electric piano's
 * preamp and effect chain based on the official manual.
 * 
 * Signal Flow:
 * 1. Preamp Section: Drive -> Envelope Filter (touch-wah)
 * 2. EQ Section: Low Shelf (75Hz) -> Mid Peak (100Hz-2kHz) -> High Shelf (3kHz)
 * 3. Vari-Pan: LFO-based stereo panning with 4 waveforms (0.1Hz-3kHz)
 * 4. Compressor: Dynamic range compression with makeup gain
 * 5. Stereo Chorus: Bucket-brigade style with independent L/R circuits
 * 6. 4-Stage Phaser: All-pass filter cascade with LFO modulation
 * 7. Stereo Delay: Bucket-brigade delay (60ms-800ms) with feedback
 * 8. Volume: Final output level control
 * 
 * All parameters are normalized to 0..1 range for easy integration.
 * The implementation uses standard C++ with no external dependencies.
 * 
 * Author: Based on Rhodes MK8 manual specifications
 * License: As per repository license
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "dsp_lut.h"
#include "dsp_fastmath.h"
#include <iostream>

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

// Simple biquad filter for EQ
struct BiquadFilter {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    
    BiquadFilter() : b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f),
                     x1(0.0f), x2(0.0f), y1(0.0f), y2(0.0f) {}
    
    void setLowShelf(float freq, float sampleRate, float gain);
    void setHighShelf(float freq, float sampleRate, float gain);
    void setPeaking(float freq, float sampleRate, float gain, float Q);
    float process(float input);
};

// All-pass filter for phaser
class AllPassFilter {
public:
    AllPassFilter() : x1(0.0f), y1(0.0f) {}

    float process(float x, float a);
    
    void reset() {
        x1 = 0.0f;
        y1 = 0.0f;
    }
    
private:
    float a1;
    float x1, y1;
};

// Simple delay line
class DelayLine {
public:
    DelayLine() : writeIndex(0) {}
    void init(int maxSamples);
    void write(float sample);
    float read(float delaySamples);
    void clear();
    
private:
    std::vector<float> buffer;
    int writeIndex;
};

// ============================================================================
// Core MK8 Wah
// ============================================================================

class Mk8WahCore {
public:
    Mk8WahCore() { reset(); }

    void setParams(float cutoff, float Q, float sr) {
        cutoff = std::clamp(cutoff, 50.0f, sr * 0.45f);
        Q = std::clamp(Q, 0.5f, 18.0f);

        // fastTan is accurate for small arguments (fc/sr << 1), ARMv8 friendly
        g = fastTan(float(M_PI) * cutoff / sr);
        k = 1.0f / Q;
        a1 = 1.0f / (1.0f + g * (g + k));
    }

    // Returns ONLY the filtered (wet) signal.
    // Dry/wet mixing is handled by the WahWah wrapper.
    float process(float x) {
        // SVF kernel (Chamberlin State-Variable Filter)
        float hp = (x - (k + g) * s1 - s2) * a1;
        float bp = g * hp + s1;
        float lp = g * bp + s2;

        // State update
        s1 = g * hp + bp;
        s2 = g * bp + lp;

        // MK8 character:
        //   LP (× 0.30) keeps the low-end warmth / body of the Rhodes tone
        //   BP (× 10.0) gives the aggressive "quack" resonance peak
        // The ratio LP:BP determines tonal balance vs. quack intensity.
        float out = lp * 0.30f + bp * 10.0f;

        // Soft saturation with gentle drive – models preamp nonlinearity.
        // tanh(0.55x)/0.55 keeps unity gain near 0 but gracefully clips peaks.
        return fastTanh(out * 0.8f) / 0.8f;
    }

    void reset() { s1 = s2 = 0.0f; }

private:
    float s1{0.0f}, s2{0.0f};
    float g{0.0f}, k{1.0f}, a1{0.0f};
};

// ============================================================================
// WahWah Wrapper mit internem Sample-Throttling
// ============================================================================

class WahWah
{
public:
    WahWah() { reset(); }

    void init(float sr)
    {
        sampleRate = (sr > 0.0f) ? sr : 48000.0f;
        lastFreqL  = lastFreqR = 300.0f;
        // Smooth-Koeffizienten für Freq & Q (verhindert Zipper-Noise)
        freqSmoothCoeff = 1.0f - std::exp(-1.0f / (sampleRate * 0.005f)); // 5ms
        wahCounter = 0;
    }

    void setMidFreqNorm(float n)    { midFreqNorm = std::clamp(n, 0.0f, 1.0f);
                                       _baseFreqCached = mapToLog(midFreqNorm, 200.0f, 450.0f); }
    void setMidGainNorm(float n)    { midGainNorm = std::clamp(n, 0.0f, 1.0f); }
    void setEnvelopeAmount(float n) { envAmount   = std::clamp(n, 0.0f, 1.0f); }

    void processStereo(float& inL, float& inR, float envL, float envR)
    {
        if (envAmount <= 0.01f) return;

        // ── Dry signal preserved for wet/dry blend ───────────────────────
        float dryL = inL;
        float dryR = inR;

        // ── MK8 Frequency Curve ──────────────────────────────────────────
        // Base frequency controlled by "Wah Base" knob (midFreqNorm).
        // Closed (env=0): ~200–450 Hz (dark, closed vowel)
        // Open  (env=1): up to ~3000 Hz (bright, open quack)
        //
        // sqrt curve: same shape as the original 0.55 power but uses
        // hardware-accelerated sqrtf on ARMv8 (no libm pow call needed).
        float baseFreq = _baseFreqCached;

        float curveL = std::sqrt(std::clamp(envL, 0.0f, 1.0f));
        float curveR = std::sqrt(std::clamp(envR, 0.0f, 1.0f));

        // Sweep ceiling: 3000 Hz is the "fully open" position on the MK8.
        float targetFreqL = baseFreq + (3000.0f - baseFreq) * curveL * envAmount;
        float targetFreqR = baseFreq + (3000.0f - baseFreq) * curveR * envAmount;

        targetFreqL = std::clamp(targetFreqL, 150.0f, 3200.0f);
        targetFreqR = std::clamp(targetFreqR, 150.0f, 3200.0f);

        // ── Smooth Frequency (prevents zipper noise) ─────────────────────
        freqSmoothL += freqSmoothCoeff * (targetFreqL - freqSmoothL);
        freqSmoothR += freqSmoothCoeff * (targetFreqR - freqSmoothR);

        lastFreqL = freqSmoothL;
        lastFreqR = freqSmoothR;

        // ── MK8 Dynamic Q (Resonance) ────────────────────────────────────
        // Rhodes MK8 wah is characteristically aggressive:
        //   Closed (env=0): Q ≈ 3.5  – warm, wide bump
        //   Open   (env=1): Q ≈ 14.0 – razor-sharp "quack" peak
        // midGainNorm ("Wah Depth") scales the resonance intensity.
        // The high Q at peak is what gives the MK8 its trademark bite.
        float QL = 3.5f + (10.5f * midGainNorm * curveL);  // 3.5 .. 14.0
        float QR = 3.5f + (10.5f * midGainNorm * curveR);

        // Update filter coefficients every 8 samples (CPU optimisation –
        // imperceptible at audio rates above ~6 kHz modulation bandwidth).
        if ((wahCounter++ & 7) == 0) {
            coreL.setParams(freqSmoothL, QL, sampleRate);
            coreR.setParams(freqSmoothR, QR, sampleRate);
        }

        // ── Pre-filter soft saturation (MK8 preamp character) ───────────
        // Very gentle drive – adds subtle harmonic richness without clipping
        // the input before it enters the resonant filter.
        float satL = fastTanh(inL * 1.05f) / 1.05f;
        float satR = fastTanh(inR * 1.05f) / 1.05f;

        // ── Filter ───────────────────────────────────────────────────────
        float wetL = coreL.process(satL);
        float wetR = coreR.process(satR);

        // ── Dry / Wet blend ──────────────────────────────────────────────
        // The blend is envelope-dependent:
        //   At peak (env=1, envAmount=1): ~88 % wet  → maximum quack, very aggressive
        //   At rest (env=0)             : ~12 % wet  → mostly dry, natural piano tone
        //
        // This means the piano always retains its core identity – the wah sweeps
        // *over* the sound rather than replacing it, exactly as on the hardware.
        float wetMix = 0.12f + 0.76f * curveL * envAmount;   // 0.12 .. 0.88
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

    float _baseFreqCached{300.0f};  // mapToLog(midFreqNorm, 200, 450), updated in setMidFreqNorm

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

// Post-drive lowpass (anti-fizz / aliasing tamer)
//
// Use AFTER diodeSaturation() and BEFORE time/mod effects.
// This is intentionally gentle; it just shaves off the harshest top end
// that distortion can generate.
//
// 1-pole LP: y[n] = y[n-1] + a * (x[n] - y[n-1])
// a = 1 - exp(-2*pi*fc/fs)
struct OnePoleLP {
    float a = 0.0f;
    float z = 0.0f;

    void setCutoff(float fc, float fs) {
        fc = std::clamp(fc, 20.0f, 0.49f * fs);
        a = 1.0f - std::exp(-2.0f * float(M_PI) * fc / fs);
    }

    float process(float x) {
        z += a * (x - z);
        return z;
    }

    void reset(float v = 0.0f) { z = v; }
};




/**
 * Rhodes MK8 Effect Chain
 * 
 * Complete stereo effect chain implementation for Rhodes MK8 electric piano.
 * Processes audio blocks with authentic preamp, EQ, and effects.
 * 
 * Usage:
 *   EffectChain fx;
 *   fx.init(48000.0f);  // Initialize with sample rate
 *   
 *   // Set parameters (all 0..1 range)
 *   fx.setVolume(0.75f);
 *   fx.setDrive(0.5f);
 *   fx.setChorusOn(1.0f);
 *   fx.setChorusRate(0.5f);
 *   fx.setChorusDepth(0.5f);
 *   
 *   // Process audio blocks
 *   fx.process(leftBuffer, rightBuffer, numSamples);
 */
class EffectChain
{
public:
    EffectChain();
    ~EffectChain();

    /** Initialising */
    void init(float sampleRate);

    /** Prozessiert ein Stereo‑Block */
    void process(float* outL, float* outR, int numFrames);

    /** Parameters */

    void resetValues();
    
    // Amp Section
    void setPreGain(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _preGainNorm = norm; } // -12..+12 dB (0.5 = 0 dB)
    void setVolume(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _volume = norm; }       // 0..1
    void setEnvelope(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _envelope = norm; }   // 0..1
    void setDrive(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _drive = norm; }         // 0..1
    
    // EQ Section
    void setBass(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _low = norm; }            // -15..+15 dB
    void setMiddle(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _mid_gain = norm; }     // -15..+15 dB
    void setMiddleFreq(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _mid_freq = norm; } // 100..2000 Hz (log)
    void setTreble(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _high = norm; }         // -15..+15 dB

    // VariPan Section
    void setVPOn(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _vpOn = norm; }           // 0,1 VariPan Off/On
    void setVPWave(float norm) { norm = std::clamp(norm, 0.0f, 3.999f); _vpWave = std::floor(norm); } // 0..3 'Sine':0;'Triangle':1;'Saw':2;'Square':3
    void setVPRate(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _vpRate = norm; }       // 0.1 Hz .. 3000 Hz (log)
    void setVPDepth(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _vpDepth = norm; }     // 0..100%

    // Compressor
    // Kompressor nach dem MK8-Prinzip: Studio-VCA mit Linked-Stereo,
    // dB-Domänen-Gain-Computer und gain-domänen Attack/Release.
    //   On/Off : _compressor_on
    //   Amount : Steuert Ratio (2:1..10:1) UND Release-Zeit (80..300 ms)
    //   Makeup : 0..+12 dB Ausgangspegel-Anhebung nach der Kompression
    void setCompressorOn(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _compressor_on = norm; } // 0,1 Compressor Off/On
    void setAmount(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _amount = norm; }       // 0..1 Amount (Ratio + Release)
    void setMakeup(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _makeup = norm; }       // 0..1 Makeup (0..+12 dB)

    // Chorus
    void setChorusOn(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _chorus_on = norm; }          // 0,1 Chorus Off/On      
    void setChorusRate(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _chorus_rate = norm; }      // 0..1 Rate
    void setChorusDepth(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _chorus_depth = norm; }    // 0..1 Depth

    // Phaser
    void setPhaserOn(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _phaser_on = norm; }          // 0,1 Phaser Off/On      
    void setPhaserRate(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _phaser_rate = norm; }      // 0..1 Rate
    void setPhaserDepth(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _phaser_depth = norm; }    // 0..1 Depth

    // Delay
    void setDelayOn(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _delay_on = norm; }       // 0,1 Delay Off/On      
    void setDelayTime(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _delay_time = norm; }   // 0..1 Time
    void setDelayFeedback(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _delay_feedback = norm; }   // 0..1 Feedback
    void setDelayMix(float norm) { norm = std::clamp(norm, 0.0f, 1.0f); _delay_mix = norm; }     // 0..1 Mix

private:
    uint32_t fSamplingFreq;
    
    // sliders (parameters)
    float _preGainNorm;
    float _volume;
    float _envelope;
    float _drive;

    float _low;
    float _mid_gain;
    float _mid_freq;
    float _high;

    float _vpOn;
    float _vpWave;
    float _vpRate;
    float _vpDepth;

    float _compressor_on;
    float _amount;
    float _makeup;

    float _chorus_on;
    float _chorus_rate;
    float _chorus_depth;

    float _phaser_on;
    float _phaser_rate;
    float _phaser_depth;

    float _delay_on;
    float _delay_time;
    float _delay_feedback;
    float _delay_mix;

    // Internal DSP state
    float sampleRate;
    
    // Preamp - envelope follower for touch-wah
    float _preGainSmooth;       // Smoothed linear pre-gain (one-pole)
    float _preGainSmoothCoeff;  // One-pole smoothing coefficient (~10 ms)
    WahWah wah;
    BiquadFilter envelopeFilter[2];
    float envelopeFollowerState[2];
    float _inputRmsSmooth = 0.0f; // Für die Auto-Sensitivität
    
    // EQ filters
    BiquadFilter lowFilter[2];
    BiquadFilter midFilter[2];
    BiquadFilter highFilter[2];

    // Cached EQ parameters (avoid recompute every block)
    float _cachedLow = -1.0f;
    float _cachedHigh = -1.0f;
    float _cachedMidFreq = -1.0f;
    float _cachedMidGain = -1.0f;
    bool  _midFilterDirty = true;

    // Envelope follower frequency smoothing (gegen Zipper-Noise)
    float envelopeFreqSmoothL;
    float envelopeFreqSmoothR;
    
    // Envelope filter throttling state
    float lastEnvFreqL;
    float lastEnvFreqR;
    int envUpdateCounter;

    // Precomputed one-pole coefficients (from init, depend only on sampleRate)
    float _levelCoeff;         // 30 ms RMS smoothing for auto-sensitivity
    float _envAttackCoeff;     // 8 ms envelope follower attack
    float _envReleaseCoeff;    // 200 ms envelope follower release

    // Drive-precomputed values (updated before sample loop in process())
    float _drivePreGain{1.0f}; // linear pregain from _drive
    float _driveComp{1.0f};    // linear compensation from _drive
    
    // Vari-Pan LFO
    float vpPhase;
    float vpPanLSmooth;  // ← NEW: Smoothed pan values
    float vpPanRSmooth;  // ← NEW
    
    // Compressor state
    float compressorEnvelope[2];
    
    // Chorus
    DelayLine chorusDelayL;
    DelayLine chorusDelayR;
    float chorusPhase;
    
    // Phaser
    AllPassFilter phaserFilters[2][4];
    float phaserPhase;
    int   _phaserAlphaCounter = 0;
    float _phaserAlphaCur = 0.0f;
    float _phaserAlphaTarget = 0.0f;
    float _phaserLastL = 0.0f;
    float _phaserLastR = 0.0f;
    
    // Delay
    DelayLine delayBufferL;
    DelayLine delayBufferR;

    // Anti Fizz Filter
    OnePoleLP _postDriveLP_L;
    OnePoleLP _postDriveLP_R;
    
    SinLUT<2048> _sinLut;

    // Helper functions
    float diodeSaturation(float input, float drive);
    float softClip(float input);
    float getLFOValue(float phase, int waveform);
    float mapToLog(float norm, float min, float max);
};