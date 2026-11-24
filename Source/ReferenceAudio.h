#pragma once

#include "TonalDSP.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace nx
{
struct ReferenceAudio
{
    Spectrum curve {};
    double loudness = -120, rms = -120, peak = -120, crest = 0, balance = 0, correlation = 0, duration = 0;
    bool valid = false;
};

struct KSection
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double sample)
    {
        const auto out = b0 * sample + z1;
        z1 = b1 * sample - a1 * out + z2;
        z2 = b2 * sample - a2 * out;
        return out;
    }
};

inline std::array<KSection, 2> kWeighting(double rate)
{
    const auto shelf = std::tan(tonalPi * 1681.974450955533 / rate);
    const auto pass = std::tan(tonalPi * 38.13547087602444 / rate);
    constexpr double sq = 0.7071752369554196, pq = 0.5003270373238773;
    const double high = std::pow(10.0, 3.999843853973347 / 20.0);
    const double intermediate = std::pow(high, 0.4996667741545416);
    const auto sa = 1 + shelf / sq + shelf * shelf, pa = 1 + pass / pq + pass * pass;
    return {{{(high + intermediate * shelf / sq + shelf * shelf) / sa,
              2 * (shelf * shelf - high) / sa,
              (high - intermediate * shelf / sq + shelf * shelf) / sa,
              2 * (shelf * shelf - 1) / sa, (1 - shelf / sq + shelf * shelf) / sa},
             {1, -2, 1, 2 * (pass * pass - 1) / pa, (1 - pass / pq + pass * pass) / pa}}};
}

inline double gatedLoudness(const std::vector<double>& blocks)
{
    constexpr auto absoluteGate = -70.0;
    double first = 0;
    unsigned count = 0;
    for (auto block : blocks)
        if (powerDb(block) - 0.691 >= absoluteGate)
        {
            first += block;
            ++count;
        }
    if (count == 0)
        return -120;
    const auto relativeGate = powerDb(first / count) - 0.691 - 10;
    const auto threshold = std::max(absoluteGate, relativeGate);
    double second = 0;
    count = 0;
    for (auto block : blocks)
        if (powerDb(block) - 0.691 >= threshold)
        {
            second += block;
            ++count;
        }
    return count > 0 ? powerDb(second / count) - 0.691 : -120;
}

inline ReferenceAudio loadReferenceAudio(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
    ReferenceAudio result;
    if (!reader || reader->sampleRate < 8000 || reader->sampleRate > 384000 || reader->lengthInSamples <= 0
        || reader->numChannels == 0 || reader->numChannels > 2)
        return result;

    const auto rate = reader->sampleRate;
    const bool stereo = reader->numChannels == 2;
    const auto length = std::min<juce::int64>(reader->lengthInSamples, static_cast<juce::int64>(120 * rate));
    constexpr int n = 8192;
    juce::AudioBuffer<float> buffer(2, n);
    std::vector<std::complex<double>> packed(n), rotations(n / 2);
    std::vector<double> window(n);
    std::vector<double> blocks;
    blocks.reserve(1200);
    for (int i = 0; i < n; ++i)
    {
        window[i] = 0.5 - 0.5 * std::cos(2 * tonalPi * i / n);
        if (i < n / 2)
            rotations[i] = std::polar(1.0, -2 * tonalPi * i / n);
    }
    std::array<std::array<KSection, 2>, 2> filters {kWeighting(rate), kWeighting(rate)};
    std::array<double, 4> hops {};
    const int hop = std::max(1, static_cast<int>(rate * 0.1));
    double leftEnergy = 0, rightEnergy = 0, cross = 0, peak = 0, hopEnergy = 0;
    int hopCount = 0, hopNumber = 0, spectra = 0;

    for (juce::int64 offset = 0; offset < length; offset += n)
    {
        const auto amount = static_cast<int>(std::min<juce::int64>(n, length - offset));
        if (!reader->read(&buffer, 0, amount, offset, true, stereo))
            break;
        double energy = 0;
        for (int i = 0; i < amount; ++i)
        {
            const auto safe = [](float f) { return std::isfinite(f) ? std::clamp(static_cast<double>(f), -16.0, 16.0) : 0.0; };
            const double l = safe(buffer.getSample(0, i)), r = stereo ? safe(buffer.getSample(1, i)) : l;
            leftEnergy += l * l;
            rightEnergy += r * r;
            cross += l * r;
            energy += l * l + r * r;
            peak = std::max({peak, std::abs(l), std::abs(r)});
            const auto kl = filters[0][1].process(filters[0][0].process(l));
            const auto kr = filters[1][1].process(filters[1][0].process(r));
            hopEnergy += kl * kl + (stereo ? kr * kr : 0);
            if (++hopCount == hop)
            {
                hops[static_cast<std::size_t>(hopNumber % 4)] = hopEnergy / hop;
                if (++hopNumber >= 4)
                    blocks.push_back(hops[0] * 0.25 + hops[1] * 0.25 + hops[2] * 0.25 + hops[3] * 0.25);
                hopEnergy = 0;
                hopCount = 0;
            }
            packed[i] = {l * window[i], r * window[i]};
        }
        if (energy / std::max(1, amount) < 1.0e-8)
            continue;
        std::fill(packed.begin() + amount, packed.end(), std::complex<double>{});
        fft(packed.data(), rotations.data(), n);
        for (int i = 1; i < n / 2; ++i)
        {
            const auto hz = i * rate / n;
            if (hz < 20 || hz > 20000)
                continue;
            const auto other = std::conj(packed[n - i]);
            const auto left = std::norm((packed[i] + other) * 0.5);
            const auto right = std::norm((packed[i] - other) * 0.5);
            result.curve[spectralBand(hz)] += static_cast<float>((left + right) * 8.0 / (3.0 * n * n));
        }
        ++spectra;
    }
    if (spectra == 0)
        return result;
    for (auto& bin : result.curve)
        bin /= spectra;
    result.valid = true;
    result.duration = length / rate;
    result.loudness = gatedLoudness(blocks);
    result.rms = powerDb((leftEnergy + rightEnergy) * 0.5 / length);
    result.peak = powerDb(peak * peak);
    result.crest = result.peak - result.rms;
    result.balance = (rightEnergy - leftEnergy) / (rightEnergy + leftEnergy);
    result.correlation = cross / std::sqrt(std::max(1.0e-30, leftEnergy * rightEnergy));
    return result;
}
}
