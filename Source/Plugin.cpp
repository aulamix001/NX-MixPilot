#include "Analysis.h"
#include "GlassLook.h"
#include "InstrumentPresets.h"
#include "SpectralWorker.h"
#include "TonalPanel.h"
#include "SmartGain.h"
#include "SmartPanel.h"
#include "ReferenceAudio.h"
#include <set>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

constexpr int capacity = 64;
constexpr float noPreview = 1000.0f;
constexpr auto ink = 0xffe7edf5;
constexpr auto cyan = 0xff24d3f5;
constexpr auto muted = 0xffa0afc0;

struct PublishedLevels
{
    void write(const nx::StereoLevels& value)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            peak[channel] = value.peak[channel];
            truePeak[channel] = value.truePeak[channel];
            rms[channel] = value.rms[channel];
            vu[channel] = value.vu[channel];
            hold[channel] = value.hold[channel];
        }
    }

    nx::StereoLevels read() const
    {
        nx::StereoLevels value;
        for (int channel = 0; channel < 2; ++channel)
        {
            value.peak[channel] = peak[channel];
            value.truePeak[channel] = truePeak[channel];
            value.rms[channel] = rms[channel];
            value.vu[channel] = vu[channel];
            value.hold[channel] = hold[channel];
        }
        return value;
    }

    std::array<std::atomic<float>, 2> peak {}, truePeak {}, rms {}, vu {}, hold {};
};

struct Measurement
{
    void write(const nx::Reading& reading, int group)
    {
        ++version;
        rms.store(reading.rms);
        activeRms.store(reading.activeRms);
        activity.store(reading.activity);
        peak.store(reading.peak);
        outputPeak.store(reading.outputPeak);
        input.write(reading.inputLevels);
        output.write(reading.outputLevels);
        learningId.store(reading.learningId);
        learnedPeak.store(reading.learnedPeak);
        learnedMaxSample.store(reading.learnedMaxSample);
        learnedCeiling.store(reading.learnedCeiling);
        learnedActiveWindows.store(reading.learnedActiveWindows);
        learnedEnergy.store(reading.learnedEnergy);
        learnedLoudEnergy.store(reading.learnedLoudEnergy);
        sampleRate.store(reading.sampleRate);
        settled.store(reading.settled);
        position.store(reading.position);
        session.store(group);
        for (int i = 0; i < nx::bands; ++i)
            power[i].store(reading.power[i]);
        stamp.store(juce::Time::getMillisecondCounter());
        ++version;
    }

    bool read(nx::Reading& reading, int group, juce::uint32* timestamp = nullptr) const
    {
        const auto before = version.load();
        if ((before & 1u) || before == 0)
            return false;
        reading.rms = rms.load();
        reading.activeRms = activeRms.load();
        reading.activity = activity.load();
        reading.peak = peak.load();
        reading.outputPeak = outputPeak.load();
        reading.inputLevels = input.read();
        reading.outputLevels = output.read();
        reading.learningId = learningId.load();
        reading.learnedPeak = learnedPeak.load();
        reading.learnedMaxSample = learnedMaxSample.load();
        reading.learnedCeiling = learnedCeiling.load();
        reading.learnedActiveWindows = learnedActiveWindows.load();
        reading.learnedEnergy = learnedEnergy.load();
        reading.learnedLoudEnergy = learnedLoudEnergy.load();
        reading.sampleRate = sampleRate.load();
        reading.settled = settled.load();
        reading.position = position.load();
        for (int i = 0; i < nx::bands; ++i)
            reading.power[i] = power[i].load();
        const auto published = stamp.load();
        const auto age = juce::Time::getMillisecondCounter() - published;
        if (session.load() != group || age >= 500 || before != version.load())
            return false;
        if (timestamp != nullptr)
            *timestamp = published;
        return true;
    }

    PublishedLevels input, output;
    std::atomic<unsigned> version {0}, learningId {0};
    std::atomic<float> learnedPeak {nx::floorDb}, learnedCeiling {nx::floorDb}, learnedEnergy {nx::floorDb}, learnedLoudEnergy {nx::floorDb}, learnedMaxSample {nx::floorDb};
    std::atomic<unsigned> learnedActiveWindows {0};
    std::atomic<float> activeRms {nx::floorDb}, activity {0};
    std::atomic<float> rms {nx::floorDb}, peak {nx::floorDb}, outputPeak {nx::floorDb};
    std::atomic<bool> settled {false};
    std::atomic<int> sampleRate {0}, session {0};
    std::atomic<std::int64_t> position {-1};
    std::atomic<juce::uint32> stamp {0};
    std::array<std::atomic<float>, nx::bands> power {};
};

constexpr std::array<const char*, 3> toneFrequencies {"toneF1", "toneF2", "toneF3"};
constexpr std::array<const char*, 3> toneGains {"toneG1", "toneG2", "toneG3"};
constexpr std::array<const char*, 3> toneQs {"toneQ1", "toneQ2", "toneQ3"};

struct TonalChange
{
    unsigned track = 0;
    nx::EqualizerSettings before, after;
    double conflictHz = 0, phaseDifference = 999;
};

struct TonePreview
{
    void write(const nx::EqualizerSettings& settings, unsigned ownerId, int sessionId)
    {
        ++revision;
        for (std::size_t i = 0; i < settings.bands.size(); ++i)
        {
            frequency[i] = static_cast<float>(settings.bands[i].frequency);
            gain[i] = static_cast<float>(settings.bands[i].gain);
            q[i] = static_cast<float>(settings.bands[i].q);
        }
        compensation = static_cast<float>(settings.compensation);
        session = sessionId;
        owner = ownerId;
        ++revision;
    }

    bool read(nx::EqualizerSettings& settings, int sessionId) const
    {
        const auto start = revision.load();
        if ((start & 1u) != 0 || owner.load() == 0 || session.load() != sessionId)
            return false;
        for (std::size_t i = 0; i < settings.bands.size(); ++i)
            settings.bands[i] = {frequency[i].load(), gain[i].load(), q[i].load()};
        settings.compensation = compensation.load();
        return start == revision.load();
    }

    std::array<std::atomic<float>, 3> frequency {}, gain {}, q {};
    std::atomic<float> compensation {0};
    std::atomic<int> session {0};
    std::atomic<unsigned> owner {0}, revision {0};
};

struct Change
{
    unsigned identity;
    float previous, next;
    std::optional<float> previousApplied;
};

class Processor;
struct Registry
{
    std::mutex mutex;
    std::array<Processor*, capacity> tracks {};
    unsigned nextIdentity = 1, nextLearningId = 1;
};

Registry& registry()
{
    static Registry value;
    return value;
}

class Processor final : public juce::AudioProcessor
{
public:
    Processor()
        : AudioProcessor(BusesProperties().withInput("Entrada", juce::AudioChannelSet::stereo(), true)
                                          .withOutput(utf8("Saída"), juce::AudioChannelSet::stereo(), true)),
          parameters(*this, nullptr, "NXMixPilot", layout())
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        identity = registry().nextIdentity++;
        auto& tracks = registry().tracks;
        auto free = std::find(tracks.begin(), tracks.end(), nullptr);
        if (free != tracks.end())
        {
            *free = this;
            slot = static_cast<int>(free - tracks.begin());
        }
    }

    ~Processor() override
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (slot >= 0)
            registry().tracks[static_cast<std::size_t>(slot)] = nullptr;
    }

    static juce::StringArray peakNames()
    {
        return {utf8("Sem referência de pico"), "Kick", "Snare", utf8("Hi-hat / Percussão"), "Baixo",
                "Piano / Teclados", "Guitarras", "Pads / Strings", "Sintetizadores",
                "Vocal principal", "Backing vocals"};
    }

    static juce::StringArray supportNames()
    {
        return {utf8("Padrão"), "Voz de apoio", "Harmonia", utf8("Percussão auxiliar"), utf8("Ambiência / FX")};
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout result;
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{"instance", 1}, utf8("Instância"),
                        StringArray{"Track", "HUB"}, 0));
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{"role", 1}, "Papel musical",
                        StringArray{"Auto", "Foco", "Baixo", "Bateria", "Acompanhamento"}, 0));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"session", 1}, utf8("Sessão"), 1, 99, 1));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"group", 1}, "Grupo", 1, 16, 1));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"trim", 1}, "Trim interno",
                        NormalisableRange<float>{-24, 24, 0.01f}, 0));
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{"operation", 1}, utf8("Operação"),
                        StringArray{utf8("Calibração técnica"), utf8("Balanço musical")}, 0));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"reference", 1}, "0 VU (dBFS RMS)",
                        NormalisableRange<float>{-36, -6, 0.1f}, nx::nominalRms));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"limit", 1}, "Limite de ajuste (dB)",
                        NormalisableRange<float>{0, 24, 0.1f}, 6));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"headroom", 1}, "Margem de pico (dB)",
                        NormalisableRange<float>{0, 18, 0.1f}, 3));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"bypass", 1}, "Bypass", false));
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{"support", 1},
                        "Tipo de acompanhamento", supportNames(), 0));
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{"peakReference", 1},
                        utf8("Referência de pico"), peakNames(), 0));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"instrument", 1},
                        "Instrumento", 0, static_cast<int>(std::size(nx::presets)), 0));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"targetPeak", 1}, "Target de pico",
                        NormalisableRange<float>{-36, 1, 0.1f}, 1));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"outputTrim", 1}, utf8("Ganho de saída"),
                        NormalisableRange<float>{-24, 24, 0.01f}, 0));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"learnTime", 1}, "Janela de aprendizado (s)",
                        NormalisableRange<float>{1.2f, 20, 0.1f}, 1.2f));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"sensitivity", 1}, "Sensibilidade",
                        NormalisableRange<float>{0, 1, 0.01f}, 1));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"freeze", 1}, "Freeze", false));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"trueCeiling", 1}, "Limite true peak (dBTP)",
                        NormalisableRange<float>{-18, 0, 0.1f}, -1));
        for (std::size_t band = 0; band < toneFrequencies.size(); ++band)
        {
            const auto index = juce::String(band + 1);
            result.add(std::make_unique<AudioParameterFloat>(ParameterID{toneFrequencies[band], 1},
                            utf8("EQ Frequência ") + index, NormalisableRange<float>{20, 20000, 0, 0.35f}, 1000));
            result.add(std::make_unique<AudioParameterFloat>(ParameterID{toneGains[band], 1},
                            "EQ Ganho " + index, NormalisableRange<float>{-12, 12}, 0));
            result.add(std::make_unique<AudioParameterFloat>(ParameterID{toneQs[band], 1},
                            "EQ Q " + index, NormalisableRange<float>{0.3f, 8}, 0.8f));
        }
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"toneComp", 1}, utf8("Compensação tonal"),
                        NormalisableRange<float>{-3, 3}, 0));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"toneBypass", 1}, "Bypass tonal", false));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"toneExclude", 1}, "Excluir do EQ", false));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"smartStrategy", 1}, "Smart strategy", 0, 2, 0));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"priority", 1}, "Prioridade", 0, 100, 50));
        result.add(std::make_unique<AudioParameterInt>(ParameterID{"function", 1}, utf8("Função musical"), 0, 5, 0));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"volumeLock", 1}, "Travar volume", false));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"eqLock", 1}, "Travar EQ", false));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"referenceOnly", 1}, utf8("Pista de referência"), false));
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{"premasterPeak", 1}, utf8("Pico pré-master"),
                        NormalisableRange<float>{-24, 0, 0.1f}, -6));
        return result;
    }

    float value(const char* name) const { return parameters.getRawParameterValue(name)->load(); }
    int session() const { return static_cast<int>(value("session")); }
    bool hub() const { return value("instance") > 0.5f; }
    nx::Role role() const { return static_cast<nx::Role>(static_cast<int>(value("role"))); }
    nx::Role musicalRole() const
    {
        const auto base = role();
        const auto variant = static_cast<int>(value("support"));
        return variant > 0 && (base == nx::automatic || base == nx::music)
            ? static_cast<nx::Role>(nx::backingVocal + variant - 1) : base;
    }

    int group() const { return static_cast<int>(value("group")); }
    void updateTrackProperties(const TrackProperties& properties) override
    {
        if (properties.name)
        {
            hostTrackName = properties.name->trim();
            inferredInstrument.store(inferInstrument(), std::memory_order_relaxed);
            const auto name = hostTrackName.toLowerCase();
            namedBus.store(name.containsWholeWord("bus") || name == "master" || name == "main",
                           std::memory_order_relaxed);
        }
    }

    juce::String displayName() const
    {
        return hostTrackName.isNotEmpty() ? hostTrackName : "Pista " + juce::String(identity);
    }


    void setTrim(float dB) { setParameter("trim", dB); }

    void setParameter(const char* id, float rawValue)
    {
        auto* parameter = parameters.getParameter(id);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(rawValue));
        parameter->endChangeGesture();
    }

    int instrument() const
    {
        const int selection = static_cast<int>(value("instrument"));
        return selection > 0 ? selection : inferredInstrument.load(std::memory_order_relaxed);
    }

    float instrumentCeiling() const
    {
        if (hub())
            return 1;
        const auto custom = value("targetPeak");
        if (custom <= 0)
            return custom;
        const auto legacy = static_cast<int>(value("peakReference"));
        if (value("instrument") == 0 && legacy > 0 && legacy <= static_cast<int>(nx::instrumentPeakRanges.size()))
            return nx::instrumentPeakRanges[static_cast<std::size_t>(legacy - 1)].maximum;
        const int preset = instrument();
        return preset > 0 ? nx::instrumentPresetCeiling(nx::presets[preset - 1]) : 1.0f;
    }

    int inferInstrument() const
    {
        const auto name = hostTrackName.toLowerCase();
        if (name.isEmpty())
            return 0;
        int best = 0, longest = 0;
        for (std::size_t i = 0; i < std::size(nx::presets); ++i)
        {
            const auto candidate = utf8(nx::presets[i].name).toLowerCase();
            if (candidate.length() > longest && name.contains(candidate))
            {
                best = static_cast<int>(i) + 1;
                longest = candidate.length();
            }
        }
        if (best != 0)
            return best;
        for (const auto& alias : {std::pair<const char*, const char*>{"bass", "Baixo elétrico"},
                                 {"baixo", "Baixo elétrico"}, {"piano", "Piano acústico"},
                                 {"bumbo", "Kick"}, {"caixa", "Snare"}, {"hi-hat", "Hi-hat fechado"}, {"chimbal", "Hi-hat fechado"},
                                 {"keys", "Teclado de arranjo"}, {"guitar", "Guitarra clean"},
                                 {"guitarra", "Guitarra clean"}, {"backing", "Backing uníssono"},
                                 {"coral", "Coral misto"}, {"vocal", "Vocal principal masculino"},
                                 {"voz", "Vocal principal masculino"}})
            if (name.contains(alias.first))
                for (std::size_t i = 0; i < std::size(nx::presets); ++i)
                    if (utf8(nx::presets[i].name) == utf8(alias.second))
                        return static_cast<int>(i) + 1;
        return 0;
    }

    nx::EqualizerSettings tone() const
    {
        nx::EqualizerSettings settings;
        for (std::size_t i = 0; i < settings.bands.size(); ++i)
            settings.bands[i] = {value(toneFrequencies[i]), value(toneGains[i]), value(toneQs[i])};
        settings.compensation = value("toneComp");
        return settings;
    }

    void setTone(const nx::EqualizerSettings& request)
    {
        toneBatch.store(true);
        for (std::size_t i = 0; i < request.bands.size(); ++i)
        {
            setParameter(toneFrequencies[i], static_cast<float>(request.bands[i].frequency));
            setParameter(toneGains[i], static_cast<float>(request.bands[i].gain));
            setParameter(toneQs[i], static_cast<float>(request.bands[i].q));
        }
        setParameter("toneComp", static_cast<float>(request.compensation));
        toneBatch.store(false);
    }

    float tonalBoost() const
    {
        if (tonalBus() || value("toneBypass") > 0.5f)
            return 0;
        const auto settings = tone();
        auto boost = std::max(0.0, settings.compensation);
        for (const auto& band : settings.bands)
            boost += std::max(0.0, band.gain);
        return static_cast<float>(boost);
    }

    bool tonalBus() const
    {
        const int assigned = static_cast<int>(value("instrument"));
        const int inferred = inferredInstrument.load(std::memory_order_relaxed);
        const int instrumentId = assigned > 0 ? assigned : inferred;
        return hub() || value("toneExclude") > 0.5f || namedBus.load(std::memory_order_relaxed)
            || (instrumentId > 0 && nx::presets[instrumentId - 1].category == 7);
    }

    bool eligible(int sessionId) const { return !hub() && session() == sessionId && value("bypass") < 0.5f; }
    bool volumeUnlocked() const { return value("volumeLock") < 0.5f && value("referenceOnly") < 0.5f; }
    bool eqUnlocked() const { return value("eqLock") < 0.5f && value("referenceOnly") < 0.5f; }


    const juce::String getName() const override { return "NX Audio MixPilot"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return parameters.getParameter("bypass"); }

    bool isBusesLayoutSupported(const BusesLayout& buses) const override
    {
        const auto channels = buses.getMainOutputChannelSet();
        return (channels == juce::AudioChannelSet::mono() || channels == juce::AudioChannelSet::stereo())
            && channels == buses.getMainInputChannelSet();
    }

    void getStateInformation(juce::MemoryBlock& output) override
    {
        if (auto xml = parameters.copyState().createXml())
            copyXmlToBinary(*xml, output);
    }

    void setStateInformation(const void* data, int bytes) override
    {
        if (auto xml = getXmlFromBinary(data, bytes))
            if (xml->hasTagName(parameters.state.getType()))
                parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }

    void prepareToPlay(double rate, int) override
    {
        sampleRate = static_cast<int>(rate);
        analyzer.prepare(sampleRate);
        equalizer.prepare(rate);
        inputMeter.prepare(rate);
        outputMeter.prepare(rate);
        gain.reset(rate, 0.035);
        outputGain.reset(rate, 0.035);
        outputGain.setCurrentAndTargetValue(1);
        gain.setCurrentAndTargetValue(1);
        settings = -1;
    }

    void releaseResources() override { measurement.write({}, 0); }

    bool supportsDoublePrecisionProcessing() const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { process(buffer); }
    void processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override { process(buffer); }

    template <typename Sample>
    void process(juce::AudioBuffer<Sample>& buffer)
    {
        juce::ScopedNoDenormals noDenormals;
        if (buffer.getNumChannels() == 0)
            return;
        const auto request = learningRequest.load();
        if (request != learningId)
        {
            learningId = request;
            learningHistogram.fill(0);
            energyHistogram.fill(0);
            learnedCeiling = learnedSampleMax = nx::floorDb;
        }
        if (holdReset.exchange(false))
        {
            inputMeter.resetHold();
            outputMeter.resetHold();
        }
        const auto sessionId = session();
        const auto isHub = hub();
        const int newSettings = sessionId * 2 + static_cast<int>(isHub);
        if (settings != newSettings)
        {
            analyzer.reset();
            measurement.write({}, 0);
            settings = newSettings;
        }

        const auto audition = previewSession.load() == sessionId && volumeUnlocked() ? preview.load() : noPreview;
        const bool bypass = value("bypass") >= 0.5f;
        const float trim = isHub || bypass ? 0.0f : audition == noPreview ? value("trim") : audition;
        gain.setTargetValue(juce::Decibels::decibelsToGain(static_cast<double>(trim)));
        outputGain.setTargetValue(juce::Decibels::decibelsToGain(static_cast<double>(value("outputTrim"))));

        std::int64_t hostPosition = -1;
        if (auto* playhead = getPlayHead())
            if (auto position = playhead->getPosition(); position && position->getIsPlaying())
                if (auto samples = position->getTimeInSamples(); samples && *samples >= 0)
                    hostPosition = *samples;

        if (!toneBatch.load())
        {
            auto targetTone = tone();
            if (tonalBus() || value("toneBypass") > 0.5f)
                targetTone = {};
            else
            {
                auto previewTone = targetTone;
                if (eqUnlocked() && tonalPreview.read(previewTone, sessionId))
                    targetTone = previewTone;
            }
            equalizer.update(targetTone);
        }

        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() == 2 ? buffer.getWritePointer(1) : nullptr;
        nx::Reading reading;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto clean = [](Sample value) { return std::isfinite(value) ? value : Sample{}; };
            const auto dryLeft = clean(left[sample]);
            const auto dryRight = right ? clean(right[sample]) : dryLeft;
            const auto ramp = gain.getNextValue() * outputGain.getNextValue();
            const auto applied = bypass ? 1.0 : ramp;
            double equalizedLeft = dryLeft, equalizedRight = dryRight;
            equalizer.process(equalizedLeft, equalizedRight);
            const auto wetLeft = clean(bypass ? dryLeft : static_cast<Sample>(equalizedLeft * applied));
            const auto wetRight = clean(bypass ? dryRight : static_cast<Sample>(equalizedRight * applied));
            left[sample] = wetLeft;
            if (right)
                right[sample] = wetRight;
            const auto bounded = [](Sample value)
            {
                return static_cast<float>(std::clamp(static_cast<double>(value), -1000000.0, 1000000.0));
            };
            inputMeter.push(bounded(dryLeft), bounded(dryRight));
            outputMeter.push(bounded(wetLeft), bounded(wetRight));
            const auto position = hostPosition < 0 ? -1 : hostPosition + sample;
            spectrum.push({bounded(dryLeft), bounded(dryRight), bounded(wetLeft), bounded(wetRight),
                           position, sampleRate, sessionId});
            if (analyzer.push(bounded(dryLeft), bounded(dryRight), bounded(wetLeft), bounded(wetRight), position, reading, false))
            {
                reading.inputLevels = inputMeter.read();
                reading.outputLevels = outputMeter.read();
                if (learningId != 0)
                {
                    const float measured = nx::db(*std::max_element(reading.inputLevels.peak.begin(), reading.inputLevels.peak.end()));
                    const float truePeak = nx::db(*std::max_element(reading.inputLevels.truePeak.begin(), reading.inputLevels.truePeak.end()));
                    if (measured > -60)
                        ++learningHistogram[static_cast<std::size_t>(std::clamp(static_cast<int>(std::ceil(measured)) + 60, 0, 84))];
                    if (reading.activity >= 0.08f && reading.activeRms > -55.0f)
                        ++energyHistogram[static_cast<std::size_t>(std::clamp(static_cast<int>(std::ceil(reading.activeRms)) + 60, 0, 84))];
                    learnedSampleMax = std::max(learnedSampleMax, measured);
                    learnedCeiling = std::max(learnedCeiling, truePeak);
                    reading.learningId = learningId;
                    reading.learnedCeiling = learnedCeiling;
                    reading.learnedMaxSample = learnedSampleMax;
                    unsigned total = 0;
                    for (auto count : learningHistogram)
                        total += count;
                    const auto percentile = 0.85f + 0.15f * learningSensitivity.load();
                    unsigned cumulative = 0;
                    for (std::size_t bin = 0; total != 0 && bin < learningHistogram.size(); ++bin)
                    {
                        cumulative += learningHistogram[bin];
                        if (cumulative >= std::ceil(total * percentile))
                        {
                            reading.learnedPeak = percentile >= 1.0f ? learnedSampleMax : static_cast<float>(bin) - 60.0f;
                            break;
                        }
                    }
                }
                if (learningId != 0)
                {
                    for (auto count : energyHistogram)
                        reading.learnedActiveWindows += count;
                    unsigned accumulated = 0;
                    const auto median = (reading.learnedActiveWindows + 1) / 2;
                    const auto loud = (reading.learnedActiveWindows * 85 + 99) / 100;
                    for (std::size_t bin = 0; median > 0 && bin < energyHistogram.size(); ++bin)
                    {
                        accumulated += energyHistogram[bin];
                        if (accumulated >= median && reading.learnedEnergy == nx::floorDb)
                            reading.learnedEnergy = static_cast<float>(bin) - 60.0f;
                        if (accumulated >= loud)
                        {
                            reading.learnedLoudEnergy = static_cast<float>(bin) - 60.0f;
                            break;
                        }
                    }
                }
                measurement.write(reading, sessionId);
            }
        }
    }

    juce::AudioProcessorValueTreeState parameters;
    Measurement measurement;
    nx::SpectralWorker spectrum;
    TonePreview tonalPreview;
    std::vector<std::vector<TonalChange>> tonalHistory, tonalRedo; // Message thread.
    std::atomic<bool> toneBatch {false};
    std::atomic<unsigned> learningRequest {0};
    std::atomic<float> learningSensitivity {1};
    std::atomic<bool> holdReset {false};
    std::atomic<float> preview {noPreview};
    std::atomic<int> previewSession {0};
    unsigned identity = 0;
    int slot = -1;
    std::optional<float> lastAppliedTrim; // Message thread.
    std::vector<std::vector<Change>> history, redoHistory; // Message thread.
    unsigned previewOwner = 0; // Accessed under the registry mutex.

private:
    std::atomic<int> inferredInstrument {0};
    std::atomic<bool> namedBus {false};
    nx::TonalEqualizer equalizer;
    juce::String hostTrackName; // VST3 channel-context callbacks and the editor use the message thread.
    nx::Analyzer analyzer;
    nx::GainMeter inputMeter, outputMeter;
    juce::SmoothedValue<double> gain {1.0}, outputGain {1.0};
    std::array<unsigned, 85> learningHistogram {}, energyHistogram {};
    unsigned learningId = 0;
    float learnedCeiling = nx::floorDb, learnedSampleMax = nx::floorDb;
    int sampleRate = 48000, settings = -1;
};

struct TrackRow
{
    unsigned identity;
    int group;
    nx::Role role;
    float trim;
    bool current;
    nx::Reading reading;
    nx::GainSuggestion proposed;
    juce::uint32 timestamp = 0;
    juce::String name;
    bool adjusted = false;
    int peakProfile = 0;
    int instrument = 0;
    float customPeak = 1.0f, outputGainDb = 0, toneBoost = 0, instrumentCeiling = 1;
    nx::SpectrumFrame spectrum;
    bool spectralCurrent = false;
    bool tonalBus = false, volumeLocked = false, eqLocked = false, referenceOnly = false;
    int priority = 50;
    nx::Function function = nx::Function::automatic;
    nx::SmartGainResult diagnostics;
};

Processor* findTrack(unsigned identity)
{
    for (auto* track : registry().tracks)
        if (track != nullptr && track->identity == identity)
            return track;
    return nullptr;
}

juce::String number(float value)
{
    return juce::String(value, 1);
}

class Editor final : public juce::Component, private juce::Timer
{
public:
    Editor(Processor& processor, std::function<void(int)> resizeEditor)
        : plugin(processor), requestEditorHeight(std::move(resizeEditor))
    {
        setLookAndFeel(&glass);
        using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        auto attachChoice = [&](juce::ComboBox& control, const char* parameter, juce::StringArray labels)
        {
            control.addItemList(labels, 1);
            addAndMakeVisible(control);
            choices.push_back(std::make_unique<ComboAttachment>(plugin.parameters, parameter, control));
        };
        auto attachSlider = [&](juce::Slider& control, const char* parameter, const char* unit)
        {
            addAndMakeVisible(control);
            control.setSliderStyle(juce::Slider::LinearHorizontal);
            control.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 24);
            control.setTextValueSuffix(unit);
            sliders.push_back(std::make_unique<SliderAttachment>(plugin.parameters, parameter, control));
        };
        attachChoice(instance, "instance", {"Track", "HUB"});
        attachChoice(role, "role", {"Auto", "Foco", "Baixo", "Bateria", "Acompanhamento"});
        attachChoice(peakReference, "peakReference", Processor::peakNames());
        attachChoice(support, "support", Processor::supportNames());
        attachChoice(operation, "operation", {utf8("Calibração técnica"), utf8("Balanço musical")});
        attachSlider(session, "session", "");
        attachSlider(group, "group", "");
        attachSlider(trim, "trim", " dB");
        attachSlider(reference, "reference", " dBFS");
        attachSlider(limit, "limit", " dB");
        attachSlider(trackLimit, "limit", " dB");
        attachSlider(outputTrim, "outputTrim", " dB");
        attachSlider(targetPeak, "targetPeak", " dBFS");
        attachSlider(learnTime, "learnTime", " s");
        attachSlider(sensitivity, "sensitivity", "");
        attachSlider(trueCeiling, "trueCeiling", " dBTP");
        targetPeak.textFromValueFunction = [](double v)
        {
            return v > 0 ? juce::String("Preset") : juce::String(v, 1);
        };
        addAndMakeVisible(category);
        category.addItem("Todas as categorias", 1);
        for (std::size_t i = 0; i < std::size(nx::instrumentCategories); ++i)
            category.addItem(utf8(nx::instrumentCategories[i]), static_cast<int>(i) + 2);
        category.setSelectedId(1);
        category.onChange = [this] { populateInstruments(); };
        addAndMakeVisible(instruments);
        instruments.onChange = [this]
        {
            if (instruments.getSelectedId() != 0)
                plugin.setParameter("instrument", static_cast<float>(instruments.getSelectedId() - 1));
        };
        populateInstruments();
        attachSlider(headroom, "headroom", " dB");
        for (auto* knob : {&limit, &trim})
        {
            knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 24);
        }
        limit.setTitle("Limite de trim");
        trim.setTitle("Trim interno");

        scope.addItemList({"Uma pista", "Um grupo", "Todas as pistas"}, 1);
        scope.setSelectedId(1);
        addAndMakeVisible(scope);
        addAndMakeVisible(pick);
        pick.onChange = [this]
        {
            const auto chosen = std::find_if(rows.begin(), rows.end(), [this](const auto& row)
                { return row.identity == static_cast<unsigned>(pick.getSelectedId()); });
            if (chosen != rows.end())
            {
                const auto index = static_cast<int>(chosen - rows.begin());
                if (index < firstRow || index >= firstRow + visibleRows)
                    firstRow = std::max(0, index - visibleRows + 1);
            }
            repaint();
        };
        for (int i = 0; i < visibleRows; ++i)
        {
            auto& button = principalButtons[static_cast<std::size_t>(i)];
            button.setClickingTogglesState(false);
            button.onClick = [this, i] { choosePrincipal(i); };
            button.setBounds(650, 447 + i * 30, 27, 24);
            addAndMakeVisible(button);
        }
        spectrumMode.addItemList({"Mix Bus", "Solo", "Multi", "Compare", "Reference", "Before/After", "Clear"}, 1);
        spectrumMode.setSelectedId(1);
        spectrumMode.onChange = [this]
        {
            if (spectrumMode.getSelectedId() == 7)
                displayedSpectra.clear();
            repaint();
        };
        addAndMakeVisible(spectrumMode);
        fftChoice.addItemList({"4096", "8192", "16384"}, 1);
        fftChoice.setSelectedId(1);
        fftChoice.onChange = [this] { configureSpectrum(); };
        addAndMakeVisible(fftChoice);
        for (int i = 0; i < visibleRows; ++i)
        {
            auto& checkbox = spectrumButtons[static_cast<std::size_t>(i)];
            checkbox.setClickingTogglesState(false);
            checkbox.setBounds(215, 447 + i * 30, 24, 24);
            checkbox.onClick = [this, i] { selectSpectrum(firstRow + i, true); };
            addAndMakeVisible(checkbox);
            auto& target = tonalButtons[static_cast<std::size_t>(i)];
            target.setClickingTogglesState(false);
            target.setBounds(380, 447 + i * 30, 24, 24);
            target.onClick = [this, i]
            {
                const auto index = static_cast<std::size_t>(firstRow + i);
                if (!tonalPanel.isVisible() || index >= rows.size() || (rows[index].tonalBus || rows[index].eqLocked || rows[index].referenceOnly))
                    return;
                cancelTonal();
                const auto id = rows[index].identity;
                if (!tonalTargets.erase(id))
                    tonalTargets.insert(id);
                updatePrincipalButtons();
                updateTonalPanel();
            };
            addChildComponent(target);
        }
        configureButton(normalise, "Norm", {});
        normalise.setClickingTogglesState(true);
        normalise.setTooltip(utf8("Normalizar a soma de energia apenas na visualização"));
        configureButton(spectralFreeze, "Freeze", [this]
        {
            if (spectralFreeze.getToggleState())
            {
                frozenRows = rows;
                frozenSpectrum = localSpectrum;
                frozenCurrent = spectralCurrent;
            }
            else
                frozenRows.clear();
        });
        spectralFreeze.setClickingTogglesState(true);
        spectralFreeze.setTooltip("Congelar apenas o espectro");
        configureButton(spectrumOptions, "Vista", [this] { showSpectrumOptions(); });
        configureButton(advancedToggle, utf8("Avançado"), [this]
        {
            advanced = !advanced;
            advancedToggle.setButtonText(advanced ? "Essencial" : utf8("Avançado"));
            updateLayoutMode();
            requestEditorHeight(getHeight());
        });
        configureButton(smartToggle, "Assistente", [this]
        {
            if (smartPanel.isVisible() && listening)
                togglePreview();
            clearTonePreview();
            tonalPanel.setVisible(false);
            smartPanel.setVisible(!smartPanel.isVisible());
            if (smartPanel.isVisible())
                smartPanel.toFront(false);
            updatePrincipalButtons();
            updateSmartPanel();
        });
        addChildComponent(smartPanel);
        smartPanel.mode.onChange = [this]
        {
            const int selection = smartPanel.mode.getSelectedId();
            if (selection <= 0)
                return;
            if (selection <= 2)
                plugin.setParameter("operation", selection == 2 ? 1.0f : 0.0f);
            plugin.setParameter("smartStrategy", static_cast<float>(std::max(0, selection - 2)));
            cancelSmart();
            timerCallback();
        };
        auto slider = [&](juce::Slider& field, const char* id)
        {
            sliders.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(plugin.parameters, id, field));
        };
        slider(smartPanel.priority, "priority");
        slider(smartPanel.busTarget, "premasterPeak");
        choices.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            plugin.parameters, "function", smartPanel.function));
        auto lock = [&](juce::Button& control, const char* id)
        {
            smartAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
                plugin.parameters, id, control));
        };
        lock(smartPanel.volumeLock, "volumeLock");
        lock(smartPanel.eqLock, "eqLock");
        lock(smartPanel.referenceLock, "referenceOnly");
        configureButton(tonalToggle, "Tonal Match", [this]
        {
            if (smartPanel.isVisible() && listening)
                togglePreview();
            smartPanel.setVisible(false);
            if (tonalPanel.isVisible())
                clearTonePreview();
            tonalPanel.setVisible(!tonalPanel.isVisible());
            if (tonalPanel.isVisible())
                tonalPanel.toFront(false);
            updatePrincipalButtons();
            updateTonalPanel();
        });
        addChildComponent(tonalPanel);
        tonalPanel.correction.onChange = [this] { loadToneBand(); };
        for (auto* control : {&tonalPanel.frequency, &tonalPanel.gain, &tonalPanel.q})
            control->onValueChange = [this] { editToneBand(); };
        tonalPanel.compensation.onClick = [this] { updateCompensation(); };
        tonalPanel.limit.onValueChange = [this]
        {
            for (auto& item : tonalPending)
                nx::limitCorrection(item.after.bands, tonalPanel.limit.getValue());
            loadToneBand();
            updateCompensation();
        };
        const auto stored = plugin.parameters.state.getProperty("tonalReference").toString();
        juce::StringArray values;
        values.addTokens(stored, " ", "");
        if (values.size() == nx::tonalBands)
            for (int i = 0; i < nx::tonalBands; ++i)
                tonalReference[i] = juce::jlimit(0.0f, 1.0e6f, values[i].getFloatValue());

        configureButton(analyze, "AUTO GAIN", [this] { startAnalysis(); });
        analyze.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff14627a));
        analyze.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff167243));
        analyze.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        analyze.setTooltip(utf8("Analisa e aplica o trim individual de cada pista ativa da sessão."));
        configureButton(review, utf8("Prévia de ganho"), [this] { capture(); });
        configureButton(apply, "Aplicar trim", [this] { commit(); });
        apply.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff135f72));
        configureButton(audition, "Ouvir B", [this] { togglePreview(); });
        configureButton(undo, "Desfazer", [this] { undoLast(); });
        configureButton(cancel, "Cancelar", [this]
        {
            const bool wasAnalyzing = analyzing;
            if (wasAnalyzing)
                assignLearning(0);
            analyzing = false;
            discard(wasAnalyzing ? utf8("Análise cancelada.") : utf8("Prévia cancelada."));
            updateButtons();
        });
        configureButton(analysisOnly, utf8("Somente análise"), [this]
        {
            if (analysisOnly.getToggleState())
            {
                if (analyzing)
                    assignLearning(0);
                analyzing = false;
                discard(utf8("Somente análise ativa. O áudio mantém os trims aplicados."));
            }
            else
                status = "Ajustes de ganho liberados.";
            updateButtons();
            repaint();
        });
        analysisOnly.setClickingTogglesState(true);
        analysisOnly.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff126476));
        configureButton(manualAnalyze, "ANALYZE", [this] { capture(); });
        configureButton(autoGain, "ANALYZER", [this] { startAnalysis(); });
        configureButton(resetHold, "RESET", [this]
        {
            plugin.holdReset.store(true);
            status = utf8("Peak hold reiniciado nesta instância.");
        });
        freeze.setButtonText("FREEZE");
        addAndMakeVisible(freeze);
        freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            plugin.parameters, "freeze", freeze);
        tonalExclude.setButtonText("Excluir do EQ");
        addAndMakeVisible(tonalExclude);
        tonalExcludeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            plugin.parameters, "toneExclude", tonalExclude);
        bypass.setButtonText("Bypass");
        addAndMakeVisible(bypass);
        bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            plugin.parameters, "bypass", bypass);

        smartPanel.toneModule.setToggleState(true, juce::dontSendNotification);
        updateLayoutMode();
        timerCallback();
        startTimerHz(12);
    }

    ~Editor() override
    {
        stopTimer();
        clearTonePreview();
        if (analyzing)
            assignLearning(0);
        setLookAndFeel(nullptr);
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearPreview();
    }

    void resized() override
    {
        instance.setBounds(853, 16, 94, 30);
        bypass.setBounds(954, 16, 82, 30);
        session.setBounds(72, 75, 122, 27);
        operation.setBounds(207, 75, 177, 27);
        reference.setBounds(474, 75, 119, 27);
        headroom.setBounds(671, 75, 118, 27);
        trackLimit.setBounds(530, 113, 126, 27);
        scope.setBounds(16, 114, 142, 26);
        pick.setBounds(168, 114, 146, 26);
        group.setBounds(plugin.hub() ? 222 : 459, plugin.hub() ? 114 : 75, 125, 26);
        role.setBounds(267, 75, 122, 27);
        support.setBounds(27, 574, 270, 28);
        peakReference.setBounds(323, 574, 352, 28);
        trim.setBounds(807, 494, 128, 130);
        limit.setBounds(trim.getBounds());

        spectrumMode.setBounds(294, 159, 115, 25);
        fftChoice.setBounds(414, 159, 73, 25);
        normalise.setBounds(491, 159, 53, 25);
        spectralFreeze.setBounds(547, 159, 62, 25);
        spectrumOptions.setBounds(612, 159, 70, 25);
        tonalToggle.setBounds(865, 114, 162, 27);
        advancedToggle.setBounds(670, 16, 104, 30);
        smartToggle.setBounds(813, 75, 215, 27);
        smartPanel.setBounds(703, advanced ? 404 : 255, 336, 447);
        tonalExclude.setBounds(724, 114, 135, 27);
        tonalPanel.setBounds(703, advanced ? 404 : 255, 336, 447);
        category.setBounds(17, 745, 190, 27);
        instruments.setBounds(214, 745, 220, 27);
        targetPeak.setBounds(447, 745, 122, 27);
        outputTrim.setBounds(581, 745, 123, 27);
        learnTime.setBounds(719, 745, 139, 27);
        sensitivity.setBounds(869, 745, 154, 27);
        trueCeiling.setBounds(116, 803, 134, 26);
        manualAnalyze.setBounds(273, 802, 112, 28);
        autoGain.setBounds(396, 802, 119, 28);
        freeze.setBounds(529, 802, 90, 28);
        resetHold.setBounds(629, 802, 96, 28);
        analyze.setBounds(16, 652, 127, 34);
        review.setBounds(153, 652, 146, 34);
        audition.setBounds(309, 652, 116, 34);
        cancel.setBounds(435, 652, 99, 34);
        analysisOnly.setBounds(551, 652, 181, 34);
        undo.setBounds(753, 652, 119, 34);
        apply.setBounds(882, 652, 152, 34);
    }

    void updateLayoutMode()
    {
        setSize(1050, advanced ? 966 : 710);
        for (auto* control : std::initializer_list<juce::Component*>{
                 &category, &instruments, &targetPeak, &outputTrim, &learnTime, &sensitivity, &trueCeiling,
                 &manualAnalyze, &autoGain, &freeze, &resetHold})
            control->setVisible(advanced);
        resized();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (plotBounds().contains(event.position))
        {
            const double position = (event.position.x - plotBounds().getX()) / plotBounds().getWidth();
            const double center = graphStart + position * graphSpan;
            graphSpan = std::clamp(graphSpan * std::pow(0.45, wheel.deltaY), 16.0, 96.0);
            graphStart = std::clamp(center - position * graphSpan, 0.0, 96.0 - graphSpan);
            repaint();
            return;
        }
        if (!plugin.hub() || !juce::Rectangle<int>(12, 422, 684, 210).contains(event.getPosition()))
            return;
        firstRow = juce::jlimit(0, std::max(0, static_cast<int>(rows.size()) - visibleRows),
                               firstRow + (wheel.deltaY > 0 ? -1 : wheel.deltaY < 0 ? 1 : 0));
        updatePrincipalButtons();
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!plugin.hub() || !juce::Rectangle<int>(20, 445, 667, 180).contains(event.getPosition()))
            return;
        const auto index = firstRow + (event.y - 445) / 30;
        if (index < static_cast<int>(rows.size()))
            selectSpectrum(index, event.mods.isCtrlDown() || event.mods.isCommandDown());
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        graphCursor = plotBounds().contains(event.position) ? event.position : juce::Point<float>{-1, -1};
        repaint(12, 150, 684, 244);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        graphCursor = {-1, -1};
        repaint(12, 150, 684, 244);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace juce;
        const bool hubMode = plugin.hub();
        g.fillAll(Colour(0xff080e16));
        ColourGradient ambient(Colour(0xff203e55), 172, 60, Colour(0xff080b12), 798, 643, true);
        ambient.addColour(0.43, Colour(0xff122333));
        g.setGradientFill(ambient);
        g.fillAll();
        panel(g, 5, 5, 1040, 55);
        label(g, "NX", 21, 10, 52, 40, 34, cyan, true);
        label(g, "AUDIO", 77, 15, 73, 32, 17, ink, true);
        g.setColour(Colour(0xff476777));
        g.drawVerticalLine(157, 17, 44);
        label(g, "MixPilot", 175, 10, 122, 40, 28, ink, true);
        badge(g, hubMode ? "HUB" : "TRACK", 307, 19, 66, cyan);
        const auto active = std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.current && row.reading.active(); });
        const auto subtitle = hubMode ? juce::String(active) + "/" + juce::String(rows.size()) + " pistas com sinal"
                                      : plugin.displayName();
        label(g, subtitle, 405, 17, 255, 30, 14, ink);
        label(g, utf8("Instância"), 782, 18, 66, 28);

        panel(g, 12, 68, 1026, 41);
        label(g, utf8("Sessão"), 22, 75, 48, 27, 13);
        if (hubMode)
        {
            label(g, mode() == nx::GainMode::calibration ? "0 VU RMS" : utf8("Âncora: pista de Foco"), 400, 75, 190, 27, 13);
            label(g, "Headroom", 600, 75, 68, 27, 13);
            label(g, utf8("Escopo da prévia"), 362, 114, 160, 26, 13);
            if (scope.getSelectedId() == 2)
                label(g, "Grupo", 172, 114, 48, 26, 13);
        }
        else
        {
            label(g, "Papel", 216, 75, 50, 27, 13);
            label(g, "Grupo", 409, 75, 50, 27, 13);
            label(g, "0 VU RMS", 22, 113, 70, 27, 13);
            label(g, "Headroom", 229, 113, 69, 27, 13);
            label(g, "Limite de trim", 433, 113, 95, 27, 13);

        }

        panel(g, 12, 150, 684, 244);
        panel(g, 707, 150, 331, 244);
        label(g, hubMode ? utf8("ESPECTRO DA SESSÃO") : "ESPECTRO DA PISTA", 26, 159, 300, 28, 17, ink, true);
        label(g, hubMode ? "MIX BUS" : utf8("MEDIÇÃO DA PISTA"), 721, 159, 297, 28, 17, ink, true);
        drawSpectrum(g);
        drawMeters(g);

        panel(g, 12, 404, 684, 231);
        panel(g, 707, 404, 331, 73);
        panel(g, 707, 485, 331, 150);
        if (hubMode)
        {
            label(g, utf8("BALANÇO POR PISTA"), 26, 407, 400, 23, 17, ink, true);
            drawRows(g);
            label(g, utf8("DIAGNÓSTICO TONAL"), 721, 411, 297, 19, 14, otherRow >= 0 ? amber : cyan, true);
            const auto detail = otherRow >= 0 ? tonalStatus
                : focusRow >= 0 ? utf8("Aguardando comparação sincronizada.")
                                : "Marque Foco na linha da pista desejada.";
            label(g, detail, 721, 431, 304, 23, 13, ink);
            label(g, otherRow >= 0 ? utf8("Sobreposição candidata · sem EQ aplicado") : utf8("A comparação usa a posição do host."),
                  721, 453, 304, 16, 11);
            label(g, "LIMITE DE TRIM", 721, 494, 100, 20, 12, cyan, true);
        }
        else
        {
            label(g, utf8("BALANÇO LOCAL"), 26, 407, 630, 23, 17, ink, true);
            drawRows(g);
            label(g, utf8("Defina a hierarquia no HUB. O tipo abaixo se aplica a Auto ou Acompanhamento."),
                  28, 526, 641, 25, 14);
            label(g, "Tipo de acompanhamento", 28, 553, 270, 20, 12);
            label(g, utf8("Referência de pico por instrumento"), 324, 553, 350, 20, 12);
            const int profile = static_cast<int>(plugin.value("peakReference"));
            if (profile > 0)
            {
                const auto range = nx::instrumentPeakRanges[static_cast<std::size_t>(profile - 1)];
                label(g, "Alvo: " + number(range.target) + " dBFS pico  /  Faixa: " + number(range.minimum) + " a " + number(range.maximum),
                      325, 605, 348, 19, 12, cyan);
            }
            label(g, utf8("SESSÃO E GRUPO"), 721, 411, 297, 19, 14, cyan, true);
            label(g, utf8("Sessão ") + juce::String(plugin.session()) + "  /  Grupo " + juce::String(plugin.group()),
                  721, 436, 297, 25, 15, ink);
            label(g, "TRIM INTERNO", 721, 494, 95, 20, 12, cyan, true);
        }

        panel(g, 5, 644, 1040, 63);
        if (advanced)
        {
            panel(g, 5, 714, 1040, 134);
            drawStereoDetails(g);
            label(g, utf8("Categoria"), 17, 724, 190, 18, 12);
            label(g, utf8("Instrumento"), 214, 724, 220, 18, 12);
            label(g, "Target de pico", 447, 724, 122, 18, 12);
            label(g, utf8("Ganho de saída"), 581, 724, 123, 18, 12);
            label(g, "Janela", 719, 724, 139, 18, 12);
            label(g, "Sensibilidade", 869, 724, 154, 18, 12);
            label(g, "Limite True Peak", 17, 803, 101, 26, 12);
            const int preset = plugin.instrument();
            if (preset > 0)
                label(g, utf8(nx::presets[preset - 1].name) + "  /  "
                    + number(plugin.value("targetPeak") <= 0 ? plugin.value("targetPeak") : nx::presets[preset - 1].target)
                    + " dBFS pico" + (plugin.instrumentCeiling() <= 0
                        ? utf8("  /  Teto: ") + number(plugin.instrumentCeiling()) + " dBFS" : juce::String{}),
                      17, 775, 670, 19, 12, cyan);
        }

        label(g, status, 17, 685, 1016, 18, 13, analyzing ? 0xff63f69e : ink);
    }

private:
    nx::GainStrategy strategy() const
    {
        const auto value = plugin.value("smartStrategy");
        if (value >= 1.5f)
            return nx::GainStrategy::hybrid;
        if (value >= 0.5f)
            return nx::GainStrategy::preserve;
        return mode() == nx::GainMode::musical ? nx::GainStrategy::musical : nx::GainStrategy::technical;
    }

    bool needsAnchor() const
    {
        const auto selection = strategy();
        return plugin.hub() && (selection == nx::GainStrategy::musical || selection == nx::GainStrategy::hybrid);
    }

    nx::GainMode mode() const
    {
        return !plugin.hub() || plugin.value("operation") < 0.5f ? nx::GainMode::calibration : nx::GainMode::musical;
    }

    static void label(juce::Graphics& g, const juce::String& text, int x, int y, int w, int h,
                      float size = 14, juce::uint32 colour = muted, bool strong = false)
    {
        g.setColour(juce::Colour(colour));
        g.setFont(juce::Font(juce::FontOptions(size, strong ? juce::Font::bold : juce::Font::plain)));
        g.drawFittedText(text, x, y, w, h, juce::Justification::centredLeft, 1);
    }

    static void panel(juce::Graphics& g, int x, int y, int w, int h)
    {
        const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(w), static_cast<float>(h));
        juce::Path shape;
        shape.addRoundedRectangle(area, 13);
        juce::DropShadow(juce::Colour(0x99020409), 6, {0, 3}).drawForPath(g, shape);
        g.setGradientFill({juce::Colour(0xaa22374c), area.getX(), area.getY(),
                           juce::Colour(0xdc080e18), area.getRight(), area.getBottom(), false});
        g.fillPath(shape);
        g.setGradientFill({juce::Colour(0xaf566f87), area.getX(), area.getY(),
                           juce::Colour(0x94314354), area.getX(), area.getBottom(), false});
        g.drawRoundedRectangle(area.reduced(0.5f), 13, 1);
    }

    static void badge(juce::Graphics& g, const juce::String& caption, int x, int y,
                      int width, juce::uint32 tint)
    {
        const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(width), 22.0f);
        const juce::Colour color(tint);
        g.setColour(color.withAlpha(0.12f));
        g.fillRoundedRectangle(area, 5);
        g.setColour(color.withAlpha(0.65f));
        g.drawRoundedRectangle(area, 5, 1);
        label(g, caption, x + 7, y, width - 14, 22, 12, tint, true);
    }

    bool localSignal() const
    {
        return localCurrent && local.peak > -90.0f;
    }

    static void drawSegments(juce::Graphics& g, juce::Rectangle<float> area, float level,
                             int steps, bool vertical)
    {
        const auto dimension = vertical ? area.getHeight() : area.getWidth();
        const auto increment = dimension / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i)
        {
            const float threshold = -48.0f + 48.0f * (i + 0.5f) / steps;
            const auto color = threshold >= -3.0f ? juce::Colour(0xffff8551)
                : threshold >= -12.0f ? juce::Colour(0xffffd24c)
                                     : juce::Colour(0xff43ef8b).interpolatedWith(juce::Colour(0xff15a8a9),
                                                                                std::max(0.0f, -threshold / 48.0f));
            const auto segment = vertical ? juce::Rectangle<float>(area.getX(), area.getBottom() - increment * (i + 1),
                                                                     area.getWidth(), increment - 1.4f)
                : juce::Rectangle<float>(area.getX() + increment * i, area.getY(),
                                           increment - 1.3f, area.getHeight());
            g.setColour(level >= threshold ? color : juce::Colour(0xff243241));
            g.fillRoundedRectangle(segment, 1.0f);
        }
    }

    void drawStereoDetails(juce::Graphics& g) const
    {
        panel(g, 5, 852, 1040, 108);
        label(g, utf8("MEDIDORES ESTÉREO"), 17, 859, 185, 20, 13, ink, true);
        label(g, utf8("Trim de entrada: ") + number(plugin.hub() ? 0.0f : plugin.value("trim")) + " dB",
              807, 859, 205, 20, 12);
        const auto reading = [&](float value, float reference = 0.0f)
        {
            return localCurrent && value > 0.000001f ? number(nx::db(value) - reference) : utf8("—");
        };
        const std::array<int, 5> columns {211, 323, 435, 547, 659};
        const std::array<juce::String, 5> captions {"Peak dBFS", "True peak dBTP", "RMS dBFS", "VU", "Hold dBTP"};
        for (std::size_t i = 0; i < columns.size(); ++i)
            label(g, captions[i], columns[i], 860, 108, 18, 12);
        auto render = [&](const nx::StereoLevels& meter, int channel, int y, const char* name)
        {
            label(g, utf8(name), 17, y, 95, 16, 12);
            drawSegments(g, {115.0f, static_cast<float>(y + 5), 80, 7},
                         localCurrent ? nx::db(meter.peak[channel]) : nx::floorDb, 12, false);
            const std::array<juce::String, 5> values {
                reading(meter.peak[channel]), reading(meter.truePeak[channel]), reading(meter.rms[channel]),
                reading(meter.vu[channel], plugin.value("reference")), reading(meter.hold[channel])
            };
            for (std::size_t i = 0; i < columns.size(); ++i)
                label(g, values[i], columns[i], y, 100, 16, 12, ink);
        };
        render(local.inputLevels, 0, 880, "Entrada L");
        render(local.inputLevels, 1, 898, "Entrada R");
        render(local.outputLevels, 0, 916, "Saída L");
        render(local.outputLevels, 1, 934, "Saída R");
        const auto peak = *std::max_element(local.outputLevels.hold.begin(), local.outputLevels.hold.end());
        if (localCurrent && peak >= 1.0f)
            badge(g, "CLIP", 808, 895, 58, 0xffff725d);
        else if (localCurrent && nx::db(peak) > plugin.value("trueCeiling"))
            badge(g, "ACIMA DO LIMITE", 808, 895, 142, amber);
    }

    void drawMeters(juce::Graphics& g) const
    {
        const bool signal = localSignal();
        for (const auto& meter : {juce::Rectangle<float>(754, 192, 27, 155),
                                 juce::Rectangle<float>(805, 192, 27, 155)})
        {
            g.setColour(juce::Colour(0xff091018));
            g.fillRoundedRectangle(meter.expanded(4), 5);
            g.setColour(juce::Colour(0xff344c61));
            g.drawRoundedRectangle(meter.expanded(4), 5, 1);
        }
        drawSegments(g, {754, 192, 27, 155}, signal ? local.peak : nx::floorDb, 28, true);
        drawSegments(g, {805, 192, 27, 155}, signal ? local.outputPeak : nx::floorDb, 28, true);
        for (int i = 0; i <= 4; ++i)
            label(g, juce::String(-12 * i), 720, 185 + i * 39, 25, 14, 11);
        label(g, "Entrada", 747, 355, 56, 22, 11, ink);
        label(g, utf8("Saída"), 804, 355, 48, 22, 11, ink);

        const auto dash = utf8("—");
        auto metric = [&](const juce::String& caption, const juce::String& value, int y)
        {
            panel(g, 855, y, 166, 51);
            label(g, caption, 866, y + 4, 151, 18, 12);
            label(g, value, 866, y + 23, 151, 24, 19, ink, true);
        };
        metric("RMS de entrada", signal ? number(local.rms) + " dBFS" : dash, 190);
        metric(utf8("Pico de amostra · saída"), signal ? number(local.outputPeak) + " dBFS" : dash, 250);
        metric("Headroom", signal ? number(-local.outputPeak) + " dB" : dash, 310);
        if (plugin.hub())
            label(g, utf8("Pré-master · referência: ") + number(plugin.value("premasterPeak")) + " dBFS", 856, 363, 170, 23, 10, cyan);
    }

    static juce::Colour spectrumColour(const TrackRow& row)
    {
        if (row.role == nx::focus)
            return juce::Colour(cyan);
        constexpr std::array<juce::uint32, 10> palette {
            0xffff77b6, 0xffae86ff, 0xff61e896, 0xffffbb60, 0xff8aa7ff,
            0xffef7764, 0xffd7eb6c, 0xffdf8df2, 0xff52c8b3, 0xffffdcad
        };
        const auto index = static_cast<std::size_t>(row.identity - 1);
        const auto shade = juce::Colour(palette[index % palette.size()]);
        return (index / palette.size()) % 2 == 0 ? shade : shade.darker(0.25f);
    }

    static juce::Rectangle<float> plotBounds() { return {62, 194, 612, 159}; }

    void configureSpectrum()
    {
        const int length = std::max(0, fftChoice.getSelectedId() - 1);
        plugin.spectrum.configure(length, averageMs);
        if (plugin.hub())
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            for (auto* track : registry().tracks)
                if (track && !track->hub() && track->session() == plugin.session())
                    track->spectrum.configure(length, averageMs);
        }
    }

    void selectSpectrum(int index, bool multiple)
    {
        if (index < 0 || index >= static_cast<int>(rows.size()))
            return;
        const auto identity = rows[static_cast<std::size_t>(index)].identity;
        if (!multiple)
        {
            displayedSpectra.clear();
            displayedSpectra.insert(identity);
            spectrumMode.setSelectedId(2);
        }
        else
        {
            if (!displayedSpectra.erase(identity))
                displayedSpectra.insert(identity);
            spectrumMode.setSelectedId(3);
        }
        updatePrincipalButtons();
        repaint();
    }

    static nx::Conflict pairConflict(const TrackRow* first, const TrackRow* second,
                                     double low = 20, double high = 20000)
    {
        if (!first || !second || !first->spectralCurrent || !second->spectralCurrent
            || first->spectrum.end < 0 || first->spectrum.end != second->spectrum.end
            || first->spectrum.rate != second->spectrum.rate)
            return {};
        const double weight = 1.0 + std::max(0, first->priority - second->priority) / 100.0;
        return nx::masking(first->spectrum.instant, second->spectrum.instant,
                           first->spectrum.pan, second->spectrum.pan, low, high, weight);
    }

    const TrackRow* rowById(unsigned id) const
    {
        const auto found = std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.identity == id; });
        return found != rows.end() ? &*found : nullptr;
    }

    void setTonalReference(const nx::Spectrum& referencePower)
    {
        tonalReference = referencePower;
        plugin.parameters.state.setProperty("tonalReference", serialiseSpectrum(referencePower), nullptr);
        tonalPanel.genre.setSelectedId(10);
        spectrumMode.setSelectedId(5);
    }

    void importTonalReference()
    {
        tonalChooser = std::make_unique<juce::FileChooser>(utf8("Importar referência tonal"), juce::File{}, "*.csv;*.wav;*.flac;*.mp3");
        tonalChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [self = juce::Component::SafePointer<Editor>(this)](const juce::FileChooser& chooser)
            {
                if (self == nullptr)
                    return;
                const auto file = chooser.getResult();
                if (!file.existsAsFile())
                    return;
                if (!file.hasFileExtension("csv"))
                {
                    self->analyseReferenceFile(file);
                    return;
                }
                if (file.getSize() > 65536)
                    return;
                std::vector<std::pair<double, double>> points;
                for (const auto& line : juce::StringArray::fromLines(file.loadFileAsString()))
                {
                    const auto columns = juce::StringArray::fromTokens(line, ",;", "");
                    if (columns.size() < 2)
                        continue;
                    const auto hz = columns[0].getDoubleValue();
                    const auto dBText = columns[1].trim();
                    const auto dB = dBText.getDoubleValue();
                    if (hz >= 20 && hz <= 20000 && dB >= -140 && dB <= 30
                        && dBText.containsOnly("0123456789.+-") && dBText.containsAnyOf("0123456789"))
                        points.emplace_back(std::log(hz), dB);
                }
                std::sort(points.begin(), points.end());
                points.erase(std::unique(points.begin(), points.end(), [](const auto& a, const auto& b)
                    { return a.first == b.first; }), points.end());
                if (points.size() < 2)
                {
                    self->tonalPanel.status = utf8("CSV precisa de dois ou mais pontos de frequência e dB.");
                    self->tonalPanel.repaint();
                    return;
                }
                nx::Spectrum referencePower {};
                for (int i = 0; i < nx::tonalBands; ++i)
                {
                    const auto query = std::log(nx::bandFrequency(i));
                    const auto upper = std::lower_bound(points.begin(), points.end(), query,
                        [](const auto& point, double hz) { return point.first < hz; });
                    if (upper == points.begin() || upper == points.end())
                        continue;
                    const auto& lower = *(upper - 1);
                    const auto fraction = (query - lower.first) / (upper->first - lower.first);
                    referencePower[i] = static_cast<float>(std::pow(10.0, (lower.second + fraction * (upper->second - lower.second)) / 10.0));
                }
                self->referenceStats = {};
                self->referenceLabel = file.getFileNameWithoutExtension();
                self->setTonalReference(referencePower);
                self->tonalPanel.status = utf8("Referência importada.");
                self->tonalPanel.repaint();
            });
    }

    static juce::String serialiseSpectrum(const nx::Spectrum& referencePower)
    {
        juce::StringArray serialized;
        for (auto power : referencePower)
            serialized.add(juce::String(power, 12));
        return serialized.joinIntoString(" ");
    }

    static nx::Spectrum parseSpectrum(const juce::String& serialised)
    {
        nx::Spectrum power {};
        const auto tokens = juce::StringArray::fromTokens(serialised, " ", "");
        if (tokens.size() != nx::tonalBands)
            return {};
        for (int i = 0; i < nx::tonalBands; ++i)
            power[i] = std::clamp(tokens[i].getFloatValue(), 0.0f, 1.0e6f);
        return power;
    }

    void analyseReferenceFile(juce::File file)
    {
        static juce::ThreadPool referencePool(1);
        const auto serial = ++referenceRequest;
        tonalPanel.status = utf8("Analisando áudio da referência em segundo plano.");
        referencePool.addJob([self = juce::Component::SafePointer<Editor>(this), file, serial]
        {
            const auto result = nx::loadReferenceAudio(file);
            juce::MessageManager::callAsync([self, file, serial, result]
            {
                if (self == nullptr || self->referenceRequest != serial)
                    return;
                if (!result.valid)
                    self->tonalPanel.status = utf8("Áudio inválido, silencioso ou com mais de dois canais.");
                else
                {
                    self->referenceStats = result;
                    self->referenceLabel = file.getFileNameWithoutExtension();
                    self->setTonalReference(result.curve);
                    self->tonalPanel.status = utf8("Referência de áudio pronta. Use Biblioteca para nomear e classificar.");
                }
                self->updateTonalPanel();
            });
        });
    }

    juce::ValueTree referenceLibrary() const
    {
        return plugin.parameters.state.getChildWithName("ReferenceLibrary");
    }

    void saveReferenceDialog()
    {
        if (nx::spectrumEnergy(tonalReference) <= 1.0e-10)
            return;
        auto* dialog = new juce::AlertWindow(utf8("Salvar referência"), "Nome, tipo e perfil",
                                             juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor("name", referenceLabel.isNotEmpty() ? referenceLabel : utf8("Minha referência"), "Nome");
        dialog->addComboBox("type", {"Mix", "Master", "Instrumento"}, "Tipo");
        juce::StringArray genres;
        for (const auto& profile : nx::profiles)
            genres.add(utf8(profile.name));
        dialog->addComboBox("genre", genres, utf8("Gênero"));
        dialog->getComboBoxComponent("type")->setSelectedItemIndex(referenceType, juce::dontSendNotification);
        dialog->getComboBoxComponent("genre")->setSelectedId(tonalPanel.genre.getSelectedId(), juce::dontSendNotification);
        dialog->addButton("Salvar", 1);
        dialog->addButton("Cancelar", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog->enterModalState(true, juce::ModalCallbackFunction::create(
            [self = juce::Component::SafePointer<Editor>(this), dialog](int code)
            {
                if (self == nullptr || code != 1)
                    return;
                const auto name = dialog->getTextEditorContents("name").trim().substring(0, 80);
                if (name.isEmpty())
                    return;
                auto library = self->referenceLibrary();
                if (!library.isValid())
                {
                    library = juce::ValueTree("ReferenceLibrary");
                    self->plugin.parameters.state.addChild(library, -1, nullptr);
                }
                if (library.getNumChildren() >= 128)
                {
                    self->tonalPanel.status = "Biblioteca cheia. Remova uma entrada antes de gravar.";
                    self->tonalPanel.repaint();
                    return;
                }
                juce::ValueTree entry("Reference");
                entry.setProperty("name", name, nullptr);
                entry.setProperty("type", dialog->getComboBoxComponent("type")->getSelectedItemIndex(), nullptr);
                entry.setProperty("genre", dialog->getComboBoxComponent("genre")->getSelectedItemIndex(), nullptr);
                entry.setProperty("curve", serialiseSpectrum(self->tonalReference), nullptr);
                entry.setProperty("metrics", self->referenceStats.valid, nullptr);
                entry.setProperty("loudness", self->referenceStats.loudness, nullptr);
                entry.setProperty("rms", self->referenceStats.rms, nullptr);
                entry.setProperty("peak", self->referenceStats.peak, nullptr);
                entry.setProperty("crest", self->referenceStats.crest, nullptr);
                entry.setProperty("balance", self->referenceStats.balance, nullptr);
                entry.setProperty("correlation", self->referenceStats.correlation, nullptr);
                entry.setProperty("duration", self->referenceStats.duration, nullptr);
                library.addChild(entry, -1, nullptr);
                self->referenceLabel = name;
                self->tonalPanel.status = utf8("Referência salva na sessão.");
                self->updateTonalPanel();
            }), true);
    }

    void showReferenceLibrary()
    {
        juce::PopupMenu menu, open, merge, remove;
        auto library = referenceLibrary();
        menu.addItem(1, utf8("Salvar referência atual…"), nx::spectrumEnergy(tonalReference) > 1.0e-10);
        const std::array<juce::String, 3> types {"Mix", "Master", "Instrumento"};
        for (int i = 0; i < library.getNumChildren(); ++i)
        {
            const auto entry = library.getChild(i);
            const auto type = std::clamp(static_cast<int>(entry.getProperty("type")), 0, 2);
            const auto genre = std::clamp(static_cast<int>(entry.getProperty("genre")), 0, 9);
            const auto label = types[type] + " / " + utf8(nx::profiles[genre].name) + " / " + entry["name"].toString();
            open.addItem(100 + i, label);
            merge.addItem(300 + i, label);
            remove.addItem(500 + i, label);
        }
        menu.addSubMenu("Carregar", open, library.getNumChildren() > 0);
        menu.addSubMenu("Combinar com atual", merge, library.getNumChildren() > 0 && nx::spectrumEnergy(tonalReference) > 1.0e-10);
        menu.addSubMenu("Remover da biblioteca", remove, library.getNumChildren() > 0);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tonalPanel),
            [self = juce::Component::SafePointer<Editor>(this), library](int choice) mutable
            {
                if (self == nullptr || choice == 0)
                    return;
                if (choice == 1)
                {
                    self->saveReferenceDialog();
                    return;
                }
                const int base = choice >= 500 ? 500 : choice >= 300 ? 300 : 100;
                const auto entry = library.getChild(choice - base);
                if (!entry.isValid())
                    return;
                if (base == 500)
                {
                    library.removeChild(entry, nullptr);
                    self->tonalPanel.status = utf8("Referência removida da biblioteca.");
                }
                else
                {
                    auto curve = parseSpectrum(entry["curve"].toString());
                    self->referenceStats = {};
                    self->referenceType = std::clamp(static_cast<int>(entry.getProperty("type")), 0, 2);
                    self->referenceLabel = entry["name"].toString();
                    if (base == 300)
                    {
                        const auto a = nx::spectrumEnergy(self->tonalReference);
                        const auto b = nx::spectrumEnergy(curve);
                        if (a <= 1.0e-10 || b <= 1.0e-10)
                            return;
                        for (int i = 0; i < nx::tonalBands; ++i)
                            curve[i] = static_cast<float>(0.5 * (self->tonalReference[i] / a + curve[i] / b));
                        self->referenceLabel = "Combinada / " + self->referenceLabel;
                    }
                    else
                    {
                        self->referenceStats.valid = static_cast<bool>(entry.getProperty("metrics"));
                        self->referenceStats.loudness = entry["loudness"];
                        self->referenceStats.rms = entry["rms"];
                        self->referenceStats.peak = entry["peak"];
                        self->referenceStats.crest = entry["crest"];
                        self->referenceStats.balance = entry["balance"];
                        self->referenceStats.correlation = entry["correlation"];
                        self->referenceStats.duration = entry["duration"];
                    }
                    self->setTonalReference(curve);
                    self->tonalPanel.status = base == 300 ? utf8("Curvas combinadas após compensação de nível.")
                                                           : utf8("Referência carregada.");
                }
                self->updateTonalPanel();
            });
    }

    void selectToneTargets()
    {
        if (!plugin.hub())
        {
            tonalPanel.status = utf8("A pista local é o alvo no modo Track.");
            return;
        }
        juce::PopupMenu menu;
        menu.addItem(1, utf8("Todas as pistas elegíveis"));
        menu.addItem(2, "Limpar alvos");
        std::vector<unsigned> ids;
        for (const auto& row : rows)
        {
            ids.push_back(row.identity);
            menu.addItem(static_cast<int>(ids.size()) + 2, row.name, !row.tonalBus && !row.eqLocked && !row.referenceOnly,
                         tonalTargets.count(row.identity) != 0);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tonalPanel),
            [self = juce::Component::SafePointer<Editor>(this), ids = std::move(ids)](int id)
            {
                if (self == nullptr || id == 0)
                    return;
                self->cancelTonal();
                if (id == 1 || id == 2)
                {
                    self->tonalTargets.clear();
                    if (id == 1)
                        for (const auto& row : self->rows)
                            if (!row.tonalBus && !row.eqLocked && !row.referenceOnly)
                                self->tonalTargets.insert(row.identity);
                }
                else if (static_cast<std::size_t>(id - 3) < ids.size())
                {
                    const auto track = ids[static_cast<std::size_t>(id - 3)];
                    if (!self->tonalTargets.erase(track))
                        self->tonalTargets.insert(track);
                }
                self->updateTonalPanel();
            });
    }

    void clearTonePreviewLocked()
    {
        for (auto* track : registry().tracks)
            if (track != nullptr && track->tonalPreview.owner.load() == plugin.identity)
                track->tonalPreview.write({}, 0, 0);
        tonalPreviewOn = false;
    }

    void clearTonePreview()
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearTonePreviewLocked();
    }

    void cancelTonal()
    {
        clearTonePreview();
        tonalPending.clear();
    }

    void populateToneBands()
    {
        tonalPanel.correction.clear(juce::dontSendNotification);
        for (std::size_t i = 0; i < tonalPending.size(); ++i)
        {
            const auto* row = rowById(tonalPending[i].track);
            const auto name = row ? row->name : "Pista " + juce::String(tonalPending[i].track);
            for (int band = 0; band < 3; ++band)
                tonalPanel.correction.addItem(name + " / " + juce::String(band + 1),
                    static_cast<int>(i * 3) + band + 1);
        }
        if (!tonalPending.empty())
        {
            tonalPanel.correction.setSelectedId(1, juce::dontSendNotification);
            loadToneBand();
        }
    }

    void loadToneBand()
    {
        const int index = tonalPanel.correction.getSelectedId() - 1;
        if (index < 0 || static_cast<std::size_t>(index / 3) >= tonalPending.size())
            return;
        const auto& band = tonalPending[static_cast<std::size_t>(index / 3)].after.bands[static_cast<std::size_t>(index % 3)];
        tonalPanel.frequency.setValue(band.frequency, juce::dontSendNotification);
        tonalPanel.gain.setValue(band.gain, juce::dontSendNotification);
        tonalPanel.q.setValue(band.q, juce::dontSendNotification);
    }

    void editToneBand()
    {
        const int index = tonalPanel.correction.getSelectedId() - 1;
        if (index < 0 || static_cast<std::size_t>(index / 3) >= tonalPending.size())
            return;
        auto& bands = tonalPending[static_cast<std::size_t>(index / 3)].after.bands;
        bands[static_cast<std::size_t>(index % 3)] =
            {tonalPanel.frequency.getValue(), tonalPanel.gain.getValue(), tonalPanel.q.getValue()};
        nx::limitCorrection(bands, tonalPanel.limit.getValue());
        tonalPanel.gain.setValue(bands[static_cast<std::size_t>(index % 3)].gain, juce::dontSendNotification);
        updateCompensation();
    }

    void updateCompensation()
    {
        for (auto& item : tonalPending)
        {
            const auto* row = rowById(item.track);
            item.after.compensation = row && row->spectralCurrent && tonalPanel.compensation.getToggleState()
                ? nx::estimateCompensation(row->spectrum.before, item.after.bands, row->spectrum.rate) : 0.0;
        }
        if (tonalPreviewOn)
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            for (const auto& item : tonalPending)
                if (auto* track = findTrack(item.track); track && track->tonalPreview.owner.load() == plugin.identity)
                    track->tonalPreview.write(item.after, plugin.identity, plugin.session());
        }
    }

    void suggestTone(bool conflicts, bool append = false, int specialty = 0)
    {
        if (!append)
            cancelTonal();
        const auto profile = std::clamp(tonalPanel.genre.getSelectedId() - 1, 0, 9);
        if (!conflicts && profile == 9 && nx::spectrumEnergy(tonalReference) <= 1.0e-10)
        {
            tonalPanel.status = utf8("Capture ou importe uma referência para o perfil Personalizado.");
            return;
        }
        const TrackRow* focus = nullptr;
        if (conflicts)
        {
            for (const auto& row : rows)
                if (displayedSpectra.count(row.identity) && row.spectralCurrent
                    && (specialty != 2 || (row.instrument > 0 && nx::presets[row.instrument - 1].category == 6))
                    && (!focus || row.role == nx::focus || (focus->role != nx::focus && row.priority > focus->priority)))
                    focus = &row;
            if (!focus)
            {
                tonalPanel.status = specialty == 2 ? "Selecione um vocal reconhecido e seu acompanhamento na coluna Ver."
                                                   : "Selecione as pistas para comparar na coluna Ver.";
                return;
            }
        }
        const auto amount = tonalPanel.intensity.getValue() / 100;
        const auto cap = tonalPanel.limit.getValue();
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            for (const auto& row : rows)
            {
                const auto* track = findTrack(row.identity);
                const bool chosen = plugin.hub() ? tonalTargets.count(row.identity) != 0 : row.identity == plugin.identity;
                if (!chosen || !row.spectralCurrent || row.tonalBus
                    || track == nullptr || !track->eligible(plugin.session()) || !track->eqUnlocked()
                    || nx::spectrumEnergy(row.spectrum.before) < 1.0e-9)
                    continue;
                TonalChange item;
                item.track = row.identity;
                item.before = track->tone();
                if (conflicts)
                {
                    if (&row == focus || !displayedSpectra.count(row.identity) || row.role == nx::focus)
                        continue;
                    const double low = specialty == 2 ? 220 : 20;
                    const double high = specialty == 1 ? 300 : specialty == 2 ? 5000 : 20000;
                    const auto conflict = pairConflict(focus, &row, low, high);
                    if (conflict.band < 0 || amount <= 0 || focus->spectrum.frames < 4 || row.spectrum.frames < 4
                        || focus->spectrum.longBefore[conflict.band] < 1.0e-8f
                        || row.spectrum.longBefore[conflict.band] < 1.0e-8f)
                        continue;
                    item.conflictHz = nx::bandFrequency(conflict.band);
                    const auto referenceHz = focus->spectrum.dominantHz[conflict.band];
                    const auto otherHz = row.spectrum.dominantHz[conflict.band];
                    if (referenceHz > 0 && std::abs(referenceHz - otherHz) < 0.1f)
                    {
                        const auto phase = focus->spectrum.phase[conflict.band] - row.spectrum.phase[conflict.band];
                        item.phaseDifference = std::remainder(phase, 2 * nx::tonalPi) * 180.0 / nx::tonalPi;
                    }
                    const auto priority = 0.75 + 0.5 * std::max(0, focus->priority - row.priority) / 100.0;
                    item.after.bands[0] = {item.conflictHz, -std::min(cap, 3.0 * amount * priority), 1.1};
                }
                else
                {
                    const int categoryId = row.instrument > 0 ? nx::presets[row.instrument - 1].category
                        : row.role == nx::bass ? 1 : row.role == nx::drums ? 0 : row.role == nx::backingVocal ? 6 : 2;
                    if (row.spectrum.frames < 4)
                        continue;
                    const auto& measured = row.spectrum.longBefore;
                    const auto target = profile == 9 ? tonalReference : nx::adaptiveTarget(measured, categoryId,
                        row.role == nx::focus || row.function == nx::Function::lead,
                        nx::profiles[static_cast<std::size_t>(profile)]);
                    const double tolerance = categoryId == 0 || row.function == nx::Function::ambience ? 2.75 : 1.75;
                    item.after.bands = nx::match(measured, target, amount, cap, tolerance);
                    if (row.spectrum.resonance >= 0 && amount > 0)
                    {
                        const auto band = row.spectrum.resonance;
                        const auto wanted = -std::min(cap / 3, amount * 2);
                        item.after.bands[2] = {nx::bandFrequency(band), wanted, 2.4};
                        item.conflictHz = nx::bandFrequency(band);
                        nx::limitCorrection(item.after.bands, cap);
                    }
                    if (std::none_of(item.after.bands.begin(), item.after.bands.end(), [](const auto& band)
                        { return band.gain != 0; }))
                        continue;
                }
                const auto existing = std::find_if(tonalPending.begin(), tonalPending.end(), [&](const auto& other)
                    { return other.track == item.track; });
                if (existing == tonalPending.end())
                    tonalPending.push_back(item);
                else if (conflicts)
                {
                    auto weakest = std::min_element(existing->after.bands.begin(), existing->after.bands.end(),
                        [](const auto& a, const auto& b) { return std::abs(a.gain) < std::abs(b.gain); });
                    *weakest = item.after.bands[0];
                    existing->conflictHz = item.conflictHz;
                    existing->phaseDifference = item.phaseDifference;
                    nx::limitCorrection(existing->after.bands, cap);
                }
            }
        }
        tonalSession = plugin.session();
        populateToneBands();
        updateCompensation();
        tonalPanel.status = tonalPending.empty()
            ? utf8("Sem correção sugerida. Confira os alvos, o áudio e, no mascaramento, a posição sincronizada.")
            : utf8("Sugestões prontas. Ouça em B ou aprove com Aplicar EQ.");
    }

    bool canCommitTone(const TonalChange& item) const
    {
        auto* target = findTrack(item.track);
        return target != nullptr && target->eligible(tonalSession) && target->eqUnlocked() && !target->tonalBus()
            && item.before == target->tone()
            && (target->tonalPreview.owner.load() == 0 || target->tonalPreview.owner.load() == plugin.identity);
    }

    void finishTonal(nx::TonalPanel::Action action)
    {
        if (analysisOnly.getToggleState() || plugin.value("bypass") > 0.5f || tonalSession != plugin.session())
        {
            cancelTonal();
            tonalPanel.status = utf8("Ative os ajustes e mantenha a sessão para usar o EQ.");
            return;
        }
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (tonalPreviewOn && action == nx::TonalPanel::preview)
        {
            clearTonePreviewLocked();
            tonalPanel.status = "A: EQ aplicado antes da proposta.";
            return;
        }
        if (tonalPending.empty() || !std::all_of(tonalPending.begin(), tonalPending.end(),
                [this](const auto& item) { return canCommitTone(item) && safeTonalOutput(item); }))
        {
            clearTonePreviewLocked();
            tonalPanel.status = utf8("Confira a sessão, a margem e os alvos. Gere novas sugestões.");
            return;
        }
        if (action == nx::TonalPanel::preview)
        {
            if (std::any_of(tonalPending.begin(), tonalPending.end(), [](const auto& item)
                { return findTrack(item.track)->value("toneBypass") > 0.5f; }))
            {
                tonalPanel.status = "Desligue o Bypass EQ nos alvos para ouvir a proposta.";
                return;
            }
            for (const auto& item : tonalPending)
                findTrack(item.track)->tonalPreview.write(item.after, plugin.identity, tonalSession);
            tonalPreviewOn = true;
            tonalPanel.status = utf8("B: prévia do EQ. A gravação do ajuste depende de Aplicar EQ.");
        }
        else
        {
            for (auto& item : tonalPending)
            {
                auto* target = findTrack(item.track);
                target->setTone(item.after);
                item.after = target->tone();
            }
            clearTonePreviewLocked();
            plugin.tonalRedo.clear();
            plugin.tonalHistory.push_back(std::move(tonalPending));
            tonalPending.clear();
            tonalPanel.status = utf8("EQ aplicado. Desfazer EQ restaura a configuração anterior.");
        }
    }

    void undoTonal()
    {
        if (analysisOnly.getToggleState() || plugin.tonalHistory.empty())
            return;
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearTonePreviewLocked();
        tonalPending.clear();
        auto& batch = plugin.tonalHistory.back();
        std::vector<TonalChange> redone;
        batch.erase(std::remove_if(batch.begin(), batch.end(), [&](const auto& item)
        {
            auto* target = findTrack(item.track);
            if (!target || !target->eqUnlocked() || target->session() != plugin.session() || !(target->tone() == item.after))
                return false;
            target->setTone(item.before);
            redone.push_back(item);
            return true;
        }), batch.end());
        const bool complete = batch.empty();
        if (!redone.empty())
            plugin.tonalRedo.push_back(std::move(redone));
        if (complete)
            plugin.tonalHistory.pop_back();
        tonalPanel.status = complete ? "EQ restaurado." : utf8("Há pistas indisponíveis ou alteradas por automação.");
    }

    void handleTonal(nx::TonalPanel::Action action)
    {
        using Panel = nx::TonalPanel;
        switch (action)
        {
            case Panel::selectTracks: selectToneTargets(); break;
            case Panel::capture:
            {
                const nx::SpectrumFrame* frame = spectralCurrent ? &localSpectrum : nullptr;
                const nx::Reading* measured = localCurrent ? &local : nullptr;
                for (const auto& row : rows)
                    if (row.spectralCurrent && displayedSpectra.count(row.identity))
                    {
                        frame = &row.spectrum;
                        measured = row.current ? &row.reading : nullptr;
                        break;
                    }
                if (frame && nx::spectrumEnergy(frame->before) > 1.0e-10)
                {
                    ++referenceRequest;
                    referenceStats = {};
                    if (measured)
                    {
                        referenceStats.valid = true;
                        referenceStats.rms = measured->rms;
                        referenceStats.peak = measured->peak;
                        referenceStats.crest = std::max(0.0f, measured->peak - measured->rms);
                        referenceStats.balance = frame->pan;
                    }
                    referenceType = frame == &localSpectrum && plugin.hub() ? 0 : 2;
                    referenceLabel = referenceType == 0 ? "Mix Bus" : utf8("Captura da sessão");
                    setTonalReference(frame->frames >= 4 ? frame->longBefore : frame->before);
                    tonalPanel.status = utf8("Referência capturada a partir do sinal medido.");
                }
                else
                    tonalPanel.status = utf8("Aguardando sinal para capturar a referência.");
                break;
            }
            case Panel::import: importTonalReference(); break;
            case Panel::suggest: suggestTone(false); break;
            case Panel::masking: suggestTone(true); break;
            case Panel::lowEnd: suggestTone(true, false, 1); break;
            case Panel::vocal: suggestTone(true, false, 2); break;
            case Panel::library: showReferenceLibrary(); break;
            case Panel::preview:
            case Panel::apply: finishTonal(action); break;
            case Panel::cancel: cancelTonal(); tonalPanel.status = utf8("Sugestão descartada."); break;
            case Panel::undo: undoTonal(); break;
            case Panel::bypass:
            {
                if (analysisOnly.getToggleState())
                    break;
                std::lock_guard<std::mutex> lock(registry().mutex);
                clearTonePreviewLocked();
                tonalBypassed = !tonalBypassed;
                for (auto* track : registry().tracks)
                    if (track && track->session() == plugin.session() && track->eqUnlocked() && !track->tonalBus()
                        && (plugin.hub() ? tonalTargets.count(track->identity) != 0 : track == &plugin))
                        track->setParameter("toneBypass", tonalBypassed ? 1 : 0);
                break;
            }
        }
        updateTonalPanel();
    }

    void updateTonalPanel()
    {
        if (!tonalPanel.isVisible())
            return;
        const bool writable = !analysisOnly.getToggleState() && plugin.value("bypass") < 0.5f;
        if (tonalPreviewOn && (!writable || tonalSession != plugin.session()))
            cancelTonal();
        const auto source = tonalPanel.genre.getSelectedId() == 10 && nx::spectrumEnergy(tonalReference) > 1.0e-10
            ? utf8("Referência") : utf8("Perfil provisório");
        tonalPanel.summary = (plugin.hub() ? juce::String(tonalTargets.size()) + " alvos"
            : "Pista local") + " / " + juce::String(tonalPending.size()) + " EQ / " + source;
        tonalPanel.update(!tonalPending.empty() && writable, tonalPreviewOn, !plugin.tonalHistory.empty() && writable,
                          tonalBypassed);
        tonalPanel.repaint();
    }

    nx::GlobalHeadroom busPlan() const
    {
        if (!plugin.hub() || !localCurrent)
            return {};
        const auto sample = nx::db(*std::max_element(local.outputLevels.peak.begin(), local.outputLevels.peak.end()));
        const auto intersample = nx::db(*std::max_element(local.outputLevels.truePeak.begin(), local.outputLevels.truePeak.end()));
        return nx::planGlobalHeadroom(sample, intersample, plugin.value("premasterPeak"),
                                      plugin.value("trueCeiling"), plugin.value("outputTrim"));
    }

    void cancelSmart()
    {
        if (analyzing && smartAnalysing)
        {
            assignLearning(0);
            analyzing = false;
        }
        smartAnalysing = false;
        discard(utf8("Prévia de preparação descartada."));
        cancelTonal();
    }

    void redoSmart()
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearPreview();
        clearTonePreviewLocked();
        unsigned restored = 0;
        if (!plugin.redoHistory.empty())
        {
            auto& batch = plugin.redoHistory.back();
            std::vector<Change> applied;
            capturedSession = plugin.session();
            capturedHeadroom = plugin.value("headroom");
            batch.erase(std::remove_if(batch.begin(), batch.end(), [&](const auto& item)
            {
                if (!stillApplies(item))
                    return false;
                auto* target = findTrack(item.identity);
                target->setTrim(item.next);
                target->lastAppliedTrim = item.next;
                applied.push_back(item);
                ++restored;
                return true;
            }), batch.end());
            if (!applied.empty())
                plugin.history.push_back(std::move(applied));
            if (batch.empty())
                plugin.redoHistory.pop_back();
        }
        if (!plugin.tonalRedo.empty())
        {
            auto& batch = plugin.tonalRedo.back();
            std::vector<TonalChange> applied;
            tonalSession = plugin.session();
            batch.erase(std::remove_if(batch.begin(), batch.end(), [&](const auto& item)
            {
                if (!canCommitTone(item) || !safeTonalOutput(item))
                    return false;
                findTrack(item.track)->setTone(item.after);
                applied.push_back(item);
                ++restored;
                return true;
            }), batch.end());
            if (!applied.empty())
                plugin.tonalHistory.push_back(std::move(applied));
            if (batch.empty())
                plugin.tonalRedo.pop_back();
        }
        if (!busRedo.empty() && plugin.volumeUnlocked())
        {
            const auto entry = busRedo.back();
            if (std::abs(plugin.value("outputTrim") - entry.first) < 0.011f)
            {
                plugin.setParameter("outputTrim", entry.second);
                busUndo.push_back(entry);
                busRedo.pop_back();
                ++restored;
            }
        }
        smartStatus = juce::String(restored) + utf8(" ajustes reaplicados com as condições atuais.");
    }

    bool safeTonalOutput(const TonalChange& item) const
    {
        const auto* track = findTrack(item.track);
        if (track == nullptr)
            return false;
        nx::Reading reading;
        if (!track->measurement.read(reading, plugin.session()) || !reading.active())
            return false;
        auto gainTrim = track->value("trim");
        for (const auto& change : pending)
            if (change.identity == item.track)
                gainTrim = change.next;
        const auto inputTrue = nx::db(*std::max_element(reading.inputLevels.truePeak.begin(), reading.inputLevels.truePeak.end()));
        double boost = std::max(0.0, item.after.compensation);
        for (const auto& band : item.after.bands)
            boost += std::max(0.0, band.gain);
        return inputTrue + gainTrim + track->value("outputTrim") + boost <= plugin.value("trueCeiling") - 0.3;
    }

    void handleSmart(nx::SmartPanel::Action action)
    {
        using Panel = nx::SmartPanel;
        switch (action)
        {
            case Panel::analyse:
                if (!smartPanel.gainModule.getToggleState() && !smartPanel.toneModule.getToggleState()
                    && !smartPanel.maskModule.getToggleState())
                    smartStatus = utf8("Selecione pelo menos um módulo.");
                else
                {
                    cancelSmart();
                    if (plugin.hub() && smartPanel.toneModule.getToggleState() && tonalTargets.empty())
                        for (const auto& row : rows)
                            if (!row.tonalBus && !row.eqLocked && !row.referenceOnly)
                                tonalTargets.insert(row.identity);
                    smartStatus = utf8("Reproduza o trecho e aguarde a janela. A análise não aplica ajustes.");
                    startAnalysis(true);
                }
                break;
            case Panel::preview:
            {
                const bool listeningBefore = listening || tonalPreviewOn;
                if (!pending.empty() && (!listeningBefore || listening))
                    togglePreview();
                if (!tonalPending.empty() && (!listeningBefore || tonalPreviewOn))
                    finishTonal(nx::TonalPanel::preview);
                smartStatus = listening || tonalPreviewOn ? utf8("B: ajustes em prévia.") : utf8("A: ajustes já gravados.");
                break;
            }
            case Panel::apply:
            {
                if (pending.empty() && tonalPending.empty())
                    break;
                bool safe;
                {
                    std::lock_guard<std::mutex> lock(registry().mutex);
                    safe = std::all_of(pending.begin(), pending.end(), [this](const auto& item) { return stillApplies(item); })
                        && std::all_of(tonalPending.begin(), tonalPending.end(),
                            [this](const auto& item) { return canCommitTone(item) && safeTonalOutput(item); });
                }
                if (!safe || analysisOnly.getToggleState())
                {
                    smartStatus = utf8("Aplicação suspensa: os alvos, a margem ou as medições mudaram.");
                    break;
                }
                if (!pending.empty())
                    commit();
                if (!tonalPending.empty())
                    finishTonal(nx::TonalPanel::apply);
                smartStatus = "Ajustes aplicados. Reproduza novamente para validar o Mix Bus.";
                break;
            }
            case Panel::cancel:
                cancelSmart();
                smartStatus = "Propostas descartadas.";
                break;
            case Panel::undo:
                if (smartPanel.toneModule.getToggleState() || smartPanel.maskModule.getToggleState())
                    undoTonal();
                if (smartPanel.gainModule.getToggleState())
                    undoLast();
                if (!busUndo.empty() && plugin.volumeUnlocked() && !analysisOnly.getToggleState())
                {
                    const auto entry = busUndo.back();
                    if (std::abs(plugin.value("outputTrim") - entry.second) < 0.011f)
                    {
                        plugin.setParameter("outputTrim", entry.first);
                        busRedo.push_back(entry);
                        busUndo.pop_back();
                    }
                }
                smartStatus = utf8("Desfazer executado para os módulos selecionados.");
                break;
            case Panel::redo:
                if (!analysisOnly.getToggleState())
                    redoSmart();
                break;
            case Panel::headroom:
            {
                const auto plan = busPlan();
                const auto original = plugin.value("outputTrim");
                if (!plan.available || !plugin.volumeUnlocked() || analysisOnly.getToggleState())
                    smartStatus = utf8("Ajuste de saída indisponível. Confira reprodução e bloqueios.");
                else if (std::abs(original - plan.suggested) < 0.01f)
                    smartStatus = utf8("Saída dentro da margem medida.");
                else
                {
                    plugin.setParameter("outputTrim", plan.suggested);
                    busUndo.emplace_back(original, plugin.value("outputTrim"));
                    busRedo.clear();
                    smartStatus = utf8("Ganho de saída ajustado. Valide o pico no próximo trecho.");
                }
                break;
            }
        }
        updateSmartPanel();
    }

    void updateSmartPanel()
    {
        if (!smartPanel.isVisible())
            return;
        smartPanel.layout(plugin.hub());
        smartPanel.mode.setSelectedId(static_cast<int>(strategy()) + 1, juce::dontSendNotification);
        const auto oldId = smartPanel.diagnosticTrack.getSelectedId();
        bool same = smartPanel.diagnosticTrack.getNumItems() == static_cast<int>(rows.size());
        for (int i = 0; same && i < static_cast<int>(rows.size()); ++i)
            same = smartPanel.diagnosticTrack.getItemId(i) == static_cast<int>(rows[i].identity)
                && smartPanel.diagnosticTrack.getItemText(i) == rows[i].name;
        if (!same)
        {
            smartPanel.diagnosticTrack.clear(juce::dontSendNotification);
            for (const auto& row : rows)
                smartPanel.diagnosticTrack.addItem(row.name, static_cast<int>(row.identity));
            smartPanel.diagnosticTrack.setSelectedId(oldId, juce::dontSendNotification);
            if (smartPanel.diagnosticTrack.getSelectedId() == 0 && !rows.empty())
                smartPanel.diagnosticTrack.setSelectedItemIndex(0, juce::dontSendNotification);
        }
        const auto* row = plugin.hub() ? rowById(static_cast<unsigned>(smartPanel.diagnosticTrack.getSelectedId()))
                                     : rows.empty() ? nullptr : &rows.front();
        juce::String text = smartStatus.isEmpty() ? status : smartStatus;
        text += "\n\n";
        if (row != nullptr && row->current)
        {
            const auto outTrue = nx::db(*std::max_element(row->reading.outputLevels.truePeak.begin(),
                                                          row->reading.outputLevels.truePeak.end()));
            text += row->name + "\nEntrada: " + number(row->reading.rms) + " dBFS RMS / "
                + number(row->reading.peak) + " dBFS pico\n";
            if (row->diagnostics.sufficient)
                text += "Desejado: " + number(row->diagnostics.wanted) + " dB / permitido: "
                    + number(row->diagnostics.permitted) + " dB\n";
            text += "Aplicado: " + number(row->trim) + " dB";
            if (row->diagnostics.sufficient)
                text += " / estimado: " + number(row->proposed.trim) + " dB";
            text += "\n";
            text += utf8("Saída: ") + number(row->reading.outputPeak) + " dBFS / " + number(outTrue)
                + " dBTP / margem: " + number(-outTrue) + " dB\n";
            text += "Crista: " + number(std::max(0.0f, row->reading.peak - row->reading.rms)) + " dB / atividade: "
                + number(row->reading.activity * 100) + "%\n";
            text += row->volumeLocked || row->referenceOnly ? "Ganho bloqueado nesta pista.\n"
                : row->diagnostics.sufficient ? utf8("Evidência: sinal ativo medido com margem verificada.\n")
                                              : utf8("Evidência insuficiente. Aguarde mais áudio.\n");
            if (row->spectralCurrent && row->spectrum.resonance >= 0)
                text += utf8("Pico estreito recorrente: ") + juce::String(nx::bandFrequency(row->spectrum.resonance), 0) + " Hz\n";
            for (const auto& item : tonalPending)
                if (item.track == row->identity)
                {
                    text += item.conflictHz > 0 ? "Conflito sincronizado.\n" : utf8("Correção de envelope tonal.\n");
                    if (std::abs(item.phaseDifference) <= 180)
                        text += utf8("Diferença de fase no bin: ") + juce::String(item.phaseDifference, 1) + utf8("°\n");
                    for (const auto& band : item.after.bands)
                        if (band.gain != 0)
                            text += juce::String(band.frequency, 0) + " Hz: " + juce::String(band.gain, 1) + " dB\n";
                }
        }
        else
            text += utf8("Sem sinal válido para esta pista.\n");
        const auto mix = busPlan();
        if (mix.available)
            text += "\nMix Bus: " + number(mix.sample) + " dBFS / " + number(mix.truePeak) + " dBTP"
                + utf8("\nSaída proposta: ") + number(mix.suggested) + " dB";
        if (referenceStats.valid)
        {
            text += "\nRef. " + referenceLabel + " / " + juce::String(referenceStats.rms, 1)
                + " dBFS RMS / pico " + juce::String(referenceStats.peak, 1)
                + "\nCrista: " + juce::String(referenceStats.crest, 1) + " dB / L/R "
                + juce::String(referenceStats.balance, 2);
            if (referenceStats.duration > 0)
                text += "\n" + juce::String(referenceStats.loudness, 1) + " LUFS / "
                    + utf8("correlação ") + juce::String(referenceStats.correlation, 2);
        }
        smartPanel.report(text);
        smartPanel.update(analyzing, !pending.empty() || !tonalPending.empty(), listening || tonalPreviewOn,
            !plugin.history.empty() || !plugin.tonalHistory.empty() || !busUndo.empty(),
            !plugin.redoHistory.empty() || !plugin.tonalRedo.empty() || !busRedo.empty(),
            !analysisOnly.getToggleState() && plugin.value("bypass") < 0.5f);
    }

    void showSpectrumOptions()
    {
        juce::PopupMenu menu;
        menu.addItem(1, utf8("Zoom 20 Hz – 20 kHz"));
        menu.addItem(2, utf8("Suavizar curva"), true, smoothSpectrum);
        const std::array<int, 4> windows {0, 300, 1000, 3000};
        for (std::size_t i = 0; i < windows.size(); ++i)
            menu.addItem(static_cast<int>(i) + 3, utf8("Média ") + juce::String(windows[i]) + " ms",
                         true, averageMs == windows[i]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(spectrumOptions),
            [self = juce::Component::SafePointer<Editor>(this), windows](int id)
            {
                if (self == nullptr)
                    return;
                if (id == 1)
                {
                    self->graphStart = 0;
                    self->graphSpan = 96;
                }
                else if (id == 2)
                    self->smoothSpectrum = !self->smoothSpectrum;
                else if (id >= 3 && id <= 6)
                {
                    self->averageMs = windows[static_cast<std::size_t>(id - 3)];
                    self->configureSpectrum();
                }
                self->repaint();
            });
    }

    void drawSpectrum(juce::Graphics& g) const
    {
        const auto plot = plotBounds();
        const bool norm = normalise.getToggleState();
        const auto bandX = [&](double band)
        {
            return plot.getX() + plot.getWidth() * static_cast<float>((band - graphStart) / graphSpan);
        };
        const auto frequencyX = [&](double hz) { return bandX(std::log(hz / 20) * 96 / std::log(1000.0)); };
        const auto dbY = [&](double db)
        {
            return plot.getY() + plot.getHeight() * static_cast<float>(std::clamp(-db / 84.0, 0.0, 1.0));
        };
        g.setColour(juce::Colour(0x9c080f17));
        g.fillRect(plot);
        for (int i = 0; i <= 4; ++i)
        {
            const auto y = juce::roundToInt(dbY(-21 * i));
            g.setColour(juce::Colour(0xff344959));
            g.drawHorizontalLine(y, plot.getX(), plot.getRight());
            label(g, juce::String(-21 * i), 25, y - 8, 31, 16, 11);
        }
        for (const int hz : {20, 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000})
        {
            const auto x = juce::roundToInt(frequencyX(hz));
            if (x >= plot.getX() - 1 && x <= plot.getRight() + 1)
            {
                g.setColour(juce::Colour(0xff344959));
                g.drawVerticalLine(x, plot.getY(), plot.getBottom());
                if (graphSpan < 75 || hz == 20 || hz == 100 || hz == 500 || hz == 2000 || hz == 10000 || hz == 20000)
                {
                    const auto text = hz < 1000 ? juce::String(hz) + " Hz" : juce::String(hz / 1000) + " kHz";
                    label(g, text, std::clamp(x - 12, 58, 639), 354, 51, 18, 10);
                }
            }
        }
        const auto& traces = spectralFreeze.getToggleState() ? frozenRows : rows;
        const auto& mix = spectralFreeze.getToggleState() ? frozenSpectrum : localSpectrum;
        const bool mixReady = spectralFreeze.getToggleState() ? frozenCurrent : spectralCurrent;
        const auto view = spectrumMode.getSelectedId();
        const nx::Spectrum* cursorPower = nullptr;
        const TrackRow* compared[2] {};
        int count = 0;
        for (const auto& row : traces)
            if (displayedSpectra.count(row.identity) != 0 && count < 2)
                compared[count++] = &row;

        auto trace = [&](const nx::Spectrum& power, juce::Colour color, bool translucent)
        {
            if (nx::spectrumEnergy(power) <= 1.0e-10)
                return;
            cursorPower = &power;
            const double normal = norm ? *std::max_element(power.begin(), power.end()) : 1.0;
            std::array<float, nx::tonalBands> y {}, slope {};
            for (int i = 0; i < nx::tonalBands; ++i)
            {
                const auto value = smoothSpectrum
                    ? 0.15 * power[std::max(0, i - 1)] + 0.70 * power[i] + 0.15 * power[std::min(nx::tonalBands - 1, i + 1)]
                    : power[i];
                y[i] = dbY(nx::powerDb(value / normal));
            }
            for (int i = 1; smoothSpectrum && i < nx::tonalBands - 1; ++i)
            {
                const auto a = y[i] - y[i - 1], b = y[i + 1] - y[i];
                if (a * b > 0)
                    slope[i] = (2 * a * b) / (a + b);
            }
            juce::Path path;
            path.startNewSubPath(bandX(0.5), y[0]);
            for (int i = 0; i < nx::tonalBands - 1; ++i)
                if (smoothSpectrum)
                    path.cubicTo(bandX(i + 0.8333), y[i] + slope[i] / 3, bandX(i + 1.1667),
                                 y[i + 1] - slope[i + 1] / 3, bandX(i + 1.5), y[i + 1]);
                else
                    path.lineTo(bandX(i + 1.5), y[i + 1]);
            auto fill = path;
            fill.lineTo(bandX(nx::tonalBands - 0.5), plot.getBottom());
            fill.lineTo(bandX(0.5), plot.getBottom());
            fill.closeSubPath();
            g.setGradientFill({color.withAlpha(translucent ? 0.08f : 0.30f), plot.getX(), plot.getY(), color.withAlpha(0.01f),
                               plot.getX(), plot.getBottom(), false});
            g.fillPath(fill);
            g.setColour(color.withAlpha(translucent ? 0.68f : 1.0f));
            g.strokePath(path, juce::PathStrokeType(translucent ? 1.2f : 1.8f));
        };
        juce::String legend;
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(plot.getSmallestIntegerContainer());
            const auto conflict = pairConflict(compared[0], compared[1]);
            if (view == 4 && conflict.band >= 0)
            {
                g.setColour(juce::Colour(0x36ffb64a));
                g.fillRect(juce::Rectangle<float>(bandX(conflict.band - 3), plot.getY(),
                    bandX(conflict.band + 4) - bandX(conflict.band - 3), plot.getHeight()));
            }
            if (view == 1 && mixReady)
            {
                trace(mix.after, juce::Colour(cyan), false);
                legend = plugin.hub() ? "Mix bus" : "Track";
            }
            if (view == 2 || view == 3 || view == 4)
            {
                int drawn = 0;
                for (const auto& row : traces)
                    if (row.spectralCurrent && displayedSpectra.count(row.identity) != 0)
                    {
                        trace(row.spectrum.before, spectrumColour(row), view != 2);
                        legend += (legend.isEmpty() ? "" : " / ") + row.name;
                        if (++drawn == (view == 2 ? 1 : view == 4 ? 2 : 64))
                            break;
                    }
            }
            if (view == 5 || view == 6)
            {
                const auto* source = compared[0] != nullptr && compared[0]->spectralCurrent ? &compared[0]->spectrum
                    : mixReady ? &mix : nullptr;
                if (source != nullptr)
                {
                    trace(source->before, juce::Colour(cyan), false);
                    if (view == 6)
                        trace(source->after, juce::Colour(0xffe8a05e), true);
                }
                if (view == 5)
                    trace(tonalReference, juce::Colour(0xffd099f8), true);
                legend = view == 5 ? utf8("Sinal / Referência") : "Antes / Depois";
            }
            if (plot.contains(graphCursor))
            {
                g.setColour(juce::Colour(0x9ce1f5ff));
                g.drawVerticalLine(juce::roundToInt(graphCursor.x), plot.getY(), plot.getBottom());
                g.drawHorizontalLine(juce::roundToInt(graphCursor.y), plot.getX(), plot.getRight());
            }
        }
        if (!cursorPower && view != 7)
            label(g, view >= 2 && view <= 4 ? "Marque as pistas na coluna Ver" : "Aguardando sinal",
                  276, 257, 248, 20, 13, ink);
        if (plot.contains(graphCursor))
        {
            const double band = graphStart + (graphCursor.x - plot.getX()) / plot.getWidth() * graphSpan;
            const auto hz = nx::bandFrequency(band - 0.5);
            const double normPower = cursorPower && norm ? *std::max_element(cursorPower->begin(), cursorPower->end()) : 1.0;
            const double level = cursorPower ? nx::powerDb((*cursorPower)[std::clamp(static_cast<int>(band), 0, 95)] / normPower)
                : -84.0 * (graphCursor.y - plot.getY()) / plot.getHeight();
            legend = juce::String(hz, 1) + " Hz   " + juce::String(level, 1) + (norm ? " dB rel." : " dBFS");
        }
        label(g, legend, 31, 374, 645, 16, 11, muted);
    }

    void drawRows(juce::Graphics& g)
    {
        const auto selected = static_cast<unsigned>(pick.getSelectedId());
        label(g, "Pista", 34, 428, 130, 16, 11);
        label(g, "RMS dBFS", 248, 428, 68, 16, 11);
        if (plugin.hub())
            label(g, "Ver", 216, 428, 25, 16, 11, cyan);
        label(g, "Pico dBFS", 327, 428, 68, 16, 11);
        if (plugin.hub() && tonalPanel.isVisible())
            label(g, "EQ", 386, 428, 23, 16, 11, cyan);
        label(g, "Sug. dB", 413, 428, 60, 16, 11);
        label(g, "Trim dB", 488, 428, 62, 16, 11);
        label(g, "Sinal / margem", 557, 428, 92, 16, 11);
        if (plugin.hub())
            label(g, "Foco", 650, 428, 31, 16, 11, cyan);
        if (rows.empty())
            label(g, utf8("Adicione instâncias Track às pistas, com a mesma sessão deste HUB."), 31, 464, 644, 32);
        if (rows.size() > visibleRows)
            label(g, juce::String(firstRow + 1) + "-" + juce::String(firstRow + visibleRows) + " / "
                      + juce::String(rows.size()), 602, 411, 79, 16, 11, cyan);
        for (int i = 0; i < visibleRows && firstRow + i < static_cast<int>(rows.size()); ++i)
        {
            const auto& row = rows[static_cast<std::size_t>(firstRow + i)];
            const float y = 445.0f + i * 30.0f;
            const juce::Rectangle<float> strip(20, y, 666, 28);
            g.setColour(juce::Colour(row.identity == selected ? 0xff1c3547 : 0x58172331));
            g.fillRoundedRectangle(strip, 5);
            if (row.identity == selected)
            {
                g.setColour(juce::Colour(0xff408ca1));
                g.drawRoundedRectangle(strip, 5, 1);
            }
            const int top = static_cast<int>(y);
            if (plugin.hub())
            {
                g.setColour(spectrumColour(row));
                g.fillEllipse(26.0f, y + 11.0f, 6.0f, 6.0f);
            }
            label(g, row.name + " / G" + juce::String(row.group), 36, top, plugin.hub() ? 140 : 84, 28, 13, ink);
            if (!plugin.hub() && row.role == nx::focus)
                badge(g, "Foco", 124, top + 3, 43, cyan);
            const bool signal = row.current && row.reading.peak > -90;
            drawSegments(g, {183, y + 9, plugin.hub() ? 26.0f : 51.0f, 10}, signal ? row.reading.rms : nx::floorDb, 10, false);
            const auto dash = utf8("—");
            label(g, signal ? number(row.reading.rms) : dash, 248, top, 75, 28, 13);
            label(g, signal ? number(row.reading.peak) : dash, 327, top, 75, 28, 13);
            const auto frozen = std::find_if(pending.begin(), pending.end(),
                [&](const auto& change) { return change.identity == row.identity; });
            const auto suggestion = pending.empty() ? (canSuggest(row) ? number(row.proposed.trim) : dash)
                : frozen == pending.end() ? dash : number(frozen->next);
            label(g, suggestion, 413, top, 74, 28, 13, frozen != pending.end() ? cyan : muted);
            label(g, number(row.trim), 488, top, 65, 28, 13, ink);
            if (row.adjusted)
            {
                g.setColour(juce::Colour(0xff60e995));
                g.fillEllipse(536.0f, y + 11.0f, 6.0f, 6.0f);
            }
            const juce::String state = !signal ? "Sem sinal" : !row.reading.active() ? "Medindo"
                : row.proposed.headroomUnmet ? "Margem insuficiente"
                : row.proposed.headroomLimited ? "Limitado pelo pico" : row.adjusted ? "Ajustada" : "Ativo";
            label(g, state, 557, top, plugin.hub() ? 90 : 123, 28, 12, row.proposed.headroomLimited ? amber : muted);
        }
    }

    void configureButton(juce::TextButton& button, const juce::String& text, std::function<void()> action)
    {
        button.setButtonText(text);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    bool canSuggest(const TrackRow& row) const
    {
        return !row.volumeLocked && !row.referenceOnly && row.current && row.reading.active() && (!needsAnchor() || focusRow >= 0);
    }

    void timerCallback() override
    {
        if ((plugin.hub() != capturedHub || plugin.session() != capturedSession || plugin.value("bypass") > 0.5f)
            && !pending.empty())
            discard(utf8("Prévia cancelada após alteração de sessão, instância ou bypass."));

        const bool isHub = plugin.hub();
        operation.setVisible(isHub);
        if (instruments.getSelectedId() != static_cast<int>(plugin.value("instrument")) + 1)
            populateInstruments();
        scope.setVisible(isHub);
        limit.setVisible(isHub);
        reference.setVisible(mode() == nx::GainMode::calibration);
        reference.setBounds(isHub ? 474 : 95, isHub ? 75 : 113, 119, 27);
        headroom.setBounds(isHub ? 671 : 304, isHub ? 75 : 113, 118, 27);
        trackLimit.setVisible(!isHub);
        tonalExclude.setVisible(!isHub);
        pick.setVisible(isHub && scope.getSelectedId() == 1);
        group.setVisible(!isHub || scope.getSelectedId() == 2);
        group.setBounds(isHub ? 222 : 459, isHub ? 114 : 75, 125, 26);
        role.setVisible(!isHub);
        support.setVisible(!isHub);
        peakReference.setVisible(!isHub);
        support.setEnabled(plugin.role() == nx::automatic || plugin.role() == nx::music);
        trim.setVisible(!isHub);

        localCurrent = plugin.measurement.read(local, plugin.session());
        spectralCurrent = plugin.spectrum.read(localSpectrum, plugin.session());
        rows.clear();
        focusRow = otherRow = -1;
        auto append = [&](Processor& track)
        {
            TrackRow row {track.identity, track.group(), track.musicalRole(), track.value("trim"), false, {}, {}, 0, track.displayName()};
            row.current = track.eligible(plugin.session())
                && track.measurement.read(row.reading, plugin.session(), &row.timestamp);
            if (isHub)
                track.spectrum.configure(std::max(0, fftChoice.getSelectedId() - 1), averageMs);
            row.spectralCurrent = row.current && track.spectrum.read(row.spectrum, plugin.session());
            row.tonalBus = track.tonalBus();
            row.volumeLocked = track.value("volumeLock") > 0.5f;
            row.eqLocked = track.value("eqLock") > 0.5f;
            row.referenceOnly = track.value("referenceOnly") > 0.5f;
            row.priority = static_cast<int>(track.value("priority"));
            row.function = static_cast<nx::Function>(static_cast<int>(track.value("function")));
            if (row.spectralCurrent)
            {
                for (int i = 0; i < nx::tonalBands; ++i)
                    row.reading.power[i / 4] += row.spectrum.instant[i];
                row.reading.position = row.spectrum.end;
            }
            row.peakProfile = track.value("instrument") > 0 ? 0 : static_cast<int>(track.value("peakReference"));
            row.instrument = track.instrument();
            row.customPeak = track.value("targetPeak");
            row.outputGainDb = track.value("outputTrim");
            row.toneBoost = track.tonalBoost();
            row.instrumentCeiling = track.instrumentCeiling();
            row.adjusted = track.lastAppliedTrim && std::abs(*track.lastAppliedTrim - row.trim) <= 0.011f;
            rows.push_back(row);
        };
        if (isHub)
        {
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                for (auto* track : registry().tracks)
                    if (track != nullptr && !track->hub() && track->session() == plugin.session())
                        append(*track);
            }
            firstRow = std::clamp(firstRow, 0, std::max(0, static_cast<int>(rows.size()) - visibleRows));
            refreshHub();
        }
        else
        {
            firstRow = 0;
            append(plugin);
        }
        updateSuggestions();
        updatePrincipalButtons();
        updateTonalPanel();
        updateSmartPanel();

        finishAnalysis();
        updateButtons();
        repaint();
    }

    void populateInstruments()
    {
        instruments.clear(juce::dontSendNotification);
        instruments.addItem(utf8("Automático pelo nome da pista"), 1);
        const int chosen = category.getSelectedId() - 2;
        const int selected = static_cast<int>(plugin.value("instrument"));
        for (std::size_t i = 0; i < std::size(nx::presets); ++i)
            if (chosen < 0 || nx::presets[i].category == chosen || static_cast<int>(i) + 1 == selected)
                instruments.addItem(utf8(nx::presets[i].name), static_cast<int>(i) + 2);
        instruments.setSelectedId(selected + 1, juce::dontSendNotification);
    }

    void assignLearning(unsigned id)
    {
        if (id != 0 && !plugin.hub())
        {
            plugin.learningSensitivity.store(plugin.value("sensitivity"));
            plugin.learningRequest.store(id);
            return;
        }
        std::lock_guard<std::mutex> lock(registry().mutex);
        for (auto* track : registry().tracks)
            if (track && id == 0 && track->learningRequest.load() == activeLearning)
                track->learningRequest.store(0);
            else if (track && id != 0 && !track->hub() && track->session() == plugin.session())
            {
                track->learningSensitivity.store(plugin.value("sensitivity"));
                track->learningRequest.store(id);
            }
        if (id == 0 && plugin.learningRequest.load() == activeLearning)
            plugin.learningRequest.store(0);
    }

    void updatePrincipalButtons()
    {
        for (int i = 0; i < visibleRows; ++i)
        {
            auto& button = principalButtons[static_cast<std::size_t>(i)];
            auto& checkbox = spectrumButtons[static_cast<std::size_t>(i)];
            auto& target = tonalButtons[static_cast<std::size_t>(i)];
            const auto index = static_cast<std::size_t>(firstRow + i);
            const bool present = plugin.hub() && index < rows.size();
            button.setVisible(present);
            checkbox.setVisible(present);
            target.setVisible(present && tonalPanel.isVisible());
            if (present)
            {
                const auto& row = rows[index];
                target.setTitle("Alvo de EQ: " + row.name);
                target.setEnabled(!row.tonalBus && !row.eqLocked && !row.referenceOnly);
                target.setToggleState(tonalTargets.count(row.identity) != 0, juce::dontSendNotification);
                checkbox.setTitle(utf8("Mostrar espectro de ") + row.name);
                checkbox.setToggleState(displayedSpectra.count(row.identity) != 0, juce::dontSendNotification);
                button.setTitle("Marcar " + row.name + " como Foco");
                button.setTooltip("Usar " + row.name + " como pista de foco e principal");
                button.setToggleState(row.role == nx::focus, juce::dontSendNotification);
            }
        }
    }

    void choosePrincipal(int visibleIndex)
    {
        const auto index = static_cast<std::size_t>(firstRow + visibleIndex);
        if (!plugin.hub() || index >= rows.size())
            return;
        const auto identity = rows[index].identity;
        const int sessionId = plugin.session();
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            auto* selected = findTrack(identity);
            if (selected == nullptr || selected->hub() || selected->session() != sessionId)
                return;

            for (auto* track : registry().tracks)
                if (track != nullptr && track != selected && !track->hub()
                    && track->session() == sessionId && track->role() == nx::focus)
                    track->setParameter("role", nx::automatic);
            selected->setParameter("role", nx::focus);
            if (strategy() != nx::GainStrategy::hybrid)
                plugin.setParameter("smartStrategy", 0);
            plugin.setParameter("operation", 1);
        }
        if (analyzing)
            assignLearning(0);
        analyzing = false;
        discard(rows[index].name + utf8(" é a Principal. Reproduza o trecho e clique em ANALYZER."));
        timerCallback();
    }

    void updateButtons()
    {
        analyze.setToggleState(analyzing, juce::dontSendNotification);
        const bool writable = !analysisOnly.getToggleState() && plugin.value("bypass") < 0.5f;
        analyze.setEnabled(writable && !freeze.getToggleState());
        autoGain.setEnabled(writable && !freeze.getToggleState());
        manualAnalyze.setEnabled(!analyzing);
        review.setEnabled(!analyzing);
        apply.setEnabled(writable && !analyzing && !pending.empty());
        audition.setEnabled(writable && !analyzing && !pending.empty());
        cancel.setEnabled(analyzing || !pending.empty());
        undo.setEnabled(writable && !analyzing && !plugin.history.empty());
    }

    std::array<float, 10> analysisSettings() const
    {
        return {plugin.value("instance"), plugin.value("session"), plugin.value("operation"), plugin.value("reference"),
                plugin.value("limit"), plugin.value("headroom"), plugin.value("learnTime"),
                plugin.value("sensitivity"), plugin.value("trueCeiling"), plugin.value("smartStrategy")};
    }

    void startAnalysis(bool previewOnly = false)
    {
        if (freeze.getToggleState() || (!previewOnly && analysisOnly.getToggleState()) || analyzing || plugin.value("bypass") > 0.5f)
            return;
        discard(plugin.hub()
            ? utf8("ANALYZER  /  Mantenha o trecho em reprodução. O trim será aplicado a cada pista ativa.")
            : utf8("ANALYZER  /  Mantenha a pista em reprodução. O trim local será ajustado."));
        smartAnalysing = previewOnly;
        requestedSettings = analysisSettings();
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            activeLearning = registry().nextLearningId++;
        }
        assignLearning(activeLearning);
        analysisStarted = juce::Time::getMillisecondCounter();
        analyzing = true;
        updateButtons();
        repaint();
    }

    juce::uint32 learningDuration() const
    {
        return static_cast<juce::uint32>(std::lround(plugin.value("learnTime") * 1000.0f));
    }

    void finishAnalysis()
    {
        if (!analyzing)
            return;
        if (freeze.getToggleState() || plugin.value("bypass") > 0.5f || requestedSettings != analysisSettings())
        {
            analyzing = false;
            assignLearning(0);
            status = utf8("ANALYZER cancelado após alteração das configurações.");
            return;
        }
        if (juce::Time::getMillisecondCounter() - analysisStarted < learningDuration())
            return;

        analyzing = false;
        if (!smartAnalysing || smartPanel.gainModule.getToggleState())
            prepareChanges(true);
        assignLearning(0);
        if (smartAnalysing)
        {
            if (smartPanel.toneModule.getToggleState())
                suggestTone(false);
            if (smartPanel.maskModule.getToggleState())
                suggestTone(true, smartPanel.toneModule.getToggleState());
            smartAnalysing = false;
            status = utf8("Análise concluída. Revise as propostas antes de aplicar.");
        }
        else if (!pending.empty())
            commit();
    }

    void refreshHub()
    {
        bool same = pick.getNumItems() == static_cast<int>(rows.size());
        for (int i = 0; same && i < pick.getNumItems(); ++i)
            same = pick.getItemId(i) == static_cast<int>(rows[static_cast<std::size_t>(i)].identity)
                && pick.getItemText(i) == rows[static_cast<std::size_t>(i)].name;
        if (!same)
        {
            const auto selected = pick.getSelectedId();
            pick.clear(juce::dontSendNotification);
            for (const auto& row : rows)
                pick.addItem(row.name, static_cast<int>(row.identity));
            pick.setSelectedId(selected, juce::dontSendNotification);
            if (pick.getSelectedId() == 0 && !rows.empty())
                pick.setSelectedItemIndex(0, juce::dontSendNotification);
        }

        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const auto& row = rows[static_cast<std::size_t>(i)];
            if (row.role == nx::focus && !row.referenceOnly && row.current && row.reading.active())
            {
                focusRow = i;
                break;
            }
        }
        if (focusRow < 0 && strategy() == nx::GainStrategy::hybrid)
        {
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            {
                const auto& row = rows[static_cast<std::size_t>(i)];
                if (!row.referenceOnly && row.current && row.reading.active()
                    && (focusRow < 0 || row.priority > rows[static_cast<std::size_t>(focusRow)].priority))
                    focusRow = i;
            }
        }
        nx::Overlap strongest;
        if (focusRow >= 0)
        {
            const auto& focus = rows[static_cast<std::size_t>(focusRow)];
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            {
                const auto& row = rows[static_cast<std::size_t>(i)];
                if (i == focusRow || !row.current)
                    continue;
                const auto overlap = nx::findOverlap(focus.reading, row.reading);
                if (overlap.strength > strongest.strength)
                {
                    strongest = overlap;
                    otherRow = i;
                }
            }
        }
        if (otherRow >= 0)
        {
            const auto band = strongest.band;
            tonalStatus = rows[static_cast<std::size_t>(otherRow)].name
                + " / " + rows[static_cast<std::size_t>(focusRow)].name
                + utf8("  ·  ") + juce::String(juce::roundToInt(nx::frequency(band))) + utf8("–")
                + juce::String(juce::roundToInt(nx::frequency(band + 1))) + " Hz";
        }
        else if (focusRow >= 0)
            tonalStatus = utf8("Aguardando sobreposição relevante em janelas sincronizadas.");
        else
            tonalStatus = "Marque uma pista ativa como Foco para comparar o espectro.";

    }

    float currentAnchor(bool learned) const
    {
        if (!needsAnchor() || focusRow < 0)
            return plugin.value("reference");
        const auto& focus = rows[static_cast<std::size_t>(focusRow)];
        return strategy() == nx::GainStrategy::musical
            ? nx::musicalRms(focus.reading, true, learned, focus.outputGainDb) + focus.trim
            : focus.reading.rms + focus.trim;
    }

    void updateSuggestions()
    {
        const auto anchor = currentAnchor(false);
        for (auto& row : rows)
            if (canSuggest(row))
            {
                row.diagnostics = diagnoseGain(row, anchor, false);
                row.proposed = row.diagnostics.suggestion;
            }
    }

    nx::SmartGainResult diagnoseGain(const TrackRow& row, float anchor, bool learned) const
    {
        auto reading = row.reading;
        const auto maxSample = std::max(reading.peak, learned ? reading.learnedMaxSample : nx::floorDb);
        if (learned && reading.learnedPeak > nx::floorDb)
            reading.peak = reading.learnedPeak;
        if (strategy() == nx::GainStrategy::hybrid)
        {
            if (learned && reading.learnedActiveWindows >= 4 && reading.learnedEnergy > nx::floorDb)
                reading.rms = 0.5f * (reading.rms + reading.learnedEnergy);
            else if (reading.activity >= 0.15f && reading.activeRms > reading.rms)
                reading.rms = 0.5f * (reading.rms + reading.activeRms);
        }
        const float liveTrue = nx::db(*std::max_element(reading.inputLevels.truePeak.begin(),
                                                       reading.inputLevels.truePeak.end()));
        const float maximum = std::max(liveTrue, learned ? reading.learnedCeiling : liveTrue);
        const float headroomDb = std::max(plugin.value("headroom"), -plugin.value("trueCeiling") + row.outputGainDb + row.toneBoost);
        float target = row.customPeak;
        if (target > 0 && row.peakProfile == 0 && row.instrument > 0)
            target = nx::presets[row.instrument - 1].target;
        const auto categoryId = row.instrument > 0 ? nx::presets[row.instrument - 1].category : 2;
        const auto chosenRole = strategy() == nx::GainStrategy::hybrid && focusRow >= 0
                && rows[static_cast<std::size_t>(focusRow)].identity == row.identity ? nx::focus : row.role;
        return nx::smartGain({reading, strategy(), chosenRole, row.function, categoryId, row.priority,
                              row.peakProfile, row.trim, target, anchor, plugin.value("limit"), headroomDb, maximum,
                              row.instrumentCeiling, maxSample, row.outputGainDb + row.toneBoost});
    }

    void capture()
    {
        discard({});
        timerCallback();
        prepareChanges(false);
    }

    void prepareChanges(bool automatic)
    {
        if (plugin.value("bypass") > 0.5f)
        {
            status = "Desative o bypass antes de capturar.";
            return;
        }
        const auto fresh = [&](const TrackRow& row)
        {
            const auto elapsed = row.timestamp - analysisStarted;
            return row.current && (!automatic || (elapsed >= learningDuration() - 200
                && elapsed <= juce::Time::getMillisecondCounter() - analysisStarted));
        };
        const auto usable = [&](const TrackRow& row)
        {
            return fresh(row) && canSuggest(row) && (!automatic
                || (row.reading.learningId == activeLearning && row.reading.learnedActiveWindows >= 4
                    && !(row.instrument > 0 && nx::presets[row.instrument - 1].category == 7)));
        };
        if (needsAnchor()
            && (focusRow < 0 || !fresh(rows[static_cast<std::size_t>(focusRow)])
                || (automatic && (rows[static_cast<std::size_t>(focusRow)].reading.learningId != activeLearning
                    || rows[static_cast<std::size_t>(focusRow)].reading.learnedActiveWindows < 4))))
        {
            status = "Escolha uma pista de Foco e reproduza um trecho ativo.";
            return;
        }
        const auto anchor = currentAnchor(automatic);
        int skipped = 0;
        capturedSession = plugin.session();
        capturedHub = plugin.hub();
        capturedHeadroom = plugin.value("headroom");
        const int scopeId = automatic || !capturedHub ? 3 : scope.getSelectedId();
        auto inScope = [&](const auto& row)
        {
            return scopeId == 1 ? row.identity == static_cast<unsigned>(pick.getSelectedId())
                : scopeId == 2 ? row.group == plugin.group() : true;
        };
        const bool commonCalibration = !automatic && capturedHub && strategy() == nx::GainStrategy::technical && scopeId != 1;
        nx::GroupCalibration calibration(plugin.value("limit"), capturedHeadroom);
        if (commonCalibration)
        {
            for (const auto& row : rows)
                if (inScope(row) && usable(row))
                    calibration.include(row.reading, row.trim);
            if (!calibration.possible())
            {
                status = utf8("Calibração em grupo indisponível. Verifique o sinal ativo e a margem de pico.");
                return;
            }
        }
        float commonStep = calibration.step(plugin.value("reference"));
        if (commonCalibration)
            for (const auto& row : rows)
                if (inScope(row) && usable(row))
                    commonStep = std::min(commonStep, diagnoseGain(row, anchor, false).permitted - row.trim);
        if (commonCalibration)
            commonStep = std::floor(commonStep * 100.0f + 0.0001f) / 100.0f;
        if (commonCalibration && commonStep < -plugin.value("limit") - 0.001f)
        {
            status = utf8("O teto de pico exige mais atenuação do que o limite de trim permite.");
            return;
        }
        for (const auto& row : rows)
        {
            if (!inScope(row))
                continue;
            const auto diagnosis = usable(row) ? diagnoseGain(row, anchor, automatic) : nx::SmartGainResult{};
            if (!usable(row) || (!commonCalibration && diagnosis.suggestion.headroomUnmet))
            {
                ++skipped;
                continue;
            }
            const float proposed = commonCalibration ? row.trim + commonStep : diagnosis.suggestion.trim;
            const float safe = std::min(-capturedHeadroom - row.reading.peak, diagnosis.permitted);
            const float rounded = std::round(proposed * 100.0f) / 100.0f;
            const float next = rounded <= safe ? rounded : std::floor(safe * 100.0f) / 100.0f;
            if (next < -24.0f || next > 24.0f)
            {
                ++skipped;
                continue;
            }
            if (std::abs(row.trim - next) >= 0.009f)
                pending.push_back({row.identity, row.trim, next, {}});
        }
        if (automatic)
        {
            status = pending.empty()
                ? utf8("Sem ajustes. Confira o sinal, o teto de pico e o limite de trim.")
                : juce::String{};
            return;
        }
        status = juce::String(commonCalibration ? utf8("Prévia de grupo: ajuste comum de ") + number(commonStep) + " dB. "
                                                : juce::String(utf8("Prévia fixa: ")))
            + juce::String(pending.size()) + utf8(" alterações, ") + juce::String(skipped)
            + " sem sinal ou sem margem. Compare em B ou aplique.";
        repaint();
    }

    bool stillApplies(const Change& change) const
    {
        auto* track = findTrack(change.identity);
        if (track == nullptr || !track->eligible(capturedSession) || !track->volumeUnlocked()
            || std::abs(track->value("trim") - change.previous) > 0.011f
            || (track->previewOwner != 0 && track->previewOwner != plugin.identity))
            return false;
        nx::Reading reading;
        if (!track->measurement.read(reading, capturedSession) || !reading.active())
            return false;
        const auto truePeak = nx::db(*std::max_element(reading.inputLevels.truePeak.begin(),
                                                       reading.inputLevels.truePeak.end()));
        const auto downstream = track->value("outputTrim") + track->tonalBoost();
        const auto ceiling = track->instrumentCeiling();
        return change.next + reading.peak <= -capturedHeadroom + 0.001f
            && (ceiling > 0 || change.next + downstream + reading.peak <= ceiling + 0.001f)
            && change.next + downstream + truePeak <= plugin.value("trueCeiling") + 0.001f;
    }

    void clearPreview()
    {
        for (auto* track : registry().tracks)
            if (track != nullptr && track->previewOwner == plugin.identity)
            {
                track->preview.store(noPreview);
                track->previewOwner = 0;
            }
        listening = false;
        audition.setButtonText("Ouvir B");
    }

    void discard(juce::String message)
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearPreview();
        pending.clear();
        status = std::move(message);
    }

    void togglePreview()
    {
        if (analysisOnly.getToggleState())
            return;
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (listening)
        {
            clearPreview();
            status = utf8("A  /  Trims aplicados antes da prévia.");
            return;
        }
        if (pending.empty() || !std::all_of(pending.begin(), pending.end(),
                                           [this](const auto& change) { return stillApplies(change); }))
        {
            status = utf8("Sinal, margem ou trim mudou. Capture novamente durante a reprodução.");
            return;
        }
        for (const auto& change : pending)
        {
            auto* track = findTrack(change.identity);
            if (track == nullptr)
                continue;
            track->previewSession.store(capturedSession);
            track->previewOwner = plugin.identity;
            track->preview.store(change.next);
        }
        listening = true;
        audition.setButtonText("Voltar a A");
        status = utf8("B  /  Trims em prévia, sem compensação de volume na comparação.");
    }

    void commit()
    {
        if (analysisOnly.getToggleState())
            return;
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (pending.empty() || !std::all_of(pending.begin(), pending.end(),
                                           [this](const auto& change) { return stillApplies(change); }))
        {
            clearPreview();
            status = utf8("Aplicação cancelada. Capture novamente com sinal e margem suficientes.");
            return;
        }
        for (auto& change : pending)
        {
            auto* track = findTrack(change.identity);
            change.previousApplied = track->lastAppliedTrim;
            track->setTrim(change.next);
            track->lastAppliedTrim = change.next;
        }
        clearPreview();
        const auto count = pending.size();
        plugin.redoHistory.clear();
        plugin.history.push_back(std::move(pending));
        pending.clear();
        status = juce::String(count) + " trims internos aplicados. Desfazer restaura os valores anteriores.";
    }

    void undoLast()
    {
        if (analysisOnly.getToggleState())
            return;
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (plugin.history.empty())
            return;
        clearPreview();
        pending.clear();
        auto& batch = plugin.history.back();
        const auto oldSize = batch.size();
        std::vector<Change> reverted;
        batch.erase(std::remove_if(batch.begin(), batch.end(), [&](const auto& change)
        {
            auto* track = findTrack(change.identity);
            if (track == nullptr || track->hub() || !track->volumeUnlocked() || track->previewOwner != 0
                || std::abs(track->value("trim") - change.next) > 0.011f)
                return false;
            track->setTrim(change.previous);
            track->lastAppliedTrim = change.previousApplied;
            reverted.push_back(change);
            return true;
        }), batch.end());
        const auto remaining = batch.size();
        if (!reverted.empty())
            plugin.redoHistory.push_back(std::move(reverted));
        if (batch.empty())
            plugin.history.pop_back();
        status = juce::String(oldSize - remaining) + " trims restaurados."
            + (remaining == 0 ? juce::String{} : utf8(" Há pistas indisponíveis ou com trim alterado."));
    }

    static constexpr int visibleRows = 6;
    static constexpr juce::uint32 amber = 0xffffb64a;
    int firstRow = 0;
    Processor& plugin;
    std::function<void(int)> requestEditorHeight;
    bool advanced = false;
    juce::TextButton advancedToggle;
    GlassLook glass;
    juce::TextButton smartToggle;
    nx::SmartPanel smartPanel {[this](nx::SmartPanel::Action action) { handleSmart(action); }};
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> smartAttachments;
    bool smartAnalysing = false;
    std::vector<unsigned> reportTracks;
    std::vector<std::pair<float, float>> busUndo, busRedo;
    juce::String smartStatus;
    juce::ComboBox spectrumMode, fftChoice;
    juce::TextButton normalise, spectralFreeze, spectrumOptions, tonalToggle;
    std::array<juce::ToggleButton, visibleRows> spectrumButtons, tonalButtons;
    nx::TonalPanel tonalPanel {[this](nx::TonalPanel::Action action) { handleTonal(action); }};
    std::set<unsigned> displayedSpectra, tonalTargets;
    std::unique_ptr<juce::FileChooser> tonalChooser;
    std::vector<TonalChange> tonalPending;
    int tonalSession = 0;
    bool tonalPreviewOn = false, tonalBypassed = false;
    nx::Spectrum tonalReference {};
    nx::ReferenceAudio referenceStats;
    juce::String referenceLabel;
    int referenceType = 0;
    unsigned referenceRequest = 0;
    nx::SpectrumFrame localSpectrum, frozenSpectrum;
    std::vector<TrackRow> frozenRows;
    bool spectralCurrent = false, frozenCurrent = false, smoothSpectrum = true;
    double graphStart = 0, graphSpan = 96;
    int averageMs = 300;
    juce::Point<float> graphCursor {-1, -1};
    juce::ComboBox instance, role, operation, scope, pick, support, peakReference, category, instruments;
    juce::Slider session, group, trim, reference, limit, trackLimit, headroom;
    juce::TextButton analyze, review, apply, audition, undo, cancel, analysisOnly;
    std::array<juce::ToggleButton, visibleRows> principalButtons;
    juce::ToggleButton bypass, freeze, tonalExclude;
    juce::TextButton manualAnalyze, autoGain, resetHold;
    juce::Slider outputTrim, targetPeak, learnTime, sensitivity, trueCeiling;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> choices;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliders;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment, freezeAttachment, tonalExcludeAttachment;
    std::vector<TrackRow> rows;
    std::vector<Change> pending;
    nx::Reading local;
    bool localCurrent = false, listening = false, analyzing = false;
    std::array<float, 10> requestedSettings {};
    juce::uint32 analysisStarted = 0;
    unsigned activeLearning = 0;
    int focusRow = -1, otherRow = -1, capturedSession = 0;
    float capturedHeadroom = 0;
    bool capturedHub = false;
    juce::String status = "Reproduza um trecho representativo antes de capturar.";
    juce::String tonalStatus;
};

class ResizableEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ResizableEditor(Processor& processor)
        : AudioProcessorEditor(processor), content(processor, [this](int height) { switchLayout(height); })
    {
        addAndMakeVisible(content);
        setResizable(true, true);
        switchLayout(content.getHeight());
    }

    void resized() override
    {
        const auto scale = std::min(getWidth() / 1050.0f, static_cast<float>(getHeight()) / content.getHeight());
        content.setTransform(juce::AffineTransform::scale(scale));
    }

private:
    void switchLayout(int height)
    {
        const auto width = getWidth() > 0 ? std::clamp(getWidth(), 788, 3150) : 1050;
        setResizeLimits(788, (height * 3 + 3) / 4, 3150, height * 3);
        getConstrainer()->setFixedAspectRatio(1050.0 / height);
        setSize(width, juce::roundToInt(width * height / 1050.0));
    }

    Editor content;
};

juce::AudioProcessorEditor* Processor::createEditor() { return new ResizableEditor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Processor; }
