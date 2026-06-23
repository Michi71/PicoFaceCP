/*
 * Rhodes MK8 Effect Chain - Implementation
 * 
 * This file contains the DSP implementations for all effects in the chain.
 * Each effect is designed to closely match the analog behavior described
 * in the Rhodes MK8 manual.
 * 
 * Key DSP techniques used:
 * - Biquad filters for EQ (Robert Bristow-Johnson cookbook formulas)
 * - All-pass filters for phasing
 * - Fractional delay lines for chorus/delay effects
 * - Envelope followers for dynamics processing
 * - LFO generators with multiple waveforms
 * - Soft saturation for analog warmth
 */

#include "effect_chain.h"
#include <cstring>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── ARMv8 math helpers ───────────────────────────────────────────────────────
// Use expf/logf instead of pow(10,x) everywhere; these map to single
// hardware instructions on Cortex-A53/A72 (vsqrt, fmul, etc.).
static constexpr float kLn10      = 2.302585093f;
static constexpr float kDB20      = kLn10 / 20.0f;   // multiply dB→linear (exp path)
static constexpr float k20OverLn10 = 20.0f / kLn10;  // multiply ln(x)→dB

// Inline replacement for pow(10, dB/20)
static inline float db2lin(float dB)  { return std::exp(dB * kDB20); }
// Inline replacement for pow(10, dB/40)
static inline float db2lin40(float dB){ return std::exp(dB * (kLn10 / 40.0f)); }
// Inline replacement for 20*log10(x)
static inline float lin2dB(float x)   { return k20OverLn10 * std::log(x); }

// ============================================================================
// BiquadFilter implementation
// ============================================================================

void BiquadFilter::setLowShelf(float freq, float sampleRate, float gain) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float A = db2lin40(gain);
    float S = 1.0f;
    float alpha = std::sin(w0) / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float cosw0 = std::cos(w0);
    
    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha;
    b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha)) / a0;
    b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
    b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha)) / a0;
    a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
    a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha) / a0;
}

void BiquadFilter::setHighShelf(float freq, float sampleRate, float gain) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float A = db2lin40(gain);
    float S = 1.0f;
    float alpha = std::sin(w0) / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    float cosw0 = std::cos(w0);
    
    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha;
    b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha)) / a0;
    b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
    b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha)) / a0;
    a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
    a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha) / a0;
}

void BiquadFilter::setPeaking(float freq, float sampleRate, float gain, float Q)
{
    // RBJ Audio EQ Cookbook - Peaking EQ
    float A  = db2lin40(gain);  // gain in dB
    float w0 = 2.0f * float(M_PI) * freq / sampleRate;
    float cosw0 = std::cos(w0);
    float sinw0 = std::sin(w0);
    float alpha = sinw0 / (2.0f * Q);

    float b0_raw =  1.0f + alpha * A;
    float b1_raw = -2.0f * cosw0;
    float b2_raw =  1.0f - alpha * A;
    float a0_raw =  1.0f + alpha / A;
    float a1_raw = -2.0f * cosw0;
    float a2_raw =  1.0f - alpha / A;

    // Normalisieren auf a0=1
    b0 = b0_raw / a0_raw;
    b1 = b1_raw / a0_raw;
    b2 = b2_raw / a0_raw;
    a1 = a1_raw / a0_raw;
    a2 = a2_raw / a0_raw;
}

float BiquadFilter::process(float input) {
    float output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    
    return output;
}

// ============================================================================
// AllPassFilter implementation
// ============================================================================
float AllPassFilter::process(float x, float a) {
    // Kanonische Form: y[n] = a * x[n] + x[n-1] - a * y[n-1]
    float y = a * x + x1 - a * y1;
    x1 = x;
    y1 = y;
    // Denormal protection
    if (std::abs(y1) < 1e-15f) y1 = 0.0f; 
    return y;
}

// ============================================================================
// DelayLine implementation
// ============================================================================

void DelayLine::init(int maxSamples) {
    buffer.resize(maxSamples, 0.0f);
    writeIndex = 0;
}

void DelayLine::write(float sample) {
    buffer[writeIndex] = sample;
    writeIndex = (writeIndex + 1) % buffer.size();
}

float DelayLine::read(float delaySamples) {
    if (buffer.empty()) return 0.0f;
    
    // Clamp delay to valid range
    delaySamples = std::clamp(delaySamples, 0.0f, (float)(buffer.size() - 1));
    
    // Calculate read position
    float readPos = writeIndex - delaySamples;
    while (readPos < 0.0f) readPos += buffer.size();
    while (readPos >= buffer.size()) readPos -= buffer.size();
    
    // Get integer indices for interpolation
    int index0 = (int)readPos;
    int index1 = (index0 + 1) % buffer.size();
    float frac = readPos - index0;
    
    // Ensure indices are within bounds
    index0 = index0 % buffer.size();
    
    return buffer[index0] * (1.0f - frac) + buffer[index1] * frac;
}

void DelayLine::clear() {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
}

// ============================================================================
// EffectChain implementation
// ============================================================================

EffectChain::EffectChain()
{
    resetValues();
    
    sampleRate = 48000.0f;
    
    _preGainSmooth = 1.0f;
    _preGainSmoothCoeff = 1.0f - std::exp(-1.0f / (48000.0f * 0.010f));

    vpPhase = 0.0f;
    chorusPhase = 0.0f;
    phaserPhase = 0.0f;
    
    // Initialize smoothing for VariPan
    vpPanLSmooth = 1.0f;
    vpPanRSmooth = 1.0f;
    
    // Initialize envelope filter throttling
    lastEnvFreqL = 0.0f;
    lastEnvFreqR = 0.0f;
    envUpdateCounter = 0;
    
    for (int i = 0; i < 2; i++) {
        envelopeFollowerState[i] = 0.0f;
        compressorEnvelope[i] = 0.0f;
    }
}

// Destructor
EffectChain::~EffectChain()
{
}

void EffectChain::resetValues()
{
    // Initialize parameters to neutral/off positions
    _preGainNorm = 0.5f;
    _volume = 0.75f;
    _envelope = 0.0f;
    _drive = 0.0f;
    
    _low = 0.5f;
    _mid_gain = 0.5f;
    _mid_freq = 0.5f;
    _high = 0.5f;
    
    _vpOn = 0.0f;
    _vpWave = 0.0f;
    _vpRate = 0.4f;
    _vpDepth = 0.0f;
    
    _compressor_on = 0.0f;
    _amount = 0.0f;
    _makeup = 0.0f;
    
    _chorus_on = 0.0f;
    _chorus_rate = 0.5f;
    _chorus_depth = 0.0f;
    
    _phaser_on = 0.0f;
    _phaser_rate = 0.5f;
    _phaser_depth = 0.0f;
    
    _delay_on = 0.0f;
    _delay_time = 0.5f;
    _delay_feedback = 0.0f;
    _delay_mix = 0.5f;
}

void EffectChain::init(float sr)
{
    if (sr < 1.0f) sr = 48000.0f;
    sampleRate = sr;

    _sinLut.init();
    
    _preGainSmoothCoeff = 1.0f - std::exp(-1.0f / (sampleRate * 0.010f));

    // Precompute time-constant coefficients (depend only on sampleRate).
    // These replace exp() calls that were previously inside the sample loop.
    _levelCoeff      = 1.0f - std::exp(-1.0f / (sampleRate * 0.030f)); // 30 ms RMS
    _envAttackCoeff  = 1.0f - std::exp(-1.0f / (sampleRate * 0.008f)); // 8 ms attack
    _envReleaseCoeff = 1.0f - std::exp(-1.0f / (sampleRate * 0.200f)); // 200 ms release
    
    // Initialize delay lines
    chorusDelayL.init((int)(sampleRate * 0.05f));  // 50ms max for chorus
    chorusDelayR.init((int)(sampleRate * 0.05f));
    
    delayBufferL.init((int)(sampleRate * 0.85f));  // 850ms for delay (slightly more than 800ms)
    delayBufferR.init((int)(sampleRate * 0.85f));
    
    // Reset filter states
    for (int i = 0; i < 2; i++) {
        envelopeFollowerState[i] = 0.0f;
        compressorEnvelope[i] = 0.0f;
    }

    envelopeFreqSmoothL = 200.0f;  // Initial frequency
    envelopeFreqSmoothR = 200.0f;
    
    // Reset envelope filter throttling
    lastEnvFreqL = 0.0f;
    lastEnvFreqR = 0.0f;
    envUpdateCounter = 0;

    wah.init(sampleRate);

    // Reset Phaser-Filter
    for (int ch = 0; ch < 2; ch++) {
        for (int stage = 0; stage < 4; stage++) {
            phaserFilters[ch][stage].reset();
        }
    }

    _postDriveLP_L.setCutoff(10000.0f, sampleRate); // try 10k..14k
    _postDriveLP_R.setCutoff(10000.0f, sampleRate);
    
    vpPhase = 0.0f;
    chorusPhase = 0.0f;
    phaserPhase = 0.0f;

    _phaserAlphaCounter = 0;
    _phaserAlphaCur = 0.0f;
    _phaserAlphaTarget = 0.0f;
    _phaserLastL = 0.0f;
    _phaserLastR = 0.0f;
}

// Helper:  Diode saturation
// pregain and comp are precomputed per block (see process()) from _drive.
float EffectChain::diodeSaturation(float input, float drive)
{
    drive = std::clamp(drive, 0.0f, 1.0f);
    if (drive < 0.001f) return input;

    float x = input * _drivePreGain;

    // extra-curve for asymmetry
    float t = std::clamp((drive - 0.7f) / 0.3f, 0.0f, 1.0f);
    float extra = t * t * t;

    float hard = 1.0f + 3.0f * extra; // 1..3
    float posDrive = 0.8f * hard;
    float negDrive = 1.2f * hard;

    float y = (x >= 0.0f)
        ? (fastTanh(posDrive * x) / posDrive)
        : (fastTanh(negDrive * x) / negDrive);

    return y * _driveComp;
}

// Helper:  Soft clipping
float EffectChain::softClip(float input) {
    if (input > 1.0f) return 1.0f;
    if (input < -1.0f) return -1.0f;
    return input;
}

/*inline float softLimiter(float x)
{
    constexpr float ceiling = 0.98f;      // etwas mehr Headroom als 1.0
    float y = x / ceiling;
    y = std::tanh(0.9f * y) / std::tanh(0.9f);
    return y * ceiling;
}*/

inline float softLimiter(float x)
{
    constexpr float ceiling = 0.98f;
    constexpr float a = 0.9f;
    // Precompute tanh(a) approx once (constant)
    static const float norm = fastTanh(a);

    float y = x / ceiling;
    y = fastTanh(a * y) / norm;
    return y * ceiling;
}

static inline float volumeTrimGainFromNorm(float n)
{
    n = std::clamp(n, 0.0f, 1.0f);
    // optional taper
    float t = n * n;
    // -30..+6 dB
    float dB = -30.0f + 36.0f * t;
    return db2lin(dB);
}

static inline float volumeGainFromNorm(float n)
{
    n = std::clamp(n, 0.0f, 1.0f);
    float t = n * n;                 // audio taper
    float dB = -60.0f + 60.0f * t;   // -60..0
    return db2lin(dB);
}

// Helper: LFO waveform generator
float EffectChain::getLFOValue(float phase, int waveform) {
    float value = 0.0f;
    
    switch (waveform) {
        case 0: // Sine
            //value = std::sin(phase * 2.0f * M_PI);
            value = _sinLut.sin01_nofloor(phase);
            break;
        case 1: // Triangle
            value = 2.0f * std::abs(2.0f * (phase - std::floor(phase + 0.5f))) - 1.0f;
            break;
        case 2: // Ramp (upwards)
            value = 2.0f * (phase - std::floor(phase)) - 1.0f;
            break;
        case 3:  { // Square - HARD für Pan-Effekt
            float t = phase - std::floor(phase);
            value = (t < 0.5f) ? 1.0f : -1.0f;
            // KEIN Smoothing für Pan!
            break;
        }
        default:
            //value = std::sin(phase * 2.0f * M_PI);
            value = _sinLut.sin01_nofloor(phase);
    }
    
    return value;
}

// Helper: Map 0..1 to logarithmic range
// min * exp(log(max/min) * n)  — avoids std::pow, uses expf which is a
// single instruction on ARMv8 FPU.
float EffectChain::mapToLog(float norm, float min, float max) {
    return min * std::exp(std::log(max / min) * norm);
}

void EffectChain::process(float* outL, float* outR, int numFrames)
{    
    //int debugCounter=0;
    // ======================================================================
    // PRE-COMPUTE PRE-GAIN TARGET (once per block)
    // ======================================================================

    float preGainDb = (_preGainNorm - 0.5f) * 24.0f;  // 0..1 -> -12..+12 dB
    float preGainTarget = db2lin(preGainDb);

    // ======================================================================
    // PRE-COMPUTE EQ PARAMETERS (LOW + HIGH) — only when changed
    // ======================================================================

    if (_low != _cachedLow) {
        _cachedLow = _low;
        float lowGain = (_low - 0.5f) * 30.0f;  // -15 dB to +15 dB
        for (int ch = 0; ch < 2; ch++) {
            lowFilter[ch].setLowShelf(75.0f, sampleRate, lowGain);
        }
    }

    if (_high != _cachedHigh) {
        _cachedHigh = _high;
        float highGain = (_high - 0.5f) * 30.0f;
        for (int ch = 0; ch < 2; ch++) {
            highFilter[ch].setHighShelf(3000.0f, sampleRate, highGain);
        }
    }

    // ======================================================================
    // PRE-COMPUTE MID EQ (NUR wenn Envelope INAKTIV!) — only when changed
    // ======================================================================

    bool useEnvelope = (_envelope > 0.01f);
    float midFreq = 0.0f;
    float midGain = 0.0f;
    float midQ = 1.0f;

    if (!useEnvelope) {
        bool midChanged = (_mid_freq != _cachedMidFreq) || (_mid_gain != _cachedMidGain);
        if (midChanged || _midFilterDirty) {
            _cachedMidFreq = _mid_freq;
            _cachedMidGain = _mid_gain;
            _midFilterDirty = false;

            midFreq = mapToLog(_mid_freq, 100.0f, 2000.0f);
            midGain = (_mid_gain - 0.5f) * 30.0f;  // -15 dB to +15 dB
            midQ = 1.0f;

            for (int ch = 0; ch < 2; ch++) {
                midFilter[ch].setPeaking(midFreq, sampleRate, midGain, midQ);
            }
        }
    } else {
        // Envelope active: mark mid filter dirty so it gets recomputed
        // when envelope is turned off again
        _midFilterDirty = true;
    }

    // ======================================================================
    // PRE-COMPUTE COMPRESSOR COEFFICIENTS (einmal pro Block – Parameter
    // ändern sich nicht innerhalb eines Blocks)
    // ======================================================================
    //
    // Zeitkonstanten: exp-basiert für korrektes IIR-Verhalten.
    //   Attack  10 ms   – fest, greift schnell genug für Note-Transients
    //   Release 80..300 ms – skaliert mit _amount: mehr Kompression = längere Release
    //     → musikalisch sinnvoll: beim Eindrehen eines stärkeren Kompressors
    //       soll die Gain-Reduction länger halten (weniger Pumpen)
    //
    // Ratio: 2:1 (Amount=0) .. 10:1 (Amount=1)
    // Makeup: 0..+12 dB linear aus Norm-Wert _makeup
    // ======================================================================

    float compAttackCoeff  = 1.0f - std::exp(-1.0f / (sampleRate * 0.010f));
    float compReleaseCoeff = 1.0f - std::exp(-1.0f / (sampleRate * (0.080f + _amount * 0.220f)));
    float compRatio        = 2.0f + _amount * 8.0f;                  // 2:1 .. 10:1
    float compMakeupGain   = db2lin(_makeup * 12.0f);                 // 0..+12 dB

    // ── Precompute drive-dependent values (no pow in sample loop) ──────────
    // These depend only on _drive which is constant within a block.
    {
        float d     = _drive * _drive;
        float t     = std::clamp((_drive - 0.7f) / 0.3f, 0.0f, 1.0f);
        float extra = t * t * t;
        float pregainDb  = 15.0f * d + 18.0f * extra;
        float compAmount = 0.80f - 0.25f * extra;
        float compDb     = pregainDb * compAmount;
        _drivePreGain = db2lin(pregainDb);
        _driveComp    = db2lin(-compDb);
    }

    // ── Precompute per-block constants that map to log-scale parameters ────
    // None of these change within a block; moving them here avoids one
    // mapToLog (= expf + logf) call per sample per active effect.
    const float vpFreq     = (_vpOn > 0.5f && _vpDepth > 0.01f)
                             ? mapToLog(_vpRate, 0.1f, 3000.0f) : 0.0f;
    const float vpInc      = vpFreq / sampleRate;

    const float chorusFreq = (_chorus_on > 0.5f && _chorus_depth > 0.01f)
                             ? mapToLog(_chorus_rate, 0.1f, 5.0f) : 0.0f;
    const float chorusInc  = chorusFreq / sampleRate;

    const float delaySamp  = (_delay_on > 0.5f && _delay_mix > 0.01f)
                             ? mapToLog(_delay_time, 60.0f, 800.0f) * sampleRate / 1000.0f
                             : 0.0f;

    const float phaserFreq = (_phaser_on > 0.5f && _phaser_depth > 0.01f)
                             ? mapToLog(_phaser_rate, 0.1f, 10.0f) : 0.0f;

    // ── Volume + fixed output trim (constant, no pow in loop) ─────────────
    const float vol         = volumeTrimGainFromNorm(_volume);
    static constexpr float outputTrimDb = +7.0f;
    static const float outputTrim = db2lin(outputTrimDb);   // ≈ 2.2387

    // ── Precompute compressor threshold constant (per block) ───────────────
    constexpr float thresholdDb = -18.0f;
    // k = slope = 1 - 1/ratio (constant once compRatio is known)
    const float compK = 1.0f - 1.0f / compRatio;
    // C = exp(thresholdDb * k * kDB20)  — constant factor, precomputed
    const float compThreshLin  = db2lin(thresholdDb);       // ≈ 0.1259
    (void)compThreshLin; // used via log path below

    // ======================================================================
    // MAIN PROCESSING LOOP
    // ======================================================================
    
    for (int i = 0; i < numFrames; i++) {
        float inL = outL[i];
        float inR = outR[i];

        // ══════════════════════════════════════════════════════════════
        // 0. PRE-GAIN (Input Trim, -12..+12 dB mit One-Pole Smoothing)
        // Verhindert Zipper-Noise bei Parameteränderungen (10 ms Glättung).
        // ══════════════════════════════════════════════════════════════

        _preGainSmooth += _preGainSmoothCoeff * (preGainTarget - _preGainSmooth);
        inL *= _preGainSmooth;
        inR *= _preGainSmooth;

        //static int wahCounter = 0;
        //wahCounter++;
        //bool updateWah = (wahCounter & 64) == 0;
        
        // ══════════════════════════════════════════════════════════════
        // 1. PREAMP / DRIVE (Dioden-Sättigung)
        // Asymmetrische Soft-Saturation modelliert den Röhrenpreamp des MK8.
        //   Drive 0..0.7: Warmth-Zone (+0..15 dB Pregain, tanh-Clipping)
        //   Drive 0.7..1: Distortion-Zone (+bis 33 dB, härterer Clip)
        // Post-Drive Low-Pass (10 kHz) entfernt High-Frequency-Aliasing
        // ("Anti-Fizz-Filter").
        // ══════════════════════════════════════════════════════════════
        if (_drive > 0.01f) {
            inL = diodeSaturation(inL, _drive);
            inR = diodeSaturation(inR, _drive);

            // post-drive anti-fizz
            inL = _postDriveLP_L.process(inL);
            inR = _postDriveLP_R.process(inR);
        }

        // ==================================================================
        // 2. ENVELOPE FOLLOWER / WAH-WAH (Touch-Wah)
        // Wenn _envelope > 0: Mid-EQ wird durch den dynamischen Wah ersetzt.
        // Der Envelope-Follower detektiert die Spielstärke (Attack 8 ms /
        // Release 200 ms) und steuert damit den Mk8WahCore (SVF-Filter).
        //   Attack 8 ms  → jede Note bekommt einen schnellen "Kick"
        //   Release 200 ms → "Quack"-Vokal klingt natürlich aus
        // Auto-Sensitivität: 30 ms RMS-Mittelwert normalisiert den Eingangspegel,
        // sodass der Wah bei allen Spiellautstärken gleich reagiert.
        // ==================================================================

        /*if (useEnvelope) {
            // TEST: Wir ignorieren den echten Envelope und setzen ihn auf 0.8 (fast offen)
            float testEnv = 0.8f; 
            
            wah.setMidFreqNorm(0.5f);    // Mitte
            wah.setMidGainNorm(1.0f);    // Max Resonanz
            wah.setEnvelopeAmount(1.0f); // Max Depth
            
            // Wir geben 0.8 fest rein
            wah.processStereo(inL, inR, testEnv, testEnv);
            
            // DEBUG LOG (nur alle 1 Sekunde)
            static int testC = 0;
            if (++testC % 48000 == 0) {
                std::cout << "WAH TEST ACTIVE - FreqL: " << wah.lastFreqL << std::endl;
            }
        }*/

        if (useEnvelope) {

            // 1) Sidechain Level (post-drive)
            float absL = std::abs(inL);
            float absR = std::abs(inR);
            float absM = 0.5f * (absL + absR);

            // 2) Slow RMS for auto-sensitivity (30 ms – tracks playing level)
            // _levelCoeff is precomputed in init()
            _inputRmsSmooth += _levelCoeff * (absM - _inputRmsSmooth);

            // 3) Normalise into a musically useful range.
            //    target ≈ 0.10: moderate hits open the wah noticeably.
            float target = 0.10f;
            float norm = target / (_inputRmsSmooth + 1e-6f);
            norm = std::clamp(norm, 0.4f, 10.0f);

            float envInL = std::clamp(absL * norm, 0.0f, 1.0f);
            float envInR = std::clamp(absR * norm, 0.0f, 1.0f);

            // 4) Attack / Release – MK8 is snappy:
            //    Attack  8 ms  → each note gets a fast "kick"
            //    Release 200 ms → vowel lingers before closing (natural)
            // _envAttackCoeff / _envReleaseCoeff precomputed in init()

            for (int ch = 0; ch < 2; ch++) {
                float inp  = (ch == 0) ? envInL : envInR;
                float& env = envelopeFollowerState[ch];

                float diff = inp - env;
                env += (diff > 0.0f ? _envAttackCoeff : _envReleaseCoeff) * diff;
                env = std::clamp(env, 0.0f, 1.0f);
            }

            // 5) Drive envelope into Wah:
            //    sqrt curve gives more sweep movement at gentle playing levels,
            //    matching the MK8 "responsive at any velocity" feel.
            float driveEnvL = std::sqrt(envelopeFollowerState[0]);
            float driveEnvR = std::sqrt(envelopeFollowerState[1]);

            wah.setMidFreqNorm(_mid_freq);
            wah.setMidGainNorm(_mid_gain);
            wah.setEnvelopeAmount(_envelope);
            wah.processStereo(inL, inR, driveEnvL, driveEnvR);
        } else {
            // Statischer Mid-Filter (unverändert)
            inL = midFilter[0].process(inL);
            inR = midFilter[1].process(inR);
        }
        
        // ==================================================================
        // 3. EQ SECTION (Low-Shelf + High-Shelf)
        // Low-Shelf  75 Hz  : Bass-Regler  −15..+15 dB (RBJ-Formel, S=1)
        // High-Shelf 3 kHz  : Treble-Regler −15..+15 dB
        // Mid-Peaking 100..2000 Hz wird ENTWEDER statisch ODER durch den
        // Envelope-Follower (Wah) gesteuert – nicht gleichzeitig.
        // ==================================================================
        
        inL = lowFilter[0].process(inL);
        inR = lowFilter[1].process(inR);
        
        inL = highFilter[0].process(inL);
        inR = highFilter[1].process(inR);
        
        // ==================================================================
        // 4. VARI-PAN
        // LFO-gesteuertes Stereo-Panning mit 4 Wellenformen (Sine, Triangle,
        // Saw, Square). Rate: 0.1 Hz .. 3000 Hz (logarithmisch).
        //
        //   panL = 0.5 + lfo * depth * 0.5   → L-Kanal wird lauter wenn LFO > 0
        //   panR = 0.5 - lfo * depth * 0.5   → R-Kanal gegenphasig
        //   → Constant-Power-Panning: sqrt(pan) hält Gesamtlautstärke konstant
        //
        // Smoothing: Unter 20 Hz sanftes Glättungsfilter (verhindert Zipper);
        //            über 20 Hz kein Smoothing (für FM/Vibrato-Effekte).
        // ==================================================================
        
        if (_vpOn > 0.5f && _vpDepth > 0.01f) {
            // vpFreq and vpInc are precomputed before the loop
            float lfoValue = getLFOValue(vpPhase, (int)_vpWave);
            
            // Stereo panning berechnen
            float panL = 0.5f + (lfoValue * _vpDepth * 0.5f);
            float panR = 0.5f - (lfoValue * _vpDepth * 0.5f);
            
            panL = std::clamp(panL, 0.0f, 1.0f);
            panR = std::clamp(panR, 0.0f, 1.0f);
            
            // Smoothing
            float smoothingFactor = (vpFreq > 20.0f) ? 1.0f : 0.05f; // Kein Smoothing bei FM-Raten
            vpPanLSmooth += (panL - vpPanLSmooth) * smoothingFactor;
            vpPanRSmooth += (panR - vpPanRSmooth) * smoothingFactor; // BUGFIX: war panL → kein Stereoeffekt
            
            // Constant-power panning
            inL *= std::sqrt(vpPanLSmooth);
            inR *= std::sqrt(vpPanRSmooth);
            
            vpPhase += vpInc;
            if (vpPhase >= 1.0f) vpPhase -= 1.0f;
        }
        
        // ==================================================================
        // 5. COMPRESSOR
        // Rhodes MK8: Studio-VCA-Kompressor für Dynamik-Kontrolle und Punch.
        //
        //   Threshold : -18 dBFS (≈ 0.126 linear) – greift bei fast allen
        //               Piano-Noten → gleichmäßiger Dynamik-Glue
        //   Ratio     : 2:1 .. 10:1 (Amount-Knob 0..1)
        //   Attack    : 10 ms (fest) – fängt Anschlag-Transienten zuverlässig
        //   Release   : 80..300 ms (skaliert mit Amount)
        //   Makeup    : 0..+12 dB (_makeup-Knob)
        //
        //   Topologie  : Feed-forward, Gain-Computer in dB-Domäne.
        //   Stereo     : Linked (lauterer Kanal steuert beide → kein
        //                Stereo-Bild-Versatz unter Kompression).
        //   Envelope   : Gain-domänen-Tracking (glättet den Gain-Verlauf
        //                besser als Level-domänen-Tracking und verhindert
        //                Pumpen/Verzerrungen bei schnellen Transienten).
        // ==================================================================

        if (_compressor_on > 0.5f) {
            // ── Level-Detektion (Linked Stereo) ────────────────────────────
            // Immer den lauteren der beiden Kanäle nehmen → Stereo-Image
            // bleibt unter Kompression stabil.
            float detector = std::max(std::abs(inL), std::abs(inR));

            // ── Gain-Computer (Hard Knee, dB-Domäne) ───────────────────────
            // Threshold fest bei -18 dBFS: deckt den typischen Dynamikbereich
            // des Rhodes MK8 ab, ohne bei leisem Spiel zu stark einzugreifen.
            // thresholdDb / compK precomputed before loop
            float levelDb       = lin2dB(detector + 1e-9f);
            float overDb        = levelDb - thresholdDb;           // Überschuss über Threshold
            float targetGainDb  = (overDb > 0.0f)
                                    ? -overDb * compK
                                    : 0.0f;                        // Gain-Reduktion in dB
            float targetGainLin = db2lin(targetGainDb);

            // ── Attack / Release auf dem Gain-Wert ─────────────────────────
            // Gain-domänen-Tracking: Gain sinkt bei Attack (Transient → diff < 0),
            // steigt bei Release (Gain erholt sich → diff > 0).
            float& env = compressorEnvelope[0]; // [1] wird gespiegelt (Linked)
            float diff = targetGainLin - env;
            env += (diff < 0.0f ? compAttackCoeff : compReleaseCoeff) * diff;
            env = std::clamp(env, 0.05f, 1.0f);
            compressorEnvelope[1] = env; // Linked: rechter Kanal identisch

            // ── Gain + Makeup anwenden ──────────────────────────────────────
            inL *= env * compMakeupGain;
            inR *= env * compMakeupGain;
        }

        if (useEnvelope) {
            inL *= 0.85f; // Schafft Headroom für Chorus/Delay Modulationen
            inR *= 0.85f;
        }
        
        // ==================================================================
        // 6. CHORUS (Bucket-Brigade Style)
        // Stereo-Chorus mit gegenläufiger LFO-Modulation auf L und R.
        //   Basis-Delay : 5 ms + depth×10 ms (5..15 ms Bereich)
        //   Modulations-Tiefe: depth×2 ms (BBD-typische Modulationstiefe)
        //   LFO         : Sinus, gegenphasig für L/R → breites Stereo-Bild
        //   Mix         : 70% Dry + 30% Wet (MK8-typisch, subtil aber wirksam)
        //   Rate        : 0.1..5 Hz logarithmisch
        // ==================================================================
        
        if (_chorus_on > 0.5f && _chorus_depth > 0.01f) {
            // chorusFreq and chorusInc are precomputed before the loop
            
            //float lfo = std::sin(chorusPhase * 2.0f * M_PI);
            float lfo = _sinLut.sin01_nofloor(chorusPhase);
            float delayMs = 5.0f + _chorus_depth * 10.0f;
            float modDepth = _chorus_depth * 2.0f;
            
            float delayL = (delayMs + lfo * modDepth) * sampleRate / 1000.0f;
            float delayR = (delayMs - lfo * modDepth) * sampleRate / 1000.0f;
            
            chorusDelayL.write(inL);
            chorusDelayR.write(inR);
            
            float wetL = chorusDelayL.read(delayL);
            float wetR = chorusDelayR.read(delayR);
            
            inL = inL * 0.7f + wetL * 0.3f;
            inR = inR * 0.7f + wetR * 0.3f;
            
            chorusPhase += chorusInc;
            if (chorusPhase >= 1.0f) chorusPhase -= 1.0f;
        }
        
        // ==================================================================
        // 7. PHASER (4-stufiger All-Pass-Kaskaden-Phaser)
        // Vier First-Order-All-Pass-Filter in Serie erzeugen 4 Notches.
        //   Alpha     : (tan(π·fc/fs) − 1) / (tan(π·fc/fs) + 1)
        //   fc-Sweep  : 200..2000 Hz, LFO-moduliert (0.1..10 Hz)
        //   Feedback  : 0..0.75 (skaliert mit Depth), tanh-gesättigt →
        //               verhindert Aufschaukeln mit dem Wah
        //   Mix       : 50% Dry + 50% Phased (klassischer Notch-Phaser)
        //   Alpha-Update: alle 32 Samples + Smoothing (CPU-Optimierung,
        //               hörbar transparent bei Modulationsfrequenzen < 1 kHz)
        // ==================================================================
        
        if (_phaser_on > 0.5f && _phaser_depth > 0.01f) {
            // 1. Warped Frequency Calculation (Stabiler bei hohen Frequenzen)
            // phaserFreq precomputed before loop
            phaserPhase += phaserFreq / sampleRate;
            if (phaserPhase >= 1.0f) phaserPhase -= 1.0f;

            float minFreq = 200.0f;
            float maxFreq = 2000.0f;

            constexpr int kAlphaUpdateInterval = 32; // try 8..32

            if ((_phaserAlphaCounter++ % kAlphaUpdateInterval) == 0) {
                float lfoValue = _sinLut.sin01(phaserPhase);
                float targetFreq = minFreq + (maxFreq - minFreq) * (lfoValue * 0.5f + 0.5f) * _phaser_depth;

                float tanW0 = fastTan(M_PI * targetFreq / sampleRate);
                _phaserAlphaTarget = (tanW0 - 1.0f) / (tanW0 + 1.0f);
            }

            // smooth alpha each sample (prevents zipper)
            _phaserAlphaCur += 0.05f * (_phaserAlphaTarget - _phaserAlphaCur); // tune 0.02..0.2
            float alpha = _phaserAlphaCur;

            // 2. Feedback-Sättigung (Wichtig!)
            // Ohne Sättigung im Feedback-Pfad schaukelt sich der Phaser mit dem Wah auf.
            float feedbackAmount = 0.75f * _phaser_depth;
            // Wir begrenzen das Feedback-Signal sanft
            float fbL = fastTanh(_phaserLastL * feedbackAmount);
            float fbR = fastTanh(_phaserLastR * feedbackAmount);

            // 3. Processing (Stages)
            float curL = inL + fbL;
            float curR = inR + fbR;

            for (int stage = 0; stage < 4; stage++) {
                curL = phaserFilters[0][stage].process(curL, alpha);
                curR = phaserFilters[1][stage].process(curR, alpha);
            }

            _phaserLastL = curL;
            _phaserLastR = curR;

            // Dry/Wet Mix (MK8 nutzt oft 50/50 für maximalen Notch-Effekt)
            inL = inL * 0.5f + curL * 0.5f;
            inR = inR * 0.5f + curR * 0.5f;
        }
        
        // ==================================================================
        // 8. DELAY (Bucket-Brigade Style, 60ms..800ms)
        // Rhodes MK8: Analoges BBD-Delay mit Feedback-Sättigung.
        //
        //   Topology: Read-then-Write.
        //     1. Delayed signal lesen (= wet UND Feedback-Quelle)
        //     2. Neues Sample + Feedback in Buffer schreiben
        //   → KEIN doppeltes Read nach Write (verhindert 1-Sample-Offset
        //     und sorgt für korrekte Feedback-Schleife).
        //
        //   Delay-Zeit: 60..800 ms logarithmisch (_delay_time 0..1)
        //   Feedback  : 0..1 (_delay_feedback)
        //   Mix       : 0..1 Dry/Wet (_delay_mix)
        // ==================================================================
        
        if (_delay_on > 0.5f && _delay_mix > 0.01f) {
            // delaySamp precomputed before loop
            
            // ── Schritt 1: Delayed signal lesen (wird als Wet UND Feedback genutzt)
            float wetL = delayBufferL.read(delaySamp);
            float wetR = delayBufferR.read(delaySamp);
            
            // ── Schritt 2: Einganssignal + Feedback in Buffer schreiben
            delayBufferL.write(inL + wetL * _delay_feedback);
            delayBufferR.write(inR + wetR * _delay_feedback);
            
            // ── Dry/Wet Mix
            inL = inL * (1.0f - _delay_mix) + wetL * _delay_mix;
            inR = inR * (1.0f - _delay_mix) + wetR * _delay_mix;
        }
        
        // ==================================================================
        // 9. OUTPUT STAGE
        // Volume-Taper  : quadratisch (-60..0 dB) für Audio-Taper-Verhalten
        // Output-Trim   : +7 dB fester Pegel-Ausgleich
        // Soft Limiter  : tanh-basierter Limiter mit 0.98 Ceiling →
        //                 verhindert hartes Clipping bei Effekt-Kombinationen
        // ==================================================================
        
        // vol and outputTrim precomputed before loop
        inL *= vol;
        inR *= vol;

        // Trim Application output +10db
        inL *= outputTrim;
        inR *= outputTrim;
        
        // Soft limiter
        inL = softLimiter(inL);  // ceiling=0.98f
        inR = softLimiter(inR);
        
        outL[i] = inL;
        outR[i] = inR;
    }
}