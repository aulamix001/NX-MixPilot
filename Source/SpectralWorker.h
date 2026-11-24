#pragma once

#include "TonalDSP.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace nx
{
struct SpectrumFrame
{
    Spectrum before {}, after {}, instant {}, longBefore {}, recurrence {};
    Spectrum dominantHz {}, phase {};
    unsigned frames = 0;
    int resonance = -1;
    std::int64_t end = -1;
    int rate = 0, session = 0;
    double pan = 0;
    juce::uint32 updated = 0;
};

class SpectralWorker final : private juce::TimeSliceClient
{
public:
    SpectralWorker()
    {
        for (int i = 0; i < maximum / 2; ++i)
            rotations[i] = std::polar(1.0, -2 * tonalPi * i / maximum);
        worker().addTimeSliceClient(this);
    }

    ~SpectralWorker() override { worker().removeTimeSliceClient(this); }

    struct Sample
    {
        float left, right, outputLeft, outputRight;
        std::int64_t position;
        int rate, session;
    };

    void push(Sample sample)
    {
        const unsigned write = cursorWrite.load(std::memory_order_relaxed);
        if (write - cachedRead == queueSize)
        {
            cachedRead = cursorRead.load(std::memory_order_acquire);
            if (write - cachedRead == queueSize)
            {
                ++drops;
                return;
            }
        }
        queue[write & (queueSize - 1)] = sample;
        cursorWrite.store(write + 1, std::memory_order_release);
    }

    void configure(int fftIndex, int averagingMs)
    {
        configuration.store((std::clamp(averagingMs, 0, 4000) << 2) | std::clamp(fftIndex, 0, 2),
                            std::memory_order_relaxed);
    }

    bool read(SpectrumFrame& result, int session) const
    {
        std::lock_guard<std::mutex> guard(frameLock);
        if (latest.session != session || latest.rate == 0
            || juce::Time::getMillisecondCounter() - latest.updated >= 1100)
            return false;
        result = latest;
        return true;
    }

private:
    static constexpr int maximum = 16384;
    static constexpr unsigned queueSize = 32768;
    using Complex = std::complex<double>;

    static juce::TimeSliceThread& worker()
    {
        struct Worker
        {
            Worker() { thread.startThread(juce::Thread::Priority::normal); }
            ~Worker() { thread.stopThread(4000); }
            juce::TimeSliceThread thread {"MixPilot Spectrum"};
        };
        static Worker work;
        return work.thread;
    }

    int useTimeSlice() override
    {
        const auto requested = configuration.load(std::memory_order_relaxed);
        const auto lost = drops.load(std::memory_order_relaxed);
        if (requested != current || lost != lastDrops)
        {
            current = requested;
            lastDrops = lost;
            reset();
        }
        const unsigned end = cursorWrite.load(std::memory_order_acquire);
        auto read = cursorRead.load(std::memory_order_relaxed);
        const auto stop = std::min(end - read, static_cast<unsigned>(maximum));
        const int samples = 4096 << (current & 3);
        bool rendered = false;
        for (unsigned i = 0; i < stop && !rendered; ++i)
        {
            const auto sample = queue[read++ & (queueSize - 1)];
            if (sample.rate != activeRate || sample.session != activeSession)
            {
                activeRate = sample.rate;
                activeSession = sample.session;
                reset();
            }
            if (sample.position >= 0 && count > 0 && sample.position != previousPosition + 1)
                reset();
            previousPosition = sample.position;
            if (sample.position >= 0 && count == 0 && sample.position % samples != 0)
                continue;
            if (preparedSize != samples || preparedRate != sample.rate)
                prepare(samples, sample.rate);
            hasInput = hasInput || sample.left != 0 || sample.right != 0;
            hasOutput = hasOutput || sample.outputLeft != 0 || sample.outputRight != 0;
            outputDiffers = outputDiffers || sample.left != sample.outputLeft || sample.right != sample.outputRight;
            before[count] = {sample.left * hann[count], sample.right * hann[count]};
            after[count] = {sample.outputLeft * hann[count], sample.outputRight * hann[count]};
            if (++count == samples)
            {
                publish(sample, samples);
                count = 0;
                hasInput = hasOutput = outputDiffers = false;
                rendered = true;
            }
        }
        cursorRead.store(read, std::memory_order_release);
        return read == end ? 8 : 0;
    }

    void reset()
    {
        count = 0;
        hasInput = hasOutput = outputDiffers = false;
        averageInput.fill(0);
        averageOutput.fill(0);
        averagingInput = averagingOutput = false;
        longBefore.fill(0);
        recurrence.fill(0);
        frames = 0;
    }

    void prepare(int size, int rate)
    {
        preparedSize = size;
        preparedRate = rate;
        for (int i = 0; i < size; ++i)
            hann[i] = 0.5 - 0.5 * std::cos(2 * tonalPi * i / size);
        for (int i = 0; i < size / 2; ++i)
        {
            const double hz = static_cast<double>(i) * rate / size;
            bins[i] = hz >= 20 && hz <= 20000 ? spectralBand(hz) : -1;
            twiddles[i] = rotations[i * (maximum / size)];
        }
    }

    Spectrum transform(std::array<Complex, maximum>& data, int size, double& pan, SpectrumFrame* frame = nullptr) const
    {
        fft(data.data(), twiddles.data(), size);
        Spectrum power {};
        double l = 0, r = 0;
        Spectrum strongest {};
        for (int i = 1; i < size / 2; ++i)
        {
            const auto conjugate = std::conj(data[size - i]);
            const auto lBin = (data[i] + conjugate) * 0.5;
            const auto rBin = (data[i] - conjugate) * Complex(0.0, -0.5);
            const auto left = std::norm(lBin);
            const auto right = std::norm(rBin);
            l += left;
            r += right;
            if (bins[i] >= 0)
            {
                const int band = bins[i];
                const auto energy = static_cast<float>((left + right) * 8 / (3.0 * size * size));
                power[band] += energy;
                if (frame && energy > strongest[band])
                {
                    strongest[band] = energy;
                    const auto mid = lBin + rBin;
                    if (std::norm(mid) > 0.05 * (left + right))
                    {
                        frame->dominantHz[band] = static_cast<float>(i * static_cast<double>(preparedRate) / size);
                        frame->phase[band] = static_cast<float>(std::arg(mid));
                    }
                    else
                        frame->dominantHz[band] = 0;
                }
            }
        }
        pan = l + r > 1.0e-9 ? (r - l) / (l + r) : 0.0;
        return power;
    }

    void publish(const Sample& sample, int size)
    {
        SpectrumFrame frame;
        double unusedPan = 0;
        if (hasInput)
            frame.instant = transform(before, size, frame.pan, &frame);
        const auto output = !outputDiffers ? frame.instant
            : hasOutput ? transform(after, size, unusedPan) : Spectrum{};
        const auto seconds = (current >> 2) * 0.001;
        const auto retain = seconds > 0 ? std::exp(-size / (sample.rate * seconds)) : 0.0;
        const bool inputActive = spectrumEnergy(frame.instant) >= 1.0e-10;
        const bool outputActive = spectrumEnergy(output) >= 1.0e-10;
        auto average = [retain](Spectrum& stored, const Spectrum& fresh, bool wasActive, bool active)
        {
            if (!active)
                stored.fill(0);
            else if (!wasActive)
                stored = fresh;
            else
                for (int i = 0; i < tonalBands; ++i)
                    stored[i] = static_cast<float>(stored[i] * retain + fresh[i] * (1 - retain));
        };
        average(averageInput, frame.instant, averagingInput, inputActive);
        average(averageOutput, output, averagingOutput, outputActive);
        averagingInput = inputActive;
        averagingOutput = outputActive;
        frame.before = averageInput;
        frame.after = averageOutput;
        if (inputActive)
        {
            const auto peak = *std::max_element(frame.instant.begin(), frame.instant.end());
            const auto longRetention = std::exp(-size / (sample.rate * 2.5));
            const auto ringRetention = std::exp(-size / (sample.rate * 1.2));
            for (int i = 0; i < tonalBands; ++i)
            {
                longBefore[i] = frames == 0 ? frame.instant[i]
                    : static_cast<float>(longBefore[i] * longRetention + frame.instant[i] * (1 - longRetention));
                bool detected = false;
                if (i >= spectralBand(300) && i + 3 < tonalBands && frame.instant[i] >= peak * 0.03f
                    && frame.instant[i] >= frame.instant[i - 1] && frame.instant[i] >= frame.instant[i + 1])
                {
                    const auto surrounding = (frame.instant[i - 3] + frame.instant[i - 2]
                                              + frame.instant[i + 2] + frame.instant[i + 3]) * 0.25f;
                    detected = surrounding > 1.0e-9f && frame.instant[i] > surrounding * 6.3f;
                }
                recurrence[i] = static_cast<float>(recurrence[i] * ringRetention + (detected ? 1 - ringRetention : 0));
            }
            ++frames;
        }
        else
        {
            frames = 0;
            longBefore.fill(0);
            recurrence.fill(0);
        }
        frame.frames = frames;
        frame.longBefore = longBefore;
        frame.recurrence = recurrence;
        if (frames >= 6)
        {
            const auto dominant = std::max_element(recurrence.begin(), recurrence.end());
            if (*dominant >= 0.70f)
                frame.resonance = static_cast<int>(dominant - recurrence.begin());
        }
        frame.end = sample.position;
        frame.rate = sample.rate;
        frame.session = sample.session;
        frame.updated = juce::Time::getMillisecondCounter();
        std::lock_guard<std::mutex> guard(frameLock);
        latest = frame;
    }

    std::array<Sample, queueSize> queue {};
    std::atomic<unsigned> cursorRead {0}, cursorWrite {0}, drops {0};
    unsigned cachedRead = 0, lastDrops = 0;
    std::atomic<int> configuration {1200};
    int current = 1200, activeRate = 0, activeSession = 0, preparedSize = 0, preparedRate = 0, count = 0;
    std::int64_t previousPosition = -1;
    std::array<double, maximum> hann {};
    std::array<int, maximum / 2> bins {};
    std::array<Complex, maximum / 2> rotations {}, twiddles {};
    std::array<Complex, maximum> before {}, after {};
    Spectrum averageInput {}, averageOutput {}, longBefore {}, recurrence {};
    unsigned frames = 0;
    bool averagingInput = false, averagingOutput = false;
    bool hasInput = false, hasOutput = false, outputDiffers = false;
    mutable std::mutex frameLock;
    SpectrumFrame latest;
};
}
