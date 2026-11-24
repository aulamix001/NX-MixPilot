#pragma once

#include <algorithm>
#include <cmath>

namespace nx
{
class ContinuousGain
{
public:
    void prepare(double rate)
    {
        const auto interval = std::max(1, static_cast<int>(std::round(rate * 0.01)));
        period = interval;
        secondsPerStep = interval / rate;
        rmsBlend = 1.0 - std::exp(-secondsPerStep / 0.4);
        peakDecay = std::exp(-secondsPerStep / 0.35);
        rampBlend = 1.0 - std::exp(-1.0 / (rate * 0.015));
        limiterBlend = 1.0 - std::exp(-1.0 / (rate * 0.10));
        reset();
    }

    void reset()
    {
        count = 0;
        squareSum = blockPeak = retainedPeak = energy = correctionDb = 0;
        targetGain = actualGain = limiterGain = 1;
    }

    void configure(bool automatic, bool freeze, double reference, double maximum,
                   double margin, bool safety, double ceiling)
    {
        if (automatic && !enabled)
        {
            count = 0;
            squareSum = blockPeak = retainedPeak = energy = 0;
        }
        enabled = automatic;
        frozen = freeze;
        referenceDb = reference;
        limitDb = maximum;
        headroomDb = margin;
        limiterEnabled = safety;
        ceilingGain = std::pow(10.0, ceiling / 20.0);
        if (!enabled)
        {
            correctionDb = 0;
            targetGain = 1;
        }
    }

    void process(double& left, double& right)
    {
        if (!enabled && !limiterEnabled && actualGain == 1 && limiterGain == 1)
            return;

        if (enabled && !frozen)
        {
            const auto l = std::clamp(left, -1.0e6, 1.0e6);
            const auto r = std::clamp(right, -1.0e6, 1.0e6);
            squareSum += (l * l + r * r) * 0.5;
            blockPeak = std::max({blockPeak, std::abs(l), std::abs(r)});
            if (++count == period)
                update();
        }
        actualGain += (targetGain - actualGain) * rampBlend;
        if (!enabled && std::abs(actualGain - 1) < 1e-7)
            actualGain = 1;

        left *= actualGain;
        right *= actualGain;
        const auto peak = std::max(std::abs(left), std::abs(right));
        const auto allowed = limiterEnabled && peak > ceilingGain ? ceilingGain / peak : 1.0;
        limiterGain = std::min(allowed, limiterGain + (1 - limiterGain) * limiterBlend);
        if (!limiterEnabled && 1 - limiterGain < 1e-7)
            limiterGain = 1;
        left *= limiterGain;
        right *= limiterGain;
    }

    double gainDb() const { return 20 * std::log10(actualGain); }
    double reductionDb() const { return -20 * std::log10(std::max(1e-12, limiterGain)); }

private:
    void update()
    {
        const auto mean = squareSum / count;
        energy += (mean - energy) * rmsBlend;
        retainedPeak = std::max(blockPeak, retainedPeak * peakDecay);
        // Silence does not pull the target up. The retained peak also restrains
        // gain boosts between hits while the RMS envelope is still settling.
        if (mean > 1e-6 && energy > 1e-6)
        {
            const auto desired = referenceDb - 10.0 * std::log10(energy);
            const auto safe = -headroomDb - 20.0 * std::log10(retainedPeak);
            const auto next = std::clamp(std::min(desired, safe), -limitDb, limitDb);
            correctionDb += std::clamp(next - correctionDb, -18 * secondsPerStep, 3 * secondsPerStep);
            targetGain = std::pow(10.0, correctionDb / 20);
        }
        count = 0;
        squareSum = blockPeak = 0;
    }

    int period = 480, count = 0;
    double secondsPerStep = 0.01, rmsBlend = 0, peakDecay = 0, rampBlend = 0, limiterBlend = 0;
    double squareSum = 0, blockPeak = 0, retainedPeak = 0, energy = 0;
    double referenceDb = -18, limitDb = 6, headroomDb = 3, ceilingGain = 1;
    double correctionDb = 0, targetGain = 1, actualGain = 1, limiterGain = 1;
    bool enabled = false, frozen = false, limiterEnabled = false;
};
}
