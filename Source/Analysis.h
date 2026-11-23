#pragma once

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

enum Role { automatic, focus, bass, drums, music };

struct Reading
{
    float rms = floorDb;
    float peak = floorDb;
    float outputPeak = floorDb;
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
        frames = 0;
    }

    bool push(float left, float right, float outLeft, float outRight,
              std::int64_t position, Reading& result)
    {
        if (position != nextPosition && (position >= 0 || nextPosition >= 0))
        {
            count = 0;
            sum = peak = outputPeak = 0;
            meanSquare = recentPeak = recentOutputPeak = 0;
            frames = 0;
        }
        nextPosition = position >= 0 ? position + 1 : -1;
        if (position >= 0 && count == 0 && position % fftSize != 0)
            return false;

        signalLeft[count] = left;
        signalRight[count] = right;
        sum += (left * left + right * right) * 0.5f;
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
        result.sampleRate = sampleRate;
        if (frames < sampleRate)
            frames += fftSize;
        result.settled = frames >= sampleRate * 3 / 4 && sum / fftSize > 1.0e-6f;
        result.position = position;
        if (peak > 0.0001f)
        {
            addSpectrum(signalLeft, result.power);
            addSpectrum(signalRight, result.power);
        }
        count = 0;
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
    int sampleRate = 0, count = 0, frames = 0;
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

inline GainSuggestion suggestTrim(const Reading& reading, GainMode mode, float reference,
                                  Role role, float maxTrim, float headroom)
{
    if (!reading.active())
        return {};
    float offset = 0;
    if (mode == GainMode::musical)
        offset = role == focus ? 0.0f
            : role == bass || (role == automatic && bassHeavy(reading)) ? -3.0f
            : role == drums ? -2.0f : -5.0f;
    const float wanted = reference + offset - reading.rms;
    const float safe = -headroom - reading.peak;
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
