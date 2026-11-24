#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ReferenceAudio.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
constexpr double rate = 48000;
constexpr int block = 512;

void require(bool result, const char* message)
{
    if (!result)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

juce::RangedAudioParameter& parameter(juce::AudioProcessor& plugin, const char* id)
{
    for (auto* raw : plugin.getParameters())
        if (auto* candidate = dynamic_cast<juce::RangedAudioParameter*>(raw);
            candidate != nullptr && candidate->getParameterID() == id)
            return *candidate;
    std::abort();
}

void write(juce::AudioProcessor& plugin, const char* id, float value)
{
    auto& p = parameter(plugin, id);
    p.setValueNotifyingHost(p.convertTo0to1(value));
}

float read(juce::AudioProcessor& plugin, const char* id)
{
    auto& p = parameter(plugin, id);
    return p.convertFrom0to1(p.getValue());
}

class Playhead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo result;
        result.setIsPlaying(true);
        result.setTimeInSamples(position);
        return result;
    }
    std::int64_t position = 0;
};

juce::Component* findControl(juce::Component& parent, const juce::String& key)
{
    for (auto* child : parent.getChildren())
    {
        if (child->isVisible() && (child->getTitle() == key
                || (dynamic_cast<juce::TextButton*>(child) != nullptr
                    && static_cast<juce::TextButton*>(child)->getButtonText() == key)))
            return child;
        if (child->isVisible())
            if (auto* result = findControl(*child, key))
                return result;
    }
    return nullptr;
}

void click(juce::Component& parent, const juce::String& key)
{
    auto* control = dynamic_cast<juce::Button*>(findControl(parent, key));
    if (!control || !control->onClick)
    {
        std::cerr << key << '\n';
        require(false, "Expected enabled editor command.");
        return;
    }
    control->onClick();
}

void feed(juce::AudioProcessor& plugin, Playhead& playhead, juce::AudioBuffer<double>& signal,
          juce::MidiBuffer& midi, double frequency, double amplitude, bool inverse)
{
    for (int i = 0; i < block; ++i)
    {
        const auto x = amplitude * std::sin(6.283185307179586 * frequency
                                          * (playhead.position + i) / rate);
        signal.setSample(0, i, x);
        signal.setSample(1, i, inverse ? -x : x);
    }
    plugin.processBlock(signal, midi);
}

void pauseTimer()
{
    juce::Thread::sleep(120);
    juce::Timer::callPendingTimersSynchronously();
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    using Plugin = std::unique_ptr<juce::AudioProcessor>;
    Plugin first(createPluginFilter()), second(createPluginFilter()), hub(createPluginFilter());
    Playhead playhead;
    juce::AudioProcessor::TrackProperties props;
    props.name = "Piano";
    first->updateTrackProperties(props);
    props.name = "Guitarra";
    second->updateTrackProperties(props);
    write(*hub, "instance", 1);
    for (auto* plugin : {first.get(), second.get(), hub.get()})
    {
        plugin->setPlayHead(&playhead);
        plugin->prepareToPlay(rate, block);
        require(plugin->getLatencySamples() == 0, "Analysis must not add audio latency.");
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor(hub->createEditor());
    juce::AudioBuffer<double> signal(2, block);
    juce::MidiBuffer midi;
    for (int i = 0; i < 90; ++i)
    {
        feed(*first, playhead, signal, midi, 996.09375, 0.12, false);
        feed(*second, playhead, signal, midi, 996.09375, 0.10, true);
        feed(*hub, playhead, signal, midi, 1253.90625, 0.18, false);
        playhead.position += block;
        if (i % 8 == 7)
            juce::Thread::sleep(12);
    }
    pauseTimer();
    const auto focus = juce::String("Marcar Piano como Foco");
    const auto visualPiano = juce::String("Mostrar espectro de Piano");
    const auto visualGuitar = juce::String("Mostrar espectro de Guitarra");
    auto* pianoVisibility = dynamic_cast<juce::Button*>(findControl(*editor, visualPiano));
    require(pianoVisibility && !pianoVisibility->getToggleState(), "HUB should start in Mix Bus view.");
    click(*editor, visualPiano);
    click(*editor, visualGuitar);
    require(pianoVisibility->getToggleState() && read(*first, "role") == 0,
            "Visual selection must never alter the track's role.");
    click(*editor, focus);
    require(read(*first, "role") == 1 && read(*second, "role") == 0 && read(*hub, "operation") == 1,
            "HUB focus command must affect only eligible instances in its session.");

    click(*editor, "Tonal Match");
    click(*editor, "Alvo de EQ: Guitarra");
    click(*editor, "Mascaramento");
    auto* suggestion = dynamic_cast<juce::Button*>(findControl(*editor, "Aplicar EQ"));
    require(suggestion && suggestion->isEnabled(), "Synchronous overlapping tracks must generate a staged cut.");
    require(read(*second, "toneG1") == 0, "Analysis must not write EQ.");
    click(*suggestion->getParentComponent(), "Ouvir B");
    require(read(*second, "toneG1") == 0, "Preview must not write EQ.");
    click(*suggestion->getParentComponent(), "Aplicar EQ");
    require(read(*second, "toneG1") < -0.1f, "Approved masking correction did not reach the target Track.");
    click(*suggestion->getParentComponent(), "Desfazer EQ");
    require(std::abs(read(*second, "toneG1")) < 1.0e-5f, "Undo did not restore the original EQ.");

    require(editor->getHeight() == 710, "The editor should start in the essential view.");
    click(*editor, juce::String::fromUTF8("Avançado"));
    require(editor->getHeight() == 966 && findControl(*editor, "RESET") != nullptr,
            "Advanced controls must stay accessible after switching views.");
    click(*editor, "Essencial");
    require(editor->getHeight() == 710 && findControl(*editor, "RESET") == nullptr,
            "The essential view must hide only the advanced controls.");
    click(*editor, "Tonal Match"); // Close the tonal panel before adjusting gain.
    write(*first, "trim", -4.7f);
    write(*second, "trim", 6);
    click(*editor, "AUTO GAIN");
    for (int frame = 0; frame < 34; ++frame)
    {
        for (int i = 0; i < 4; ++i)
        {
            feed(*first, playhead, signal, midi, 996.09375, 0.12, false);
            feed(*second, playhead, signal, midi, 996.09375, frame < 17 ? 0.5 : 0.011, true);
            feed(*hub, playhead, signal, midi, 1253.90625, 0.18, false);
            playhead.position += block;
        }
        juce::Thread::sleep(47);
        juce::Timer::callPendingTimersSynchronously();
    }
    require(std::abs(read(*first, "trim") + 4.7f) < 0.02f,
            "Musical Auto Gain must leave a safe focus level unchanged.");
    require(read(*second, "trim") <= -3.9f,
            "Musical Auto Gain must use the learned loud passage and guitar ceiling even when the final window is quiet.");

    const auto file = juce::File::createTempFile(".wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
                                                  .withSampleRate(rate).withNumChannels(2).withBitsPerSample(24));
        require(writer != nullptr, "Unable to create reference fixture.");
        juce::AudioBuffer<float> samples(2, 48000);
        for (int i = 0; i < 48000; ++i)
        {
            const auto x = static_cast<float>(0.125 * std::sin(6.283185307179586 * 1000 * i / rate));
            samples.setSample(0, i, x);
            samples.setSample(1, i, x * 0.5f);
        }
        require(writer->writeFromAudioSampleBuffer(samples, 0, 48000), "Failed to write reference fixture.");
    }
    const auto reference = nx::loadReferenceAudio(file);
    file.deleteFile();
    require(reference.valid && std::abs(reference.peak - 20 * std::log10(0.125)) < 0.04
        && reference.correlation > 0.999 && reference.balance < -0.55
        && reference.loudness > -25 && reference.loudness < -18,
        "Imported WAV must report real peak, channel relationships and gated LUFS.");
    require(reference.curve[nx::spectralBand(1000)] > 0.0001f, "Imported reference must contain the source spectrum.");

    write(*first, "toneG1", -3);
    for (int i = 0; i < 8; ++i)
    {
        feed(*first, playhead, signal, midi, 1000, 0.16, true);
        playhead.position += block;
    }
    require(std::abs(signal.getSample(0, 19) + signal.getSample(1, 19)) < 1.0e-12,
            "VST3 processor EQ must preserve interchannel phase.");
    juce::MemoryBlock state;
    first->getStateInformation(state);
    second->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    require(std::abs(read(*second, "toneG1") + 3) < 1.0e-5, "Host state failed to restore EQ automation parameters.");

    // Exercise the complete callback/worker plumbing of 32 concurrent instances with shared test buffers.
    std::vector<Plugin> processors;
    for (int i = 0; i < 32; ++i)
    {
        processors.emplace_back(createPluginFilter());
        processors.back()->prepareToPlay(rate, block);
    }
    std::vector<std::unique_ptr<juce::AudioBuffer<double>>> buffers;
    for (std::size_t i = 0; i < processors.size(); ++i)
    {
        buffers.push_back(std::make_unique<juce::AudioBuffer<double>>(2, block));
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < block; ++sample)
                buffers.back()->setSample(channel, sample, 0.02 * std::sin(sample / 8.0));
    }
    const auto started = std::chrono::steady_clock::now();
    for (int n = 0; n < 12; ++n)
        for (std::size_t i = 0; i < processors.size(); ++i)
            processors[i]->processBlock(*buffers[i], midi);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    for (const auto& buffer : buffers)
        require(std::isfinite(buffer->getSample(0, 200)), "Concurrent validation produced invalid output.");
    std::cout << "HUB/TRACK role selection, display isolation, zero latency, double stereo phase and restored EQ state passed. "
              << "32 callbacks / 128 ms audio: " << ms << " ms aggregate elapsed.\n";
}
