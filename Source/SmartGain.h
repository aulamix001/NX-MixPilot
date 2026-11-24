#pragma once

#include "Analysis.h"

namespace nx
{
enum class GainStrategy { technical, musical, preserve, hybrid };
enum class Function { automatic, lead, rhythm, harmony, melody, ambience };
enum class SoundShape { transient, sustained, hybrid };

inline SoundShape soundShape(int category, float crestDb)
{
    if (category == 0 || crestDb >= 17)
        return SoundShape::transient;
    if (category == 1 || category == 2 || crestDb <= 8)
        return SoundShape::sustained;
    return SoundShape::hybrid;
}

inline float tolerance(SoundShape shape)
{
    return shape == SoundShape::transient ? 2.5f : shape == SoundShape::sustained ? 1.0f : 1.75f;
}

struct SmartGainInput
{
    Reading signal;
    GainStrategy strategy = GainStrategy::technical;
    Role role = automatic;
    Function function = Function::automatic;
    int category = 2, priority = 50, legacyPeakProfile = 0;
    float trim = 0, target = 1, anchor = nominalRms, limit = 6, margin = 3, ceiling = floorDb;
};

struct SmartGainResult
{
    GainSuggestion suggestion;
    float wanted = 0, permitted = 0, crest = 0;
    bool sufficient = false;
};

inline SmartGainResult smartGain(const SmartGainInput& input)
{
    SmartGainResult result;
    const auto& signal = input.signal;
    result.suggestion.trim = result.wanted = input.trim;
    result.permitted = -input.margin - std::max(signal.peak, input.ceiling);
    result.crest = std::max(0.0f, signal.peak - signal.rms);
    result.sufficient = signal.active();
    if (!result.sufficient)
        return result;

    const bool anchorRole = input.role == focus;
    auto role = input.role;
    if (role == automatic)
    {
        if (input.function == Function::rhythm)
            role = drums;
        else if (input.function == Function::harmony)
            role = harmony;
        else if (input.function == Function::ambience)
            role = ambience;
        else if (input.function == Function::melody || input.function == Function::lead)
            role = music;
    }

    const auto peakShape = soundShape(input.category, result.crest);
    float desired = input.trim;
    switch (input.strategy)
    {
        case GainStrategy::technical:
            desired = suggestTrim(signal, GainMode::calibration, input.anchor, role, 90, -90,
                                  input.legacyPeakProfile, input.target).trim;
            if (std::abs(desired - input.trim) <= tolerance(peakShape))
                desired = input.trim;
            break;
        case GainStrategy::musical:
        case GainStrategy::hybrid:
        {
            const auto musical = suggestTrim(signal, GainMode::musical, input.anchor, role, 90, -90,
                                             input.legacyPeakProfile, input.target).trim;
            desired = musical;
            if (input.strategy == GainStrategy::hybrid && !anchorRole)
            {
                const auto technical = suggestTrim(signal, GainMode::calibration, nominalRms, role, 90, -90,
                                                    input.legacyPeakProfile, input.target).trim;
                const float qualifier = peakShape == SoundShape::transient ? 0.25f : 0.65f;
                const float absoluteCandidate = technical + qualifier * (musical - technical);
                // Priority weighs the musical candidate against the technical one.
                // Both are absolute proposals measured before trim, so reanalysis does not accumulate gain.
                const float musicalWeight = static_cast<float>(std::clamp(input.priority, 0, 100)) / 100.0f;
                desired = absoluteCandidate + musicalWeight * (musical - absoluteCandidate);
                if (std::abs(desired - input.trim) <= tolerance(peakShape))
                    desired = input.trim;
            }
            break;
        }
        case GainStrategy::preserve:
            break;
    }

    result.wanted = desired;
    const auto limited = std::max(-input.limit, std::min({desired, input.limit, result.permitted}));
    result.suggestion = {limited, desired > result.permitted, limited > result.permitted + 0.001f};
    return result;
}

struct GlobalHeadroom
{
    float suggested = 0, sample = floorDb, truePeak = floorDb;
    bool available = false;
};

inline GlobalHeadroom planGlobalHeadroom(float sample, float truePeak, float sampleGoal,
                                         float trueCeiling, float current)
{
    if (sample < -90 || truePeak < -90)
        return {};
    const auto delta = std::min({0.0f, sampleGoal - sample, trueCeiling - truePeak});
    return {std::clamp(current + delta, -24.0f, 24.0f), sample, truePeak, true};
}
}
