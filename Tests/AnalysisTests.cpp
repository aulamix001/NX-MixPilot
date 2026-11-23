#include "Analysis.h"
#include <cstdlib>
#include <iostream>

namespace
{
void expect(bool predicate, const char* message)
{
    if (!predicate)
    {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(float a, float b, float tolerance = 0.02f)
{
    return std::abs(a - b) <= tolerance;
}

nx::Reading tone(float hz, float rmsDb, bool inverted = false)
{
    nx::Analyzer analyzer;
    constexpr int sampleRate = 48000;
    analyzer.prepare(sampleRate);
    nx::Reading result;
    const float amplitude = std::sqrt(2.0f) * std::pow(10.0f, rmsDb / 20.0f);
    for (int i = 0; i < nx::fftSize * 48; ++i)
    {
        const float left = amplitude * std::sin(2 * nx::pi * hz * i / sampleRate);
        const float right = inverted ? -left : left;
        analyzer.push(left, right, left, right, i, result);
    }
    return result;
}
}

int main()
{
    const auto measured = tone(1000, -18, true);
    expect(measured.active() && near(measured.rms, -18), "A tone calibrated to -18 dBFS RMS must read -18.");
    expect(near(measured.peak, -14.9897f), "Sine sample peak must be about 3.01 dB above RMS.");
    expect(near(measured.outputPeak, measured.peak), "Unity gain must preserve the peak.");
    const auto nominal = nx::suggestTrim(measured, nx::GainMode::calibration, nx::nominalRms, nx::automatic, 6, 3);
    expect(near(nominal.trim, 0), "A calibrated tone needs no correction.");

    const auto custom = nx::suggestTrim(measured, nx::GainMode::calibration, -21, nx::focus, 6, 3);
    expect(near(custom.trim, -3), "A custom reference must change the technical trim.");
    const auto quiet = tone(1000, -24);
    const auto technical = nx::suggestTrim(quiet, nx::GainMode::calibration, -18, nx::automatic, 12, 3);
    const auto musical = nx::suggestTrim(quiet, nx::GainMode::musical, -18, nx::music, 12, 3);
    expect(near(technical.trim, 6) && near(musical.trim, 1), "Musical balance must retain the role offset.");
    auto profile = [&](nx::Role role)
    {
        return nx::suggestTrim(quiet, nx::GainMode::musical, -18, role, 12, 3).trim;
    };
    expect(near(profile(nx::backingVocal), 0) && near(profile(nx::harmony), -1)
        && near(profile(nx::auxiliaryPercussion), -3) && near(profile(nx::ambience), -6),
        "Support roles must preserve the intended relative hierarchy.");
    expect(near(nx::suggestTrim(quiet, nx::GainMode::calibration, -18, nx::ambience, 12, 3).trim, 6),
        "Musical roles must not affect the technical calibration reference.");
    auto peakCheck = quiet;
    peakCheck.peak = -21;
    expect(near(nx::suggestTrim(peakCheck, nx::GainMode::calibration, -18, nx::music, 20, 3, 1).trim, 11),
           "Kick preset must bring expected sample peak into the supplied range.");
    expect(near(nx::suggestTrim(peakCheck, nx::GainMode::musical, -24, nx::focus, 20, 3, 9).trim, 0),
           "A main vocal selected as Principal must retain its musical anchor.");
    expect(near(nx::suggestTrim(peakCheck, nx::GainMode::calibration, -18, nx::music, 5, 3, 1).trim, 5),
           "The suggested peak range must not override the trim limit.");
    peakCheck.peak = -8;
    expect(near(nx::suggestTrim(peakCheck, nx::GainMode::calibration, -18, nx::music, 12, 3, 5).trim, -6),
           "A piano peaking at -8 must get -6 dB trim to reach -14 dBFS.");
    peakCheck.peak = -16;
    expect(near(nx::suggestTrim(peakCheck, nx::GainMode::calibration, -18, nx::bass, 12, 3, 4).trim, 6),
           "A bass peaking at -16 must get +6 dB trim to reach -10 dBFS.");
    expect(nx::bassHeavy(tone(94, -24)) && !nx::bassHeavy(measured), "Auto role must distinguish dominant lows.");

    auto transient = quiet;
    transient.peak = -1;
    const auto capped = nx::suggestTrim(transient, nx::GainMode::calibration, -18, nx::automatic, 6, 3);
    expect(near(capped.trim, -2) && capped.headroomLimited && !capped.headroomUnmet,
           "Sample headroom must take priority over nominal RMS on transient-rich audio.");
    const auto impossible = nx::suggestTrim(transient, nx::GainMode::calibration, -18, nx::automatic, 1, 3);
    expect(impossible.headroomUnmet, "A trim range too small to honor headroom must be flagged.");
    expect(!nx::Reading{}.active(), "An empty reading must never enable calibration.");

    nx::GroupCalibration group(6, 3);
    group.include(quiet, 2);
    group.include(measured, -2);
    expect(group.possible() && near(group.step(-18), 2), "A group uses the loudest post-trim RMS.");
    const float before = (quiet.rms + 2) - (measured.rms - 2);
    const float step = group.step(-18);
    const float after = (quiet.rms + 2 + step) - (measured.rms - 2 + step);
    expect(near(before, after), "Common gain must leave relative levels intact.");
    group.include(transient, 0);
    expect(group.possible() && near(group.step(-18), -2), "A group must respect the largest sample peak.");

    nx::GroupCalibration unsafeGroup(1, 3);
    unsafeGroup.include(transient, 0);
    expect(!unsafeGroup.possible(), "A group with insufficient adjustment range must be rejected.");

    const auto overlap = nx::findOverlap(measured, tone(1000, -18));
    expect(overlap.band >= 0 && nx::frequency(overlap.band) <= 1000 && nx::frequency(overlap.band + 1) >= 1000,
           "Aligned overlapping tones must identify the correct band, even with inverted stereo phase.");
    expect(nx::findOverlap(measured, tone(4200, -18)).band == -1, "Separated tones must not report overlap.");
    auto shifted = measured;
    shifted.position += nx::fftSize;
    expect(nx::findOverlap(measured, shifted).band == -1, "Unaligned measurements must not report overlap.");
    std::cout << "RMS, sample peak, nominal and custom calibration, group gain, musical roles and spectral alignment passed.\n";
}
