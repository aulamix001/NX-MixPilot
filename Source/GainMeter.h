#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace nx
{
struct StereoLevels
{
    std::array<float, 2> peak {}, truePeak {}, rms {}, vu {}, hold {};
};

class GainMeter
{
public:
    void prepare(double rate)
    {
        *this = {};
        rmsCoefficient = 1.0 - std::exp(-1.0 / (rate * 0.30));
        // Two cascaded stages reach 99% of a step in approximately 300 ms.
        vuCoefficient = 1.0 - std::exp(-6.63835 / (rate * 0.30));
        for (int phase = 0; phase < 3; ++phase)
        {
            double sum = 0;
            for (int i = 0; i < taps; ++i)
            {
                const double x = i - (taps / 2 - 1) - (phase + 1) * 0.25;
                constexpr auto pi = 3.14159265358979323846;
                const double window = 0.42 - 0.5 * std::cos(2 * pi * i / (taps - 1))
                    + 0.08 * std::cos(4 * pi * i / (taps - 1));
                kernel[phase][i] = window * std::sin(pi * x) / (pi * x);
                sum += kernel[phase][i];
            }
            for (auto& coefficient : kernel[phase])
                coefficient /= sum;
        }
    }

    void push(double left, double right)
    {
        const std::array<double, 2> samples {left, right};
        for (int c = 0; c < 2; ++c)
        {
            auto& channel = channels[c];
            const auto value = samples[c];
            channel.buffer[cursor] = channel.buffer[cursor + taps] = value;
            const auto absolute = std::abs(value);
            double reconstructed = absolute;
            for (const auto& phase : kernel)
            {
                double interpolated = 0;
                for (int i = 0; i < taps; ++i)
                    interpolated += channel.buffer[cursor + 1 + i] * phase[i];
                reconstructed = std::max(reconstructed, std::abs(interpolated));
            }
            channel.peak = std::max(channel.peak, absolute);
            channel.truePeak = std::max(channel.truePeak, reconstructed);
            channel.hold = std::max(channel.hold, reconstructed);
            channel.rms += rmsCoefficient * (value * value - channel.rms);
            channel.vu1 += vuCoefficient * (absolute - channel.vu1);
            channel.vu2 += vuCoefficient * (channel.vu1 - channel.vu2);
        }
        if (++cursor == taps)
            cursor = 0;
    }

    StereoLevels read()
    {
        StereoLevels output;
        for (int c = 0; c < 2; ++c)
        {
            auto& channel = channels[c];
            output.peak[c] = static_cast<float>(channel.peak);
            output.truePeak[c] = static_cast<float>(channel.truePeak);
            output.rms[c] = static_cast<float>(std::sqrt(channel.rms));
            output.vu[c] = static_cast<float>(channel.vu2 * 1.11072073454); // Sine-equivalent RMS.
            output.hold[c] = static_cast<float>(channel.hold);
            channel.peak = channel.truePeak = 0;
        }
        return output;
    }

    void resetHold()
    {
        for (auto& channel : channels)
            channel.hold = 0;
    }

private:
    static constexpr int taps = 24;
    struct Channel
    {
        std::array<double, taps * 2> buffer {};
        double peak = 0, truePeak = 0, hold = 0, rms = 0, vu1 = 0, vu2 = 0;
    };
    std::array<Channel, 2> channels {};
    std::array<std::array<double, taps>, 3> kernel {};
    double rmsCoefficient = 0, vuCoefficient = 0;
    int cursor = 0;
};
}
