#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

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
constexpr int maxTonalFilters = 12;
using Bells = std::array<Bell, maxTonalFilters>;
inline int activeBands(const Bells& bells)
{
    return static_cast<int>(std::count_if(bells.begin(), bells.end(),
        [](const auto& bell) { return std::abs(bell.gain) >= 0.0001; }));
}

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
        next.used = 0;
        for (const auto& band : requested.bands)
            if (std::abs(band.gain) >= 0.0001)
            {
                for (auto& channel : next.channels)
                    channel[next.used].prepare(band, sampleRate);
                ++next.used;
            }
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
        std::array<std::array<BellFilter, maxTonalFilters>, 2> channels {};
        double gain = 1;
        std::size_t used = 0;
        void process(double& left, double& right)
        {
            for (std::size_t i = 0; i < used; ++i)
            {
                left = channels[0][i].process(left);
                right = channels[1][i].process(right);
            }
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
    std::array<BellFilter, maxTonalFilters> filters;
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
    return before > 1.0e-10 && after > 1.0e-10 ? powerDb(before / after) : 0.0;
}

struct Profile
{
    const char* name;
    double warmth, presence, air;
};

inline constexpr std::array<Profile, 13> profiles {{
    {"Worship", 0.3, 0.7, 0.4}, {"Gospel Contemporâneo", 0.5, 0.8, 0.5},
    {"Pop", 0.0, 0.8, 0.8}, {"Pop Rock", -0.2, 0.9, 0.35}, {"Rock", 0.25, 1, -0.25},
    {"Sertanejo", -0.2, 0.5, 0.7}, {"Jazz", 0.55, 0.0, -0.35},
    {"Orquestral", 0.35, 0.1, -0.1}, {"Eletrônico", 0.8, -0.15, 0.8}, {"Personalizado", 0, 0, 0},
    {"Acústico", 0.45, 0.15, 0.15}, {"Hip-Hop", 0.9, 0.5, 0.25}, {"Cinematográfico", 0.5, 0.1, 0.5}
}};

inline double bellShape(double hz, double center, double width)
{
    const auto x = std::log2(hz / center) / width;
    return std::exp(-0.5 * x * x);
}

enum class SourceProfile { family, vocal, backing, acoustic, electric, piano, bass, drums, pad, bus };

inline Spectrum adaptiveTarget(const Spectrum& source, int category, bool principal, const Profile& profile,
                               SourceProfile instrument = SourceProfile::family)
{
    // The measured envelope is the base, so profiles never impose a flat or generic spectrum.
    constexpr double warmth[] {0.2, 0.75, 0.15, -0.3, 0.25, 0.4, -0.35, 0};
    constexpr double articulation[] {0.2, -0.5, 0.1, 0.35, 0.0, 0.25, 0.8, 0};
    category = std::clamp(category, 0, 7);
    struct Anchor { double hz, width, gain; };
    constexpr std::array<Anchor, 10> anchors {{
        {1000, 1, 0}, {3100, 1.1, 0.55}, {700, 1.5, 0.25}, {2500, 1.1, 0.40},
        {1500, 1.1, 0.35}, {850, 1.25, 0.30}, {90, 0.85, 0.60},
        {150, 1.4, 0.35}, {500, 1.7, 0.20}, {180, 1.4, 0.15}
    }};
    const auto anchor = anchors[static_cast<std::size_t>(instrument)];
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
            + profile.air * bellShape(hz, 9000, 0.9) + anchor.gain * bellShape(hz, anchor.hz, anchor.width);
        target[band] = static_cast<float>(0.55 * source[band] + 0.45 * power / weight)
            * static_cast<float>(std::pow(10.0, tilt / 10.0));
    }
    return target;
}

inline double eqResponse(const Bells& bells, double hz, double rate)
{
    double db = 0;
    for (const auto& bell : bells)
    {
        if (bell.gain == 0)
            continue;
        BellFilter filter;
        filter.prepare(bell, rate);
        db += powerDb(filter.response(hz, rate));
    }
    return db;
}

inline void limitCorrection(Bells& bells, double limit, double rate = 48000)
{
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        std::array<BellFilter, maxTonalFilters> filters;
        for (std::size_t i = 0; i < bells.size(); ++i)
            filters[i].prepare(bells[i], rate);
        const auto magnitude = [&](double hz)
        {
            double db = 0;
            for (const auto& filter : filters)
                if (filter.active)
                    db += powerDb(filter.response(hz, rate));
            return std::abs(db);
        };
        double peak = 0;
        const auto upper = std::min(20000.0, rate * 0.49);
        for (int i = 0; i <= 768; ++i)
            peak = std::max(peak, magnitude(20 * std::pow(upper / 20, i / 768.0)));
        for (const auto& bell : bells)
            if (bell.gain != 0)
                peak = std::max(peak, magnitude(std::min(bell.frequency, upper)));
        // Leave room for peaks falling between adjacent evaluation frequencies.
        const auto ceiling = std::max(0.0, limit - 0.02);
        if (peak <= ceiling || peak == 0)
            return;
        for (auto& bell : bells)
            bell.gain *= ceiling / peak;
    }
}

struct TonalDifference
{
    std::array<double, tonalBands> db {}, weight {};
    double totalWeight = 0;

    void center(std::array<double, tonalBands>& values) const
    {
        if (totalWeight == 0)
            return;
        double mean = 0;
        for (int i = 0; i < tonalBands; ++i)
            mean += values[i] * weight[i];
        mean /= totalWeight;
        for (auto& value : values)
            value -= mean;
    }

    double error(const Bells& bells, double rate) const
    {
        if (totalWeight == 0)
            return 0;
        auto residual = db;
        for (int i = 0; i < tonalBands; ++i)
            if (weight[i] > 0)
                residual[i] -= eqResponse(bells, bandFrequency(i), rate);
        center(residual);
        double sum = 0;
        for (int i = 0; i < tonalBands; ++i)
            sum += weight[i] * residual[i] * residual[i];
        return std::sqrt(sum / totalWeight);
    }
};

inline TonalDifference tonalDifference(const Spectrum& current, const Spectrum& target, int smooth, double rate,
                                      double low = 20, double high = 20000)
{
    TonalDifference difference;
    const auto inputPeak = *std::max_element(current.begin(), current.end());
    const auto targetPeak = *std::max_element(target.begin(), target.end());
    if (inputPeak < 1.0e-10 || targetPeak < 1.0e-10)
        return difference;
    std::array<double, tonalBands> raw {};
    for (int i = 0; i < tonalBands; ++i)
        if (current[i] > inputPeak * 0.0002 && target[i] > targetPeak * 0.00005 && bandFrequency(i) < rate * 0.47
            && bandFrequency(i) >= low && bandFrequency(i) <= high)
        {
            raw[i] = powerDb(target[i] / static_cast<double>(targetPeak))
                   - powerDb(current[i] / static_cast<double>(inputPeak));
            difference.weight[i] = std::clamp(std::sqrt(current[i] / (inputPeak * 0.08)), 0.06, 1.0);
            difference.totalWeight += difference.weight[i];
        }
    smooth = std::clamp(smooth, 0, 6);
    for (int i = 0; i < tonalBands; ++i)
    {
        if (difference.weight[i] == 0)
            continue;
        double total = 0;
        for (int k = std::max(0, i - smooth); k <= std::min(tonalBands - 1, i + smooth); ++k)
        {
            const double weight = (smooth + 1 - std::abs(i - k)) * difference.weight[k];
            difference.db[i] += weight * raw[k];
            total += weight;
        }
        difference.db[i] /= total;
    }
    difference.center(difference.db);
    return difference;
}

inline Bells match(const Spectrum& current, const Spectrum& target, double intensity, double limit,
                   double toleranceDb = 0.35, int smooth = 2, double rate = 48000,
                   double low = 20, double high = 20000)
{
    Bells result {};
    intensity = std::clamp(intensity, 0.0, 1.0);
    if (intensity == 0 || limit <= 0)
        return result;
    const auto difference = tonalDifference(current, target, smooth, rate, low, high);
    if (difference.totalWeight == 0)
        return result;
    auto residual = difference.db;
    constexpr std::array<double, 5> widths {0.45, 0.75, 1.1, 1.8, 3.2};
    struct Candidate
    {
        int index;
        double q, norm;
        std::array<double, tonalBands> basis;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(tonalBands * widths.size());
    const auto weight = difference.weight;
    std::array<double, tonalBands> frequencies {};
    for (int i = 0; i < tonalBands; ++i)
        frequencies[i] = bandFrequency(i);
    for (int i = 0; i < tonalBands; ++i)
        if (weight[i] != 0)
            for (const auto q : widths)
            {
                Candidate candidate {i, q, 0, {}};
                BellFilter filter;
                filter.prepare({frequencies[i], 1, q}, rate);
                for (int k = 0; k < tonalBands; ++k)
                    if (weight[k] != 0)
                        candidate.basis[k] = powerDb(filter.response(frequencies[k], rate));
                difference.center(candidate.basis);
                for (int k = 0; k < tonalBands; ++k)
                    candidate.norm += weight[k] * candidate.basis[k] * candidate.basis[k];
                if (candidate.norm > 1e-8)
                    candidates.push_back(candidate);
            }

    for (auto& band : result)
    {
        double best = 0;
        for (const auto& candidate : candidates)
        {
            const auto i = candidate.index;
            if (std::abs(residual[i]) <= toleranceDb)
                continue;
            double dot = 0;
            for (int k = 0; k < tonalBands; ++k)
                dot += weight[k] * candidate.basis[k] * residual[k];
            const auto boostCap = weight[i] < 0.22 ? limit * 0.4 : limit;
            const auto gain = std::clamp(dot / candidate.norm, -limit, boostCap);
            const auto score = gain * (2 * dot - gain * candidate.norm);
            if (std::abs(gain) >= 0.22 && score > best)
            {
                best = score;
                band = {frequencies[i], gain, candidate.q};
            }
        }
        if (best <= difference.totalWeight * 0.0008)
        {
            band = {};
            break;
        }
        std::array<double, tonalBands> response {};
        BellFilter filter;
        filter.prepare(band, rate);
        for (int k = 0; k < tonalBands; ++k)
            if (weight[k] > 0)
                response[k] = powerDb(filter.response(frequencies[k], rate));
        difference.center(response);
        double improvement = 0;
        for (int k = 0; k < tonalBands; ++k)
            improvement += weight[k] * response[k] * (2 * residual[k] - response[k]);
        if (improvement <= 0)
        {
            band = {};
            break;
        }
        for (int k = 0; k < tonalBands; ++k)
            residual[k] -= response[k];
    }
    limitCorrection(result, limit, rate);
    const auto acceptableError = difference.error(result, rate) + std::max(0.04, toleranceDb * 0.12);
    for (auto band = result.rbegin(); band != result.rend(); ++band)
        if (band->gain != 0)
        {
            const auto previous = *band;
            *band = {};
            if (difference.error(result, rate) > acceptableError)
                *band = previous;
        }
    limitCorrection(result, limit, rate);
    for (auto& band : result)
        band.gain *= intensity;
    limitCorrection(result, limit, rate);
    return result;
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
