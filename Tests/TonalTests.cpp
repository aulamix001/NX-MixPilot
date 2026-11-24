#include "TonalDSP.h"
#include "SmartGain.h"
#include "InstrumentPresets.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

void verifyFFT(int n)
{
    std::vector<std::complex<double>> wave(static_cast<std::size_t>(n));
    std::vector<std::complex<double>> twiddles(static_cast<std::size_t>(n / 2));
    for (int i = 0; i < n / 2; ++i)
        twiddles[i] = std::polar(1.0, -2 * nx::tonalPi * i / n);
    constexpr int tone = 173;
    for (int i = 0; i < n; ++i)
        wave[i] = std::sin(2 * nx::tonalPi * tone * i / n);
    nx::fft(wave.data(), twiddles.data(), n);
    require(near(std::abs(wave[tone]), n / 2.0, 1.0e-7), "FFT amplitude does not match a bin-centered sine.");
    require(near(wave[tone].real(), 0, 1.0e-7) && near(wave[tone].imag(), -n / 2.0, 1.0e-7),
            "FFT phase is incorrect.");
    require(std::abs(wave[tone + 1]) < 1.0e-7, "FFT introduced energy in the neighboring bin.");
}
}

int main()
{
    for (const auto size : {4096, 8192, 16384})
        verifyFFT(size);

    constexpr double rate = 48000;
    nx::BellFilter filter;
    filter.prepare({1000, -3, 1}, rate);
    require(near(nx::powerDb(filter.response(1000, rate)), -3, 0.0001), "Bell center gain is inaccurate.");
    require(near(nx::powerDb(filter.response(30, rate)), 0, 0.02), "Bell unexpectedly affects far frequencies.");
    auto sine = [](int i) { return 0.2 * std::sin(2 * nx::tonalPi * i / 48.0); };
    filter.prepare({1000, -3, 1}, rate);
    std::array<std::complex<double>, 8> projected {};
    for (int i = 0; i < 9600; ++i)
    {
        const double out = filter.process(sine(i));
        if (i >= 4800)
            for (std::size_t h = 0; h < projected.size(); ++h)
                projected[h] += out * std::polar(1.0, -2 * nx::tonalPi * (h + 1) * i / 48.0);
    }
    require(near(std::abs(projected[0]) / 480.0, std::pow(10.0, -3.0 / 20.0), 1.0e-7),
            "Time-domain center response disagrees with the expected gain.");
    double harmonicPower = 0;
    for (std::size_t h = 1; h < projected.size(); ++h)
        harmonicPower += std::norm(projected[h]);
    require(std::sqrt(harmonicPower / std::norm(projected[0])) < 1.0e-8,
            "Steady-state filter THD exceeded the test threshold.");

    for (const double sampleRate : {44100, 48000, 88200, 96000, 192000})
    {
        nx::BellFilter tested;
        const nx::Bell setting {880, 4, 1.4};
        tested.prepare(setting, sampleRate);
        require(near(nx::powerDb(tested.response(setting.frequency, sampleRate)), setting.gain, 1e-7),
               "EQ response must remain accurate at every supported validation rate.");
        double residual = 0;
        for (int i = 0; i < static_cast<int>(sampleRate / 2); ++i)
        {
            const auto output = tested.process(i == 0 ? 1.0 : 0.0);
            require(std::isfinite(output), "EQ impulse response must remain stable.");
            if (i > sampleRate / 4)
                residual = std::max(residual, std::abs(output));
        }
        require(residual < 1e-10, "Impulse left an unexpected residual tail.");
        tested.prepare(setting, sampleRate);
        const auto cycle = static_cast<int>(sampleRate / 100);
        double signalPower = 0, noisePower = 0, ss = 0, cc = 0;
        for (int i = 0; i < cycle * 20; ++i)
        {
            const auto angle = 2 * nx::tonalPi * 100 * i / sampleRate;
            const auto output = tested.process(0.2 * std::sin(angle));
            if (i >= cycle * 10)
            {
                ss += output * std::sin(angle);
                cc += output * std::cos(angle);
                signalPower += output * output;
            }
        }
        const auto fundamental = 2 * (ss * ss + cc * cc) / (cycle * 10);
        noisePower = std::max(0.0, signalPower - fundamental);
        require(std::sqrt(noisePower / fundamental) < 1e-6, "Steady-state EQ THD+N exceeded -120 dB.");
    }

    nx::TonalEqualizer eq;
    eq.prepare(rate);
    double l = 0.5, r = -0.5;
    eq.process(l, r);
    require(l == 0.5 && r == -0.5, "Bypass must pass the same sample without latency or stereo change.");
    nx::EqualizerSettings request;
    request.bands[0] = {1000, 3, 1};
    eq.update(request);
    double previous = 0;
    for (int i = 0; i < 3000; ++i)
    {
        l = sine(i);
        r = -l;
        eq.process(l, r);
        require(std::abs(l + r) < 1.0e-12, "Identical stereo processing changed interchannel phase.");
        require(std::abs(l - previous) < 0.042, "EQ transition introduced a discontinuity.");
        previous = l;
    }

    nx::Spectrum single {};
    const auto bin = nx::spectralBand(1000);
    single[bin] = 0.01f;
    const auto compensation = nx::estimateCompensation(single, request.bands, rate);
    require(near(compensation, -3, 0.08), "Compensation disagrees with predicted power gain.");
    const auto target = nx::adaptiveTarget(single, 6, true, nx::profiles[1]);
    require(target[bin] > target.front() && nx::spectrumEnergy(target) > 0, "Target must retain instrument shape.");
    nx::Spectrum different = single;
    different[nx::spectralBand(11000)] = 0.7f;
    const auto proposed = nx::match(single, different, 0.35, 3.0);
    for (const auto& band : proposed)
        require(band.gain <= 0 || band.frequency < 2000, "Matching boosted a missing frequency region.");
    for (const auto& band : nx::match(single, different, 0, 3))
        require(band.gain == 0, "Intensity zero produced a correction.");

    nx::Spectrum broadband {}, knownReference {};
    nx::Bells known {};
    constexpr std::array<double, 5> gains {3.0, -2.5, 2.8, -2.8, 2.2};
    for (int i = 0; i < 5; ++i)
        known[static_cast<std::size_t>(i)] = {nx::bandFrequency(12 + i * 15), gains[static_cast<std::size_t>(i)], 1.1 + 0.3 * i};
    for (int i = 0; i < nx::tonalBands; ++i)
    {
        broadband[i] = 0.0003f;
        knownReference[i] = static_cast<float>(broadband[i] * 5
            * std::pow(10.0, nx::eqResponse(known, nx::bandFrequency(i), rate) / 10.0));
    }
    const auto fit = nx::match(broadband, knownReference, 1, 4, 0.15, 0, rate);
    const auto quarter = nx::match(broadband, knownReference, 0.25, 4, 0.15, 0, rate);
    const auto half = nx::match(broadband, knownReference, 0.5, 4, 0.15, 0, rate);
    const auto difference = nx::tonalDifference(broadband, knownReference, 0, rate);
    const auto beforeError = difference.error({}, rate);
    const auto afterError = difference.error(fit, rate);
    require(nx::activeBands(fit) >= 4 && afterError < beforeError * 0.42,
            "Multiband fitting did not recover the known reference.");
    require(difference.error(quarter, rate) > difference.error(half, rate)
        && difference.error(half, rate) > afterError, "Error should improve monotonically at 25, 50 and 100% intensity.");
    for (std::size_t i = 0; i < fit.size(); ++i)
        require(near(quarter[i].gain, fit[i].gain * 0.25, 1e-6), "Intensity must scale the same correction.");
    for (int i = 0; i <= 4096; ++i)
    {
        const auto hz = 20 * std::pow(1000.0, i / 4096.0);
        require(std::abs(nx::eqResponse(fit, hz, rate)) <= 4, "Combined EQ exceeded its cap on a dense sweep.");
    }
    const auto narrow = nx::match(broadband, knownReference, 1, 4, 0.15, 0, rate, 120, 5000);
    for (const auto& band : narrow)
        require(band.gain == 0 || (band.frequency >= 120 && band.frequency <= 5000), "EQ ignored the selected range.");
    auto identical = broadband;
    for (auto& power : identical)
        power *= 10;
    require(nx::activeBands(nx::match(broadband, identical, 1, 6)) == 0,
            "A level difference on an identical spectrum must not generate EQ.");
    const auto fittedCompensation = nx::estimateCompensation(broadband, fit, rate);
    double outputEnergy = 0;
    for (int i = 0; i < nx::tonalBands; ++i)
        outputEnergy += broadband[i] * std::pow(10.0,
            (nx::eqResponse(fit, nx::bandFrequency(i), rate) + fittedCompensation) / 10);
    require(near(nx::powerDb(outputEnergy / nx::spectrumEnergy(broadband)), 0, 0.01),
            "Matched broadband compensation exceeded its error tolerance.");
    std::cout << "Known spectral differences: " << beforeError << " -> " << afterError
              << " dB weighted RMS using " << nx::activeBands(fit) << " bands.\n";

    const auto samePan = nx::masking(single, single, 0.1, 0.15);
    const auto oppositePan = nx::masking(single, single, -1, 1);
    require(samePan.band == bin && samePan.score > oppositePan.score, "Masking score must consider panorama.");
    require(nx::masking(single, {}, 0, 0).band < 0, "Silence cannot mask another track.");

    nx::SmartGainInput input;
    input.signal.rms = -21;
    input.signal.peak = -12;
    input.signal.settled = true;
    input.target = -6;
    input.limit = 12;
    const auto firstPass = nx::smartGain(input);
    require(near(firstPass.suggestion.trim, 6, 0.01), "Smart gain must work from original signal.");
    auto vocal = input;
    vocal.category = 6;
    vocal.role = nx::focus;
    vocal.target = -10;
    vocal.instrumentCeiling = -6;
    vocal.signal.rms = -21.6f;
    vocal.signal.peak = -7.1f;
    vocal.trim = -4.4f;
    vocal.limit = 4.4f;
    require(near(nx::smartGain(vocal).suggestion.trim, -2.9f, 0.001),
            "Explicit technical peak calibration must refine a previously applied vocal trim within the limit.");
    vocal.signal.peak = -25;
    const auto quietVocal = nx::smartGain(vocal);
    require(quietVocal.trimLimited && near(quietVocal.suggestion.trim, 4.4f, 0.001),
            "The technical calibration must report when maximum trim prevents a full correction.");
    input.trim = firstPass.suggestion.trim;
    require(nx::smartGain(input).suggestion.trim == input.trim, "Reanalysis must not accumulate or reapply trim.");
    input.strategy = nx::GainStrategy::preserve;
    input.trim = 3;
    require(nx::smartGain(input).suggestion.trim == 3, "Preserve Mix must keep safe trim.");
    input.ceiling = -1;
    require(nx::smartGain(input).suggestion.trim < 0, "True Peak safety must override preservation.");
    input.strategy = nx::GainStrategy::hybrid;
    input.signal.settled = false;
    require(!nx::smartGain(input).sufficient && nx::smartGain(input).suggestion.trim == input.trim,
            "Hybrid must abstain if activity is insufficient.");
    input.signal.settled = true;
    input.strategy = nx::GainStrategy::musical;
    input.signal.rms = -46;
    input.signal.peak = -39;
    input.anchor = -19;
    input.ceiling = -39;
    input.role = nx::music;
    input.target = 1;
    input.trim = 6;
    require(nx::smartGain(input).suggestion.trim == 0, "Quiet guitar must lose an old boost instead of retaining it.");
    input.trim = -9;
    input.limit = 6;
    require(nx::smartGain(input).suggestion.trim == -9, "Musical balance must retain stronger prior attenuation.");
    input.signal.rms = -18;
    input.trim = 0;
    require(nx::smartGain(input).suggestion.trim == -6, "An audible accompaniment must be attenuated relative to the focus.");
    input.role = nx::focus;
    input.trim = -4.7f;
    require(nx::smartGain(input).suggestion.trim == input.trim, "Musical balance must preserve a safe focus trim.");
    input.ceiling = 3;
    require(nx::smartGain(input).suggestion.trim == -6, "Focus still needs reduction if its measured peak requires headroom.");
    input.signal.learnedActiveWindows = 20;
    input.signal.learnedEnergy = -24;
    input.signal.learnedLoudEnergy = -13;
    require(nx::musicalRms(input.signal, true, true, -2) == -26
        && nx::musicalRms(input.signal, false, true, 1) == -12,
        "Musical analysis must distinguish the anchor median and louder accompaniment windows, including output gain.");

    input.role = nx::music;
    input.signal.rms = -46;
    input.signal.peak = -39;
    input.trim = 6;
    input.limit = 24;
    input.ceiling = -11;
    input.instrumentCeiling = -10;
    input.maxSample = -6;
    input.downstreamGain = 2;
    require(nx::smartGain(input).suggestion.trim == -6,
            "The highest learned sample peak and gain after trim must count toward the instrument ceiling.");
    input.limit = 3;
    require(nx::smartGain(input).suggestion.headroomUnmet,
            "A preset that cannot reach its ceiling inside the trim limit must flag insufficient range.");
    require(nx::instrumentPresetCeiling(nx::presets[0]) == -6 &&
            nx::instrumentPresetCeiling({"Guitarra clean", 3, -14}) == -10,
            "Instrument ceilings must use the project's starter ranges.");

    nx::SmartGainInput styled;
    styled.signal.rms = -20;
    styled.signal.peak = -14;
    styled.signal.settled = true;
    styled.anchor = -20;
    styled.limit = 12;
    styled.strategy = nx::GainStrategy::musical;
    styled.role = nx::music;
    styled.styleLane = nx::StyleLane::harmony;
    require(near(nx::smartGain(styled).suggestion.trim, -5, 0.001), "Universal must retain the established guitar balance.");
    styled.style = nx::GainStyle::worship;
    require(near(nx::smartGain(styled).suggestion.trim, -6.5f, 0.001), "Worship should reserve more space for the focus.");
    styled.style = nx::GainStyle::popRock;
    require(near(nx::smartGain(styled).suggestion.trim, -4.25f, 0.001), "Pop Rock should retain more harmonic level.");
    styled.role = nx::focus;
    styled.trim = -2;
    require(nx::smartGain(styled).suggestion.trim == -2, "Genre presets must preserve a safe focus trim.");
    styled.role = nx::music;
    styled.trim = 0;
    styled.strategy = nx::GainStrategy::technical;
    styled.signal.peak = -24;
    styled.target = -14;
    styled.stylePresetTarget = true;
    styled.style = nx::GainStyle::worship;
    require(near(nx::smartGain(styled).suggestion.trim, 9.25f, 0.001), "Style should mildly steer a factory peak target.");
    styled.stylePresetTarget = false;
    require(near(nx::smartGain(styled).suggestion.trim, 10, 0.001), "Style must never rewrite a manual peak target.");
    styled.instrumentCeiling = -13;
    styled.maxSample = -8;
    require(near(nx::smartGain(styled).suggestion.trim, -5, 0.001), "Genre style must never weaken the measured peak ceiling.");
    styled.strategy = nx::GainStrategy::preserve;
    styled.instrumentCeiling = 1;
    styled.style = nx::GainStyle::gospel;
    styled.trim = -1;
    require(nx::smartGain(styled).suggestion.trim == -1, "Preserve Mix must not rebalance the user mix by style.");

    for (const auto& preset : nx::presets)
    {
        if (preset.category == 7)
            continue;
        nx::SmartGainInput capture;
        capture.category = preset.category;
        capture.target = preset.target;
        capture.instrumentCeiling = nx::instrumentPresetCeiling(preset);
        capture.signal.peak = capture.maxSample = capture.ceiling = preset.target - 5;
        require(!nx::smartGain(capture).sufficient, "A current silent window alone must not qualify for calibration.");
        capture.capturedPeak = true;
        require(near(nx::smartGain(capture).suggestion.trim, 5, 0.001),
                "Every instrument and vocal preset must raise a quiet learned peak to its target.");
        capture.signal.peak = capture.maxSample = capture.ceiling = preset.target + 5;
        require(near(nx::smartGain(capture).suggestion.trim, -5, 0.001),
                "Every instrument and vocal preset must attenuate a high learned peak.");
        capture.maxSample = capture.instrumentCeiling + 8;
        require(nx::smartGain(capture).suggestion.headroomUnmet,
                "A captured peak above the ceiling must still be rejected if available trim cannot protect it.");
    }

    const auto bus = nx::planGlobalHeadroom(-2, -1.4f, -6, -1, 1);
    require(bus.available && near(bus.suggested, -3, 0.01), "Global headroom must use the measured bus.");
    require(nx::planGlobalHeadroom(-20, -19, -6, -1, 0).suggested == 0, "Headroom manager must not raise a quiet mix.");

    for (const int instancesCount : {16, 32, 64, 128})
    {
        std::vector<nx::TonalEqualizer> instances(static_cast<std::size_t>(instancesCount));
        nx::EqualizerSettings worstCase;
        for (std::size_t i = 0; i < worstCase.bands.size(); ++i)
            worstCase.bands[i] = {120.0 * (i + 1), i % 2 ? -2.0 : 2.0, 1.1};
        for (auto& instance : instances)
        {
            instance.prepare(96000);
            instance.update(worstCase);
        }
        const auto start = std::chrono::steady_clock::now();
        double checksum = 0;
        for (int i = 0; i < 9600; ++i)
            for (auto& instance : instances)
            {
                l = sine(i);
                r = l * 0.5;
                instance.process(l, r);
                checksum += l * l + r * r;
            }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(std::isfinite(checksum) && checksum > 0, "Multi-instance run produced invalid samples.");
        std::cout << instancesCount << " x 12-band EQ instances / 100 ms audio: " << elapsed << " ms runtime.\n";
    }
    std::cout << "FFT 4096/8192/16384, response, THD, phase, compensation, limits and masking passed.\n";
}
