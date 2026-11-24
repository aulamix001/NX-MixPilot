#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nx
{
struct LoudnessLevels
{
    float momentary = -120, shortTerm = -120, integrated = -120;
};

// Stereo/mono BS.1770 weighting with an EBU-style 100 ms update cadence.
class LoudnessMeter
{
public:
    void prepare(double rate)
    {
        const auto kShelf = std::tan(3.141592653589793 * 1681.974450955533 / rate);
        const auto kHighPass = std::tan(3.141592653589793 * 38.13547087602444 / rate);
        const double qShelf = 0.7071752369554196, qHighPass = 0.5003270373238773;
        const auto high = std::pow(10.0, 3.999843853973347 / 20);
        const auto middle = std::pow(high, 0.4996667741545416);
        const auto shelfDen = 1 + kShelf / qShelf + kShelf * kShelf;
        const auto passDen = 1 + kHighPass / qHighPass + kHighPass * kHighPass;
        const std::array<double, 5> shelf {
            (high + middle * kShelf / qShelf + kShelf * kShelf) / shelfDen,
            2 * (kShelf * kShelf - high) / shelfDen,
            (high - middle * kShelf / qShelf + kShelf * kShelf) / shelfDen,
            2 * (kShelf * kShelf - 1) / shelfDen,
            (1 - kShelf / qShelf + kShelf * kShelf) / shelfDen
        };
        const std::array<double, 5> highPass {1, -2, 1, 2 * (kHighPass * kHighPass - 1) / passDen,
                                              (1 - kHighPass / qHighPass + kHighPass * kHighPass) / passDen};
        for (auto& channel : channels)
        {
            channel[0].coefficients = shelf;
            channel[1].coefficients = highPass;
        }
        samplesPerStep = std::max(1, static_cast<int>(std::lround(rate / 10)));
        reset();
    }

    void reset()
    {
        samples = steps = cursor = 0;
        sum = totalEnergy = 0;
        totalBlocks = 0;
        history.fill(0);
        histogram.fill({});
        levels = {};
        for (auto& channel : channels)
            for (auto& filter : channel)
                filter.z1 = filter.z2 = 0;
    }

    void push(double left, double right, bool stereo)
    {
        sum += weightedEnergy(0, left);
        if (stereo)
            sum += weightedEnergy(1, right);
        if (++samples < samplesPerStep)
            return;

        history[cursor] = sum / samples;
        cursor = (cursor + 1) % static_cast<int>(history.size());
        steps = std::min(steps + 1, static_cast<int>(history.size()));
        samples = 0;
        sum = 0;
        if (steps >= 4)
        {
            double momentary = 0;
            for (int i = 1; i <= 4; ++i)
                momentary += history[(cursor + static_cast<int>(history.size()) - i) % history.size()];
            momentary /= 4;
            levels.momentary = loudness(momentary);
            if (levels.momentary >= -70)
            {
                auto& bin = histogram[binIndex(levels.momentary)];
                ++bin.count;
                bin.energy += momentary;
                ++totalBlocks;
                totalEnergy += momentary;
                const float gate = std::max(-70.0f, loudness(totalEnergy / totalBlocks) - 10.0f);
                double energy = 0;
                std::uint64_t count = 0;
                const auto gateBin = binIndex(gate);
                for (std::size_t i = gateBin; i < histogram.size(); ++i)
                {
                    if (histogram[i].count == 0)
                        continue;
                    if (i == gateBin && loudness(histogram[i].energy / histogram[i].count) < gate)
                        continue;
                    energy += histogram[i].energy;
                    count += histogram[i].count;
                }
                levels.integrated = count != 0 ? loudness(energy / count) : -120;
            }
        }
        if (steps == static_cast<int>(history.size()))
        {
            double shortTerm = 0;
            for (const auto energy : history)
                shortTerm += energy;
            levels.shortTerm = loudness(shortTerm / history.size());
        }
    }

    LoudnessLevels read() const { return levels; }

private:
    struct Filter
    {
        std::array<double, 5> coefficients {};
        double z1 = 0, z2 = 0;
        double process(double in)
        {
            const auto& c = coefficients;
            const double out = c[0] * in + z1;
            z1 = c[1] * in - c[3] * out + z2;
            z2 = c[2] * in - c[4] * out;
            return out;
        }
    };

    struct Bin
    {
        double energy = 0;
        std::uint64_t count = 0;
    };

    double weightedEnergy(int channel, double sample)
    {
        const auto weighted = channels[channel][1].process(channels[channel][0].process(sample));
        return weighted * weighted;
    }

    static float loudness(double energy)
    {
        return energy > 1e-12 ? static_cast<float>(-0.691 + 10 * std::log10(energy)) : -120;
    }

    static std::size_t binIndex(float level)
    {
        return static_cast<std::size_t>(std::clamp(static_cast<int>((level + 70) * 10), 0, 1000));
    }

    int samplesPerStep = 4800, samples = 0, steps = 0, cursor = 0;
    double sum = 0, totalEnergy = 0;
    std::uint64_t totalBlocks = 0;
    std::array<std::array<Filter, 2>, 2> channels {};
    std::array<double, 30> history {};
    std::array<Bin, 1001> histogram {};
    LoudnessLevels levels;
};
}
