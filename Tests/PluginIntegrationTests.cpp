#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ReferenceAudio.h"
#include "SpectralWorker.h"
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
        if (child->isVisible() && !child->getBounds().isEmpty() && (child->getTitle() == key
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
    auto* tonalAction = findControl(*editor, "Mascaramento");
    require(tonalAction && editor->getLocalBounds().contains(editor->getLocalArea(tonalAction, tonalAction->getLocalBounds())),
            "Tonal actions must remain visible inside the essential editor.");
    click(*editor, "Alvo de EQ: Guitarra");
    click(*editor, "Mascaramento");
    auto* suggestion = dynamic_cast<juce::Button*>(findControl(*editor, "Aplicar EQ"));
    require(suggestion && suggestion->isEnabled(), "Synchronous overlapping tracks must generate a staged cut.");
    require(read(*second, "toneG1") == 0, "Analysis must not write EQ.");
    click(*suggestion->getParentComponent(), "Ouvir B");
    require(read(*second, "toneG1") == 0, "Preview must not write EQ.");
    const auto originalTrim = read(*second, "trim");
    click(*suggestion->getParentComponent(), "Aplicar EQ");
    require(read(*second, "toneG1") < -0.1f, "Approved masking correction did not reach the target Track.");
    require(read(*second, "trim") == originalTrim, "Applying EQ must not rewrite gain staging.");
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

    auto* styles = dynamic_cast<juce::ComboBox*>(findControl(*editor, "Estilo de ganho"));
    require(styles && styles->getNumItems() == 6 && styles->getText() == "Universal", "Six styles must be accessible in the compact HUB.");
    styles->setSelectedId(2, juce::sendNotificationSync);
    require(read(*hub, "gainStyle") == 1, "Selecting Worship must reach the persisted gain parameter.");
    juce::MemoryBlock hubState;
    hub->getStateInformation(hubState);
    {
        Plugin restoredHub(createPluginFilter());
        restoredHub->setStateInformation(hubState.getData(), static_cast<int>(hubState.getSize()));
        require(read(*restoredHub, "gainStyle") == 1, "The style must survive session save and restore.");
    }

    write(*first, "trim", 0);
    write(*second, "trim", 0);
    write(*hub, "limit", 12);
    click(*editor, "AUTO GAIN");
    for (int frame = 0; frame < 34; ++frame)
    {
        for (int i = 0; i < 4; ++i)
        {
            feed(*first, playhead, signal, midi, 996.09375, 0.12, false);
            feed(*second, playhead, signal, midi, 996.09375, frame < 17 ? 0.12 : 0.011, true);
            feed(*hub, playhead, signal, midi, 1253.90625, 0.18, false);
            playhead.position += block;
        }
        juce::Thread::sleep(47);
        juce::Timer::callPendingTimersSynchronously();
    }
    require(std::abs(read(*first, "trim")) < 0.02f, "A new style must not boost or attenuate a safe focus.");
    require(std::abs(read(*second, "trim") + 6.5f) < 0.2f,
            "Worship Auto Gain must use the louder accompaniment RMS windows even when its peak ceiling is not active.");

    {
        Plugin kick(createPluginFilter());
        props.name = "BUMBO - C1";
        kick->updateTrackProperties(props);
        write(*kick, "gainStyle", 1);
        write(*kick, "peakReference", 1);
        kick->setPlayHead(&playhead);
        kick->prepareToPlay(rate, block);
        std::unique_ptr<juce::AudioProcessorEditor> kickEditor(kick->createEditor());
        pauseTimer();
        constexpr double kickSamplePeak = 0.1798870915; // -14.9 dBFS
        const auto renderKick = [&](bool hit)
        {
            signal.clear();
            if (hit)
                for (int i = 0; i < 64; ++i)
                    for (int channel = 0; channel < 2; ++channel)
                        signal.setSample(channel, i, kickSamplePeak * (1.0 - i / 64.0));
            kick->processBlock(signal, midi);
            playhead.position += block;
        };
        const auto captureKick = [&](bool withHits)
        {
            click(*kickEditor, "AUTO GAIN");
            for (int frame = 0; frame < 34; ++frame)
            {
                for (int i = 0; i < 4; ++i)
                    renderKick(withHits && i == 1 && (frame == 2 || frame == 7 || frame == 12));
                juce::Thread::sleep(47);
                juce::Timer::callPendingTimersSynchronously();
            }
        };
        captureKick(true);
        require(std::abs(read(*kick, "trim") - 4.9f) < 0.02f,
                "A sparse kick followed by silence must receive the technical peak correction.");
        for (int i = 0; i < 5; ++i)
            renderKick(false);
        renderKick(true);
        require(std::abs(signal.getMagnitude(0, block) - std::pow(10.0, -10.0 / 20.0)) < 0.001,
                "The correction must bring the actual insert output to the kick peak target.");
        captureKick(true);
        require(std::abs(read(*kick, "trim") - 4.9f) < 0.02f,
                "Repeating technical calibration must not compound the kick gain.");
        click(*kickEditor, "Desfazer");
        require(std::abs(read(*kick, "trim")) < 0.02f, "Undo must restore the original kick trim.");
        captureKick(false);
        require(std::abs(read(*kick, "trim")) < 0.02f, "A silent capture must not boost the kick.");
    }

    {
        Plugin mix(createPluginFilter()), vocal(createPluginFilter()), backing(createPluginFilter()),
            piano(createPluginFilter()), bus(createPluginFilter());
        props.name = "Lead Vox";
        vocal->updateTrackProperties(props);
        props.name = "BGV L";
        backing->updateTrackProperties(props);
        props.name = "Piano";
        piano->updateTrackProperties(props);
        props.name = "Piano Bus";
        bus->updateTrackProperties(props);
        for (auto* plugin : {mix.get(), vocal.get(), backing.get(), piano.get(), bus.get()})
        {
            write(*plugin, "session", 2);
            plugin->setPlayHead(&playhead);
            plugin->prepareToPlay(rate, block);
        }
        write(*mix, "instance", 1);
        std::unique_ptr<juce::AudioProcessorEditor> mixEditor(mix->createEditor());
        pauseTimer();
        click(*mixEditor, "AUTO GAIN");
        for (int frame = 0; frame < 34; ++frame)
        {
            for (int i = 0; i < 4; ++i)
            {
                feed(*vocal, playhead, signal, midi, 375, frame < 14 ? 0.18 : 0.0, false);
                feed(*backing, playhead, signal, midi, 375, frame < 14 ? 0.10 : 0.0, false);
                feed(*piano, playhead, signal, midi, 375, frame < 14 ? 0.32 : 0.0, false);
                feed(*bus, playhead, signal, midi, 375, 0.35, false);
                playhead.position += block;
            }
            juce::Thread::sleep(47);
            juce::Timer::callPendingTimersSynchronously();
        }
        require(std::abs(read(*vocal, "trim") - 4.89f) < 0.05f, "HUB must raise a quiet lead vocal toward -10 dBFS.");
        require(std::abs(read(*backing, "trim") - 4.0f) < 0.05f, "HUB must use the distinct -16 dBFS backing preset.");
        require(std::abs(read(*piano, "trim") + 4.10f) < 0.05f, "HUB must attenuate a hot piano toward -14 dBFS.");
        require(std::abs(read(*bus, "trim")) < 0.02f, "Auto Gain must avoid processing a named bus again.");

        write(*vocal, "trim", -4.4f);
        write(*vocal, "limit", 4.4f);
        write(*vocal, "role", 1);
        std::unique_ptr<juce::AudioProcessorEditor> vocalEditor(vocal->createEditor());
        pauseTimer();
        click(*vocalEditor, "AUTO GAIN");
        const auto vocalPeak = std::pow(10.0, -7.1 / 20.0);
        for (int frame = 0; frame < 34; ++frame)
        {
            for (int i = 0; i < 4; ++i)
            {
                feed(*vocal, playhead, signal, midi, 375, vocalPeak, false);
                playhead.position += block;
            }
            juce::Thread::sleep(47);
            juce::Timer::callPendingTimersSynchronously();
        }
        require(std::abs(read(*vocal, "trim") + 2.9f) < 0.02f,
                "Track Auto Gain must replace an excessive previous trim on the focus vocal.");
        feed(*vocal, playhead, signal, midi, 375, vocalPeak, false);
        require(std::abs(signal.getMagnitude(0, block) - std::pow(10.0, -10.0 / 20.0)) < 0.001,
                "The focus vocal insert audio must reach the target sample peak.");
    }

    {
        Plugin vocal(createPluginFilter());
        props.name = "VOCAL";
        vocal->updateTrackProperties(props);
        vocal->setPlayHead(&playhead);
        vocal->prepareToPlay(rate, block);
        write(*vocal, "trim", -3.83f);
        write(*vocal, "reference", -18);
        std::unique_ptr<juce::AudioProcessorEditor> vocalEditor(vocal->createEditor());
        click(*vocalEditor, "Tonal Match");
        click(*vocalEditor, "Sugerir EQ");
        auto* tonalApply = dynamic_cast<juce::Button*>(findControl(*vocalEditor, "Aplicar EQ"));
        require(tonalApply && !tonalApply->isEnabled() && read(*vocal, "toneG1") == 0,
                "Tonal Match without captured audio must not enable Apply or write EQ.");
        click(*vocalEditor, "Tonal Match");

        click(*vocalEditor, "Nivelar RMS");
        for (int frame = 0; frame < 34; ++frame)
        {
            for (int i = 0; i < 4; ++i)
            {
                feed(*vocal, playhead, signal, midi, 375, frame < 18 ? 0.1 : 0.0, false);
                playhead.position += block;
            }
            juce::Thread::sleep(47);
            juce::Timer::callPendingTimersSynchronously();
        }
        require(std::abs(read(*vocal, "trim") - 5.01) < 0.08,
                "RMS leveling must use captured active audio, even when the clip ends with a pause.");
        const auto measure = [&]
        {
            for (int i = 0; i < 8; ++i)
            {
                feed(*vocal, playhead, signal, midi, 375, 0.1, false);
                playhead.position += block;
            }
            return 20 * std::log10(signal.getRMSLevel(0, 0, block));
        };
        require(std::abs(measure() + 18) < 0.08, "RMS leveling must reach the nominal output on a known signal.");
        click(*vocalEditor, "Ouvir original");
        require(std::abs(measure() + 23.01) < 0.02 && std::abs(read(*vocal, "trim") - 5.01) < 0.08,
                "Audition must restore the actual original audio without changing the saved trim.");
        click(*vocalEditor, "Ouvir ajuste");
        require(std::abs(measure() + 18) < 0.08, "Returning to the applied trim must restore the adjusted output.");
    }

    {
        Plugin dynamics(createPluginFilter());
        dynamics->prepareToPlay(rate, block);
        std::unique_ptr<juce::AudioProcessorEditor> dynamicsEditor(dynamics->createEditor());
        click(*dynamicsEditor, juce::String::fromUTF8("Avançado"));
        require(findControl(*dynamicsEditor, juce::String::fromUTF8("Ganho contínuo")) != nullptr
                && findControl(*dynamicsEditor, juce::String::fromUTF8("Limitador de segurança")) != nullptr,
                "Both dynamics controls must be accessible in the advanced view.");
        require(read(*dynamics, "continuousGain") == 0 && read(*dynamics, "safetyLimiter") == 0,
                "Dynamics modules must be disabled for new instances.");
        dynamics->setPlayHead(&playhead);
        write(*dynamics, "continuousGain", 1);
        for (int frame = 0; frame < 400; ++frame)
        {
            feed(*dynamics, playhead, signal, midi, 375, 0.08, true);
            playhead.position += block;
        }
        require(std::abs(signal.getMagnitude(0, 0, block) - 0.15962) < 0.002,
                "Continuous gain must actually raise the output of a quiet signal.");
        write(*dynamics, "safetyLimiter", 1);
        write(*dynamics, "limiterCeiling", -6);
        feed(*dynamics, playhead, signal, midi, 375, 0.95, true);
        require(signal.getMagnitude(0, block) <= 0.501188, "Limiter must bound the output of the insert.");
        require(dynamics->getLatencySamples() == 0, "The sample-peak limiter must add zero latency.");
        write(*dynamics, "bypass", 1);
        feed(*dynamics, playhead, signal, midi, 375, 0.95, true);
        require(std::abs(signal.getMagnitude(0, block) - 0.95) < 0.001,
                "Bypass must exclude both processing stages.");
        juce::MemoryBlock state;
        dynamics->getStateInformation(state);
        Plugin restored(createPluginFilter());
        restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        require(read(*restored, "continuousGain") == 1 && read(*restored, "safetyLimiter") == 1
                && std::abs(read(*restored, "limiterCeiling") + 6) < 0.02,
                "The processing switches and ceiling must survive preset roundtrip.");
    }

    {
        Plugin source(createPluginFilter()), restored(createPluginFilter());
        write(*source, "toneF12", 3300);
        write(*source, "toneG12", -2.5f);
        write(*source, "toneQ12", 2.1f);
        juce::MemoryBlock state;
        source->getStateInformation(state);
        restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        require(std::abs(read(*restored, "toneF12") - 3300) < 0.1f
                && std::abs(read(*restored, "toneG12") + 2.5f) < 0.01f
                && std::abs(read(*restored, "toneQ12") - 2.1f) < 0.01f,
                "The twelfth EQ band must roundtrip through the plugin state.");

        auto worker = std::make_unique<nx::SpectralWorker>();
        constexpr unsigned token = 55;
        worker->capture(token, 15);
        for (int frame = 0; frame < 180; ++frame)
        {
            for (int i = 0; i < 4096; ++i)
                worker->push({0, 0, 0, 0, frame * 4096LL + i, 48000, 1});
            if (frame % 3 == 0)
                juce::Thread::sleep(9);
        }
        juce::Thread::sleep(30);
        require(worker->captureProgress(token).seconds == 0, "Silent FFT frames must not advance tonal capture.");
        for (int frame = 0; frame < 240 && worker->captureProgress(token).seconds < 15; ++frame)
        {
            for (int i = 0; i < 4096; ++i)
            {
                const auto sample = static_cast<float>(0.1 * std::sin(6.283185307179586 * i / 48));
                worker->push({sample, sample, sample, sample, (180 + frame) * 4096LL + i, 48000, 1});
            }
            if (frame % 3 == 0)
                juce::Thread::sleep(9);
        }
        juce::Thread::sleep(45);
        const auto captured = worker->captureProgress(token);
        require(captured.seconds >= 15 && captured.seconds < 15.09, "Tonal capture must finish at 15 seconds of active audio.");
        const auto peak = std::distance(captured.power.begin(), std::max_element(captured.power.begin(), captured.power.end()));
        require(std::abs(nx::bandFrequency(static_cast<double>(peak)) - 1000) < 80,
                "Captured spectrum must retain the actual tone frequency.");
    }

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
