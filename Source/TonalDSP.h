#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace nx
{
constexpr int tonalBands = 96;
constexpr double tonalPi = 3.14159265358979323846;
using Spectrum = std::array<float, tonalBands>;

inline double bandFrequency(double band)
{
    return 20.0 * std::pow(1000.0, (band + 0.5) / tonalBands);
}

inline int spectralBand(double hz)
{
    return std::clamp(static_cast<int>(std::log(hz / 20.0) * tonalBands / std::log(1000.0)), 0, tonalBands - 1);
}

inline double powerDb(double power)
{
    return 10 * std::log10(std::max(power, 1.0e-14));
}

inline double spectrumEnergy(const Spectrum& power)
{
    double total = 0;
    for (auto bin : power)
        total += bin;
    return total;
}

template <typename Complex>
void fft(Complex* data, const Complex* twiddles, int size)
{
    for (int i = 1, j = 0; i < size; ++i)
    {
        int bit = size >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (int length = 2; length <= size; length *= 2)
        for (int start = 0; start < size; start += length)
            for (int i = 0; i < length / 2; ++i)
            {
                const auto even = data[start + i];
                const auto odd = data[start + i + length / 2] * twiddles[i * (size / length)];
                data[start + i] = even + odd;
                data[start + i + length / 2] = even - odd;
            }
}

struct Bell
{
    double frequency = 1000, gain = 0, q = 0.8;
    bool operator==(const Bell& b) const { return frequency == b.frequency && gain == b.gain && q == b.q; }
};
using Bells = std::array<Bell, 3>;

struct EqualizerSettings
{
    Bells bands {};
    double compensation = 0;
    bool operator==(const EqualizerSettings& b) const { return bands == b.bands && compensation == b.compensation; }
};

struct BellFilter
{
    void prepare(Bell bell, double rate)
    {
        active = std::abs(bell.gain) > 0.0001;
        z1 = z2 = 0;
        if (!active)
            return;
        const double omega = 2 * tonalPi * std::clamp(bell.frequency, 15.0, rate * 0.47) / rate;
        const double alpha = std::sin(omega) / (2 * std::clamp(bell.q, 0.3, 8.0));
        const double a = std::pow(10.0, bell.gain / 40.0);
        const double denominator = 1 + alpha / a;
        b0 = (1 + alpha * a) / denominator;
        b1 = -2 * std::cos(omega) / denominator;
        b2 = (1 - alpha * a) / denominator;
        a1 = b1;
        a2 = (1 - alpha / a) / denominator;
    }

    double process(double in)
    {
        if (!active)
            return in;
        const double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return out;
    }

    double response(double hz, double rate) const
    {
        if (!active)
            return 1;
        const auto z = std::polar(1.0, -2 * tonalPi * hz / rate);
        return std::norm((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
    }

    bool active = false;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

class TonalEqualizer
{
public:
    void prepare(double rate)
    {
        sampleRate = std::max(1000.0, rate);
        fadeLength = std::max(1, static_cast<int>(sampleRate * 0.035));
        active = 0;
        remaining = 0;
        applied = {};
        banks = {};
    }

    void update(const EqualizerSettings& requested)
    {
        if (remaining > 0 || requested == applied)
            return;
        auto& next = banks[1 - active];
        for (auto& channel : next.channels)
            for (std::size_t i = 0; i < requested.bands.size(); ++i)
                channel[i].prepare(requested.bands[i], sampleRate);
        next.gain = std::pow(10.0, requested.compensation / 20.0);
        remaining = fadeLength;
        applied = requested;
    }

    void process(double& left, double& right)
    {
        if (remaining == 0)
        {
            banks[active].process(left, right);
            return;
        }
        auto nextLeft = left, nextRight = right;
        banks[active].process(left, right);
        banks[1 - active].process(nextLeft, nextRight);
        const double mix = 1.0 - static_cast<double>(remaining - 1) / fadeLength;
        left += mix * (nextLeft - left);
        right += mix * (nextRight - right);
        if (--remaining == 0)
            active = 1 - active;
    }

private:
    struct Bank
    {
        std::array<std::array<BellFilter, 3>, 2> channels {};
        double gain = 1;
        void process(double& left, double& right)
        {
            for (auto& filter : channels[0])
                left = filter.process(left);
            for (auto& filter : channels[1])
                right = filter.process(right);
            left *= gain;
            right *= gain;
        }
    };
    std::array<Bank, 2> banks {};
    EqualizerSettings applied;
    double sampleRate = 48000;
    int active = 0, remaining = 0, fadeLength = 1680;
};

inline double estimateCompensation(const Spectrum& input, const Bells& bells, double rate)
{
    std::array<BellFilter, 3> filters;
    for (std::size_t i = 0; i < bells.size(); ++i)
        filters[i].prepare(bells[i], rate);
    double before = 0, after = 0;
    for (int i = 0; i < tonalBands; ++i)
    {
        const auto hz = bandFrequency(i);
        if (hz >= rate * 0.49)
            break;
        double power = input[i];
        before += power;
        for (const auto& filter : filters)
            power *= filter.response(hz, rate);
        after += power;
    }
    return before > 1.0e-10 && after > 1.0e-10 ? std::clamp(powerDb(before / after), -3.0, 3.0) : 0.0;
}

struct Profile
{
    const char* name;
    double warmth, presence, air;
};

inline constexpr std::array<Profile, 10> profiles {{
    {"Worship", 0.3, 0.7, 0.4}, {"Gospel Contemporâneo", 0.5, 0.8, 0.5},
    {"Pop", 0.0, 0.8, 0.8}, {"Pop Rock", -0.2, 0.9, 0.35}, {"Rock", 0.25, 1, -0.25},
    {"Sertanejo", -0.2, 0.5, 0.7}, {"Jazz", 0.55, 0.0, -0.35},
    {"Orquestral", 0.35, 0.1, -0.1}, {"Eletrônico", 0.8, -0.15, 0.8}, {"Personalizado", 0, 0, 0}
}};

inline double bellShape(double hz, double center, double width)
{
    const auto x = std::log2(hz / center) / width;
    return std::exp(-0.5 * x * x);
}

inline Spectrum adaptiveTarget(const Spectrum& source, int category, bool principal, const Profile& profile)
{
    // The measured envelope is the base, so profiles never impose a flat or generic spectrum.
    constexpr double warmth[] {0.2, 0.75, 0.15, -0.3, 0.25, 0.4, -0.35, 0};
    constexpr double articulation[] {0.2, -0.5, 0.1, 0.35, 0.0, 0.25, 0.8, 0};
    category = std::clamp(category, 0, 7);
    Spectrum target {};
    for (int band = 0; band < tonalBands; ++band)
    {
        const auto hz = bandFrequency(band);
        // Smooth local power without allowing a neighbouring region to invent a target for silence.
        double power = 0, weight = 0;
        for (int adjacent = std::max(0, band - 5); adjacent <= std::min(tonalBands - 1, band + 5); ++adjacent)
        {
            const auto w = 6.0 - std::abs(adjacent - band);
            power += source[adjacent] * w;
            weight += w;
        }
        const auto tilt = (profile.warmth + warmth[category]) * bellShape(hz, 160, 1.2)
            + (profile.presence + articulation[category] + (principal ? 0.45 : -0.2)) * bellShape(hz, 2300, 1.0)
            + profile.air * bellShape(hz, 9000, 0.9);
        target[band] = static_cast<float>(0.55 * source[band] + 0.45 * power / weight)
            * static_cast<float>(std::pow(10.0, tilt / 10.0));
    }
    return target;
}

inline Bells match(const Spectrum& current, const Spectrum& target, double intensity, double limit,
                   double toleranceDb = 0.0)
{
    Bells result {};
    const auto currentTotal = spectrumEnergy(current), targetTotal = spectrumEnergy(target);
    if (currentTotal < 1.0e-9 || targetTotal < 1.0e-9)
        return result;
    const auto floor = *std::max_element(current.begin(), current.end()) * 0.002;
    std::array<double, tonalBands> differences {};
    for (int bin = 1; bin < tonalBands - 1; ++bin)
    {
        if (current[bin] <= std::max(1.0e-10, floor))
            continue;
        const int from = std::max(0, bin - 3), to = std::min(tonalBands - 1, bin + 3);
        double a = 0, b = 0;
        for (int k = from; k <= to; ++k)
        {
            a += current[k] / currentTotal;
            b += target[k] / targetTotal;
        }
        const auto difference = powerDb(b) - powerDb(a);
        const auto outside = std::max(0.0, std::abs(difference) - toleranceDb);
        differences[bin] = std::clamp(intensity * std::copysign(outside, difference), -limit, limit);
    }
    for (auto& bell : result)
    {
        const auto found = std::max_element(differences.begin(), differences.end(),
            [](double a, double b) { return std::abs(a) < std::abs(b); });
        if (std::abs(*found) < 0.20)
            break;
        const int band = static_cast<int>(found - differences.begin());
        // Split the cap across the three overlapping bells to bound the total boost/cut.
        bell = {bandFrequency(band), std::clamp(*found, -limit / 3.0, limit / 3.0), 0.95};
        std::fill(differences.begin() + std::max(0, band - 11),
                  differences.begin() + std::min(tonalBands, band + 12), 0.0);
    }
    return result;
}

inline void limitCorrection(Bells& bells, double limit)
{
    double budget = 0;
    for (const auto& bell : bells)
        budget += std::abs(bell.gain);
    if (budget > limit && budget > 0)
        for (auto& bell : bells)
            bell.gain *= limit / budget;
}

struct Conflict
{
    int band = -1;
    double score = 0;
};

inline Conflict masking(const Spectrum& focus, const Spectrum& other, double focusPan, double otherPan,
                       double fromHz = 20, double toHz = 20000, double priorityWeight = 1.0)
{
    Conflict result;
    const auto focusMax = *std::max_element(focus.begin(), focus.end());
    const double proximity = std::clamp(1.0 - std::abs(focusPan - otherPan) * 0.65, 0.0, 1.0);
    for (int band = std::max(2, spectralBand(fromHz)); band <= std::min(tonalBands - 3, spectralBand(toHz)); ++band)
    {
        if (focus[band] < std::max(1.0e-9f, focusMax * 0.1f) || other[band] < focus[band] * 0.4f)
            continue;
        const auto score = std::min(focus[band], other[band]) * proximity * priorityWeight;
        if (score > result.score)
            result = {band, score};
    }
    return result;
}
}
