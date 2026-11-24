#include "TonalDSP.h"
#include "SmartGain.h"
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
    double budget = 0;
    for (const auto& band : proposed)
    {
        budget += std::abs(band.gain);
        require(band.gain <= 0 || band.frequency < 2000, "Matching boosted a missing frequency region.");
    }
    require(budget <= 3.00001, "Combined automatic correction exceeded the limit.");
    for (const auto& band : nx::match(single, different, 0, 3))
        require(band.gain == 0, "Intensity zero produced a correction.");

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
    const auto bus = nx::planGlobalHeadroom(-2, -1.4f, -6, -1, 1);
    require(bus.available && near(bus.suggested, -3, 0.01), "Global headroom must use the measured bus.");
    require(nx::planGlobalHeadroom(-20, -19, -6, -1, 0).suggested == 0, "Headroom manager must not raise a quiet mix.");

    std::array<nx::TonalEqualizer, 48> instances;
    for (auto& instance : instances)
    {
        instance.prepare(96000);
        instance.update(request);
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
    std::cout << "FFT 4096/8192/16384, response, THD, phase, compensation, limits and masking passed. "
              << "48 EQ instances / 100 ms of audio: " << elapsed << " ms of runtime.\n";
}
