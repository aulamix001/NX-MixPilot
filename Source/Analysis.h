#pragma once

#include "GainMeter.h"
#include "LoudnessMeter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>

namespace nx
{
constexpr int fftSize = 2048;
constexpr int bands = 24;
constexpr float floorDb = -120.0f;
constexpr float pi = 3.14159265358979323846f;

inline float db(float amplitude)
{
    return amplitude > 1.0e-6f ? 20.0f * std::log10(amplitude) : floorDb;
}

inline float frequency(int edge)
{
    return 20.0f * std::pow(1000.0f, static_cast<float>(edge) / bands);
}

enum Role { automatic, focus, bass, drums, music, backingVocal, harmony, auxiliaryPercussion, ambience };

struct Reading
{
    float rms = floorDb;
    float peak = floorDb;
    float outputPeak = floorDb;
    float activeRms = floorDb, activity = 0;
    StereoLevels inputLevels {}, outputLevels {};
    LoudnessLevels inputLoudness, outputLoudness;
    float learnedPeak = floorDb, learnedCeiling = floorDb, learnedMaxSample = floorDb;
    unsigned learningId = 0, learnedActiveWindows = 0;
    float learnedEnergy = floorDb, learnedLoudEnergy = floorDb;
    int sampleRate = 0;
    bool settled = false;
    std::int64_t position = -1;
    std::array<float, bands> power {};

    bool active() const { return settled && rms > -60.0f; }
};

class Analyzer
{
public:
    Analyzer()
    {
        for (int i = 0; i < fftSize; ++i)
            window[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / fftSize);
        for (int i = 0; i < fftSize / 2; ++i)
            rotation[i] = std::polar(1.0f, -2.0f * pi * i / fftSize);
    }

    void prepare(int rate)
    {
        sampleRate = rate;
        reset();
        for (int bin = 0; bin < fftSize / 2; ++bin)
        {
            const float hz = static_cast<float>(bin) * rate / fftSize;
            binBand[bin] = hz < 20 || hz >= 20000 ? -1
                : std::clamp(static_cast<int>(std::log(hz / 20.0f) * bands / std::log(1000.0f)), 0, bands - 1);
        }
    }

    void reset()
    {
        count = 0;
        nextPosition = -1;
        sum = peak = outputPeak = 0;
        meanSquare = 0;
        recentPeak = 0;
        recentOutputPeak = 0;
        frames = activeSamples = 0;
        activePower = 0;
    }

    bool push(float left, float right, float outLeft, float outRight,
              std::int64_t position, Reading& result, bool withSpectrum = true)
    {
        if (position != nextPosition && (position >= 0 || nextPosition >= 0))
        {
            count = 0;
            sum = peak = outputPeak = 0;
            meanSquare = recentPeak = recentOutputPeak = 0;
            frames = activeSamples = 0;
            activePower = 0;
        }
        nextPosition = position >= 0 ? position + 1 : -1;
        if (position >= 0 && count == 0 && position % fftSize != 0)
            return false;

        if (withSpectrum)
        {
            signalLeft[count] = left;
            signalRight[count] = right;
        }
        const float energy = (left * left + right * right) * 0.5f;
        sum += energy;
        if (energy > 1.0e-6f)
        {
            activePower += energy;
            ++activeSamples;
        }
        peak = std::max({peak, std::abs(left), std::abs(right)});
        outputPeak = std::max({outputPeak, std::abs(outLeft), std::abs(outRight)});
        if (++count != fftSize)
            return false;

        const float seconds = static_cast<float>(fftSize) / std::max(sampleRate, 1);
        const float retention = std::exp(-seconds / 0.35f);
        meanSquare = meanSquare * retention + (sum / fftSize) * (1 - retention);
        recentPeak = std::max(peak, recentPeak * std::exp(-seconds / 1.0f));
        recentOutputPeak = std::max(outputPeak, recentOutputPeak * std::exp(-seconds / 1.0f));
        result = {};
        result.rms = db(std::sqrt(meanSquare));
        result.peak = db(recentPeak);
        result.outputPeak = db(recentOutputPeak);
        result.activeRms = activeSamples > 0 ? db(std::sqrt(activePower / activeSamples)) : floorDb;
        result.activity = static_cast<float>(activeSamples) / fftSize;
        result.sampleRate = sampleRate;
        if (frames < sampleRate)
            frames += fftSize;
        result.settled = frames >= sampleRate * 3 / 4 && sum / fftSize > 1.0e-6f;
        result.position = position;
        if (withSpectrum && peak > 0.0001f)
        {
            addSpectrum(signalLeft, result.power);
            addSpectrum(signalRight, result.power);
        }
        count = activeSamples = 0;
        activePower = 0;
        sum = peak = outputPeak = 0;
        return true;
    }

private:
    void addSpectrum(const std::array<float, fftSize>& signal, std::array<float, bands>& power)
    {
        for (int i = 0, reversed = 0; i < fftSize; ++i)
        {
            scratch[reversed] = signal[i] * window[i];
            int bit = fftSize / 2;
            while (bit > 0 && (reversed & bit))
            {
                reversed ^= bit;
                bit >>= 1;
            }
            reversed ^= bit;
        }
        for (int size = 2; size <= fftSize; size *= 2)
            for (int start = 0; start < fftSize; start += size)
                for (int j = 0; j < size / 2; ++j)
                {
                    const auto even = scratch[start + j];
                    const auto odd = scratch[start + j + size / 2] * rotation[j * (fftSize / size)];
                    scratch[start + j] = even + odd;
                    scratch[start + j + size / 2] = even - odd;
                }
        for (int bin = 1; bin < fftSize / 2; ++bin)
            if (binBand[bin] >= 0)
                power[binBand[bin]] += std::norm(scratch[bin]) * (8.0f / (3.0f * fftSize * fftSize));
    }

    std::array<float, fftSize> window {}, signalLeft {}, signalRight {};
    std::array<std::complex<float>, fftSize> scratch {};
    std::array<std::complex<float>, fftSize / 2> rotation {};
    std::array<int, fftSize / 2> binBand {};
    int sampleRate = 0, count = 0, frames = 0, activeSamples = 0;
    float activePower = 0;
    std::int64_t nextPosition = -1;
    float sum = 0, peak = 0, outputPeak = 0, meanSquare = 0, recentPeak = 0, recentOutputPeak = 0;
};

inline bool bassHeavy(const Reading& reading)
{
    float low = 0, total = 0;
    for (int band = 0; band < bands; ++band)
    {
        total += reading.power[band];
        if (frequency(band + 1) <= 200.0f)
            low += reading.power[band];
    }
    return total > 1.0e-8f && low > total * 0.65f;
}

constexpr float nominalRms = -18.0f;

enum class GainMode { calibration, musical };

struct GainSuggestion
{
    float trim = 0;
    bool headroomLimited = false;
    bool headroomUnmet = false;
};

struct PeakRange
{
    float minimum;
    float maximum;
    float target;
};

inline constexpr std::array<PeakRange, 10> instrumentPeakRanges {{
    {-12, -6, -10}, {-12, -6, -10}, {-20, -12, -16}, {-12, -6, -10}, {-18, -10, -14},
    {-18, -10, -14}, {-24, -12, -18}, {-18, -8, -13}, {-12, -6, -10}, {-18, -10, -14}
}};

inline GainSuggestion suggestTrim(const Reading& reading, GainMode mode, float reference,
                                  Role role, float maxTrim, float headroom, int peakProfile = 0, float customPeak = 1.0f,
                                  float ceiling = floorDb, bool capturedPeak = false)
{
    if (!reading.active() && !capturedPeak)
        return {};
    float offset = 0;
    if (mode == GainMode::musical)
    {
        switch (role)
        {
            case focus: offset = 0; break;
            case bass: offset = -3; break;
            case drums: offset = -2; break;
            case backingVocal: offset = -6; break;
            case harmony: offset = -7; break;
            case auxiliaryPercussion: offset = -9; break;
            case ambience: offset = -12; break;
            case automatic: offset = bassHeavy(reading) ? -3.0f : -5.0f; break;
            case music: offset = -5; break;
        }
    }
    float wanted = reference + offset - reading.rms;
    if (peakProfile > 0 && peakProfile <= static_cast<int>(instrumentPeakRanges.size())
        && !(mode == GainMode::musical && role == focus))
    {
        const auto range = instrumentPeakRanges[static_cast<std::size_t>(peakProfile - 1)];
        wanted = mode == GainMode::calibration
            ? range.target - reading.peak
            : std::clamp(wanted, range.minimum - reading.peak, range.maximum - reading.peak);
    }
    if (customPeak <= 0 && !(mode == GainMode::musical && role == focus))
        wanted = mode == GainMode::calibration ? customPeak - reading.peak
            : std::min(wanted, customPeak - reading.peak);
    const float safe = -headroom - std::max(reading.peak, ceiling);
    const float trim = std::clamp(std::min(wanted, safe), -maxTrim, maxTrim);
    return {trim, wanted > safe, trim > safe + 0.001f};
}
// The loudest current RMS is the group reference. Every member receives the
// same step, bounded by the worst sample peak and each member's trim range.
class GroupCalibration
{
public:
    GroupCalibration(float maximumStep, float headroom)
        : margin(headroom), lower(-maximumStep), upper(maximumStep) {}

    void include(const Reading& reading, float currentTrim)
    {
        highestRms = std::max(highestRms, reading.rms + currentTrim);
        lower = std::max(lower, -24.0f - currentTrim);
        upper = std::min({upper, 24.0f - currentTrim, -margin - reading.peak - currentTrim});
    }

    bool possible() const { return highestRms > floorDb && lower <= upper; }
    float step(float reference) const
    {
        return possible() ? std::clamp(reference - highestRms, lower, upper) : 0.0f;
    }

private:
    float margin, lower, upper, highestRms = floorDb;
};

struct Overlap
{
    int band = -1;
    float strength = 0;
};

inline Overlap findOverlap(const Reading& focusTrack, const Reading& other)
{
    Overlap result;
    if (!focusTrack.active() || !other.active() || focusTrack.position < 0
        || focusTrack.position != other.position || focusTrack.sampleRate != other.sampleRate)
        return result;
    const float strongest = *std::max_element(focusTrack.power.begin(), focusTrack.power.end());
    for (int band = 0; band < bands; ++band)
    {
        const float focusPower = focusTrack.power[band], otherPower = other.power[band];
        if (focusPower < std::max(strongest * 0.08f, 1.0e-7f) || otherPower < focusPower * 0.5f)
            continue;
        const float strength = std::min(focusPower, otherPower);
        if (strength > result.strength)
            result = {band, strength};
    }
    return result;
}
}
