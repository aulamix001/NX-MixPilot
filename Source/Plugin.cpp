#include "Analysis.h"
#include "GlassLook.h"
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

struct Measurement
{
    void write(const nx::Reading& reading, int group)
    {
        ++version;
        rms.store(reading.rms);
        peak.store(reading.peak);
        outputPeak.store(reading.outputPeak);
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
        reading.peak = peak.load();
        reading.outputPeak = outputPeak.load();
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

    std::atomic<unsigned> version {0};
    std::atomic<float> rms {nx::floorDb}, peak {nx::floorDb}, outputPeak {nx::floorDb};
    std::atomic<bool> settled {false};
    std::atomic<int> sampleRate {0}, session {0};
    std::atomic<std::int64_t> position {-1};
    std::atomic<juce::uint32> stamp {0};
    std::array<std::atomic<float>, nx::bands> power {};
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
    unsigned nextIdentity = 1;
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
            hostTrackName = properties.name->trim();
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

    bool eligible(int sessionId) const { return !hub() && session() == sessionId && value("bypass") < 0.5f; }

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
        gain.reset(rate, 0.035);
        gain.setCurrentAndTargetValue(1);
        settings = -1;
    }

    void releaseResources() override { measurement.write({}, 0); }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (buffer.getNumChannels() == 0)
            return;
        const auto sessionId = session();
        const auto isHub = hub();
        const int newSettings = sessionId * 2 + static_cast<int>(isHub);
        if (settings != newSettings)
        {
            analyzer.reset();
            measurement.write({}, 0);
            settings = newSettings;
        }

        const auto audition = previewSession.load() == sessionId ? preview.load() : noPreview;
        const bool bypass = value("bypass") >= 0.5f;
        const float trim = isHub || bypass ? 0.0f : audition == noPreview ? value("trim") : audition;
        gain.setTargetValue(juce::Decibels::decibelsToGain(trim));

        std::int64_t hostPosition = -1;
        if (auto* playhead = getPlayHead())
            if (auto position = playhead->getPosition(); position && position->getIsPlaying())
                if (auto samples = position->getTimeInSamples(); samples && *samples >= 0)
                    hostPosition = *samples;

        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() == 2 ? buffer.getWritePointer(1) : nullptr;
        nx::Reading reading;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float dryLeft = left[sample], dryRight = right ? right[sample] : dryLeft;
            const float ramp = gain.getNextValue();
            const float applied = bypass || isHub ? 1.0f : ramp;
            left[sample] = dryLeft * applied;
            if (right)
                right[sample] = dryRight * applied;
            const auto position = hostPosition < 0 ? -1 : hostPosition + sample;
            if (analyzer.push(dryLeft, dryRight, left[sample], dryRight * applied, position, reading))
                measurement.write(reading, sessionId);
        }
    }

    juce::AudioProcessorValueTreeState parameters;
    Measurement measurement;
    std::atomic<float> preview {noPreview};
    std::atomic<int> previewSession {0};
    unsigned identity = 0;
    int slot = -1;
    std::optional<float> lastAppliedTrim; // Message thread.
    std::vector<std::vector<Change>> history; // Message thread.
    unsigned previewOwner = 0; // Accessed under the registry mutex.

private:
    juce::String hostTrackName; // VST3 channel-context callbacks and the editor use the message thread.
    nx::Analyzer analyzer;
    juce::SmoothedValue<float> gain {1.0f};
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

class Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor(Processor& processor)
        : AudioProcessorEditor(processor), plugin(processor)
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
        configureButton(analyze, "ANALYZER", [this] { startAnalysis(); });
        analyze.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff14627a));
        analyze.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff167243));
        analyze.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        analyze.setTooltip(utf8("Analisa e aplica o trim individual de cada pista ativa da sessão."));
        configureButton(review, utf8("Capturar prévia"), [this] { capture(); });
        configureButton(apply, "Aplicar trim", [this] { commit(); });
        apply.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff135f72));
        configureButton(audition, "Ouvir B", [this] { togglePreview(); });
        configureButton(undo, "Desfazer", [this] { undoLast(); });
        configureButton(cancel, "Cancelar", [this]
        {
            const bool wasAnalyzing = analyzing;
            analyzing = false;
            discard(wasAnalyzing ? utf8("Análise cancelada.") : utf8("Prévia cancelada."));
            updateButtons();
        });
        configureButton(analysisOnly, utf8("Somente análise"), [this]
        {
            if (analysisOnly.getToggleState())
            {
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
        bypass.setButtonText("Bypass");
        addAndMakeVisible(bypass);
        bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            plugin.parameters, "bypass", bypass);

        setSize(1050, 710);
        timerCallback();
        startTimerHz(12);
    }

    ~Editor() override
    {
        stopTimer();
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

        analyze.setBounds(16, 652, 127, 34);
        review.setBounds(153, 652, 146, 34);
        audition.setBounds(309, 652, 116, 34);
        cancel.setBounds(435, 652, 99, 34);
        analysisOnly.setBounds(551, 652, 181, 34);
        undo.setBounds(753, 652, 119, 34);
        apply.setBounds(882, 652, 152, 34);
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
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
            pick.setSelectedId(static_cast<int>(rows[static_cast<std::size_t>(index)].identity),
                               juce::sendNotificationSync);
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
            label(g, utf8("HUB controla a sessão pelo master."), 681, 113, 348, 27, 13);
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
        label(g, status, 17, 685, 1016, 18, 13, analyzing ? 0xff63f69e : ink);
    }

private:
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
            label(g, utf8("Pré-master · referência de pico: −6 dBFS"), 856, 363, 170, 23, 10, cyan);
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

    void drawSpectrum(juce::Graphics& g) const
    {
        const juce::Rectangle<float> plot(62, 194, 612, 163);
        g.setColour(juce::Colour(0x9c080f17));
        g.fillRect(plot);
        const auto xFor = [&](float hz) { return plot.getX() + plot.getWidth() * std::log10(hz / 20.0f) / 3.0f; };
        for (int decade = 0; decade < 3; ++decade)
            for (int division = 1; division < 10; ++division)
            {
                g.setColour(juce::Colour(division == 1 ? 0xff344959 : 0xff23313e));
                const int x = juce::roundToInt(xFor(20.0f * division * std::pow(10.0f, decade)));
                g.drawVerticalLine(x, plot.getY(), plot.getBottom());
            }
        for (int i = 0; i <= 4; ++i)
        {
            const auto y = juce::roundToInt(plot.getY() + plot.getHeight() * i / 4.0f);
            g.setColour(juce::Colour(0xff344959));
            g.drawHorizontalLine(y, plot.getX(), plot.getRight());
            label(g, juce::String(-20 * i), 27, y - 8, 29, 16, 11);
        }
        const auto hasSpectrum = [](const nx::Reading& reading)
        {
            return std::any_of(reading.power.begin(), reading.power.end(),
                               [](float power) { return power > 1.0e-8f; });
        };
        const auto trace = [&](const nx::Reading& reading, juce::Colour color, bool secondary)
        {
            std::array<float, nx::bands> y {}, slope {};
            const float pitch = plot.getWidth() / nx::bands;
            const float firstX = plot.getX() + pitch * 0.5f;
            const float lastX = firstX + pitch * (nx::bands - 1);
            for (int i = 0; i < nx::bands; ++i)
            {
                const auto left = reading.power[std::max(0, i - 1)];
                const auto right = reading.power[std::min(nx::bands - 1, i + 1)];
                const auto power = 0.80f * reading.power[i] + 0.10f * (left + right);
                const auto decibels = nx::db(std::sqrt(power));
                y[i] = plot.getY() + plot.getHeight() * std::clamp(-decibels / 80.0f, 0.0f, 1.0f);
            }
            for (int i = 1; i < nx::bands - 1; ++i)
            {
                const auto left = y[i] - y[i - 1], right = y[i + 1] - y[i];
                // A bounded slope keeps each segment within its two measured endpoints.
                if (left * right > 0)
                    slope[i] = (2 * left * right) / (left + right);
            }
            juce::Path curve;
            curve.startNewSubPath(firstX, y.front());
            for (int i = 0; i < nx::bands - 1; ++i)
            {
                const auto x = firstX + i * pitch;
                curve.cubicTo(x + pitch / 3.0f, y[i] + slope[i] / 3.0f,
                              x + 2.0f * pitch / 3.0f, y[i + 1] - slope[i + 1] / 3.0f,
                              x + pitch, y[i + 1]);
            }
            auto fill = curve;
            fill.lineTo(lastX, plot.getBottom());
            fill.lineTo(firstX, plot.getBottom());
            fill.closeSubPath();
            g.setGradientFill({color.withAlpha(secondary ? 0.08f : 0.33f), plot.getX(), plot.getY() + 40.0f,
                               color.withAlpha(secondary ? 0.006f : 0.025f), plot.getX(), plot.getBottom(), false});
            g.fillPath(fill);
            if (!secondary)
            {
                g.setColour(color.withAlpha(0.16f));
                g.strokePath(curve, juce::PathStrokeType(5));
            }
            g.setColour(color.withAlpha(secondary ? 0.64f : 1.0f));
            g.strokePath(curve, juce::PathStrokeType(secondary ? 1.1f : 1.65f));
        };
        bool drewTrace = false;
        const auto* principal = focusRow >= 0 ? &rows[static_cast<std::size_t>(focusRow)] : nullptr;
        if (plugin.hub())
            for (const auto& row : rows)
                if (&row != principal && row.current && hasSpectrum(row.reading))
                {
                    trace(row.reading, spectrumColour(row), true);
                    drewTrace = true;
                }

        if (localCurrent && hasSpectrum(local))
        {
            const auto mixColor = juce::Colour(plugin.hub() ? 0xffa9bed3 : cyan);
            trace(local, mixColor, drewTrace);
            drewTrace = true;
            label(g, plugin.hub() ? "Mix bus" : "Entrada", 581, 161, 90, 24, 12, mixColor.getARGB());
        }
        if (principal != nullptr && hasSpectrum(principal->reading))
        {
            trace(principal->reading, spectrumColour(*principal), false);
            drewTrace = true;
            label(g, principal->name + " (Principal)", 385, 161, 181, 24, 12, cyan);
        }
        if (!drewTrace)
            label(g, "Aguardando sinal", 309, 257, 168, 20, 14, ink);

        constexpr std::array<float, 7> ticks {20, 60, 200, 600, 2000, 6000, 20000};
        const std::array<juce::String, 7> captions {"20 Hz", "60", "200", "600", "2 kHz", "6 kHz", "20 kHz"};
        for (std::size_t i = 0; i < ticks.size(); ++i)
            label(g, captions[i], std::min(636, juce::roundToInt(xFor(ticks[i])) - 10), 360, 57, 20, 11);
    }

    void drawRows(juce::Graphics& g)
    {
        const auto selected = static_cast<unsigned>(pick.getSelectedId());
        label(g, "Pista", 34, 428, 130, 16, 11);
        label(g, "RMS dBFS", 248, 428, 68, 16, 11);
        label(g, "Pico dBFS", 327, 428, 68, 16, 11);
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
            drawSegments(g, {183, y + 9, 51, 10}, signal ? row.reading.rms : nx::floorDb, 10, false);
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
        return row.current && row.reading.active() && (mode() == nx::GainMode::calibration || focusRow >= 0);
    }

    void timerCallback() override
    {
        if ((plugin.hub() != capturedHub || plugin.session() != capturedSession || plugin.value("bypass") > 0.5f)
            && !pending.empty())
            discard(utf8("Prévia cancelada após alteração de sessão, instância ou bypass."));

        const bool isHub = plugin.hub();
        operation.setVisible(isHub);
        scope.setVisible(isHub);
        limit.setVisible(isHub);
        reference.setVisible(mode() == nx::GainMode::calibration);
        reference.setBounds(isHub ? 474 : 95, isHub ? 75 : 113, 119, 27);
        headroom.setBounds(isHub ? 671 : 304, isHub ? 75 : 113, 118, 27);
        trackLimit.setVisible(!isHub);
        pick.setVisible(isHub && scope.getSelectedId() == 1);
        group.setVisible(!isHub || scope.getSelectedId() == 2);
        group.setBounds(isHub ? 222 : 459, isHub ? 114 : 75, 125, 26);
        role.setVisible(!isHub);
        support.setVisible(!isHub);
        peakReference.setVisible(!isHub);
        support.setEnabled(plugin.role() == nx::automatic || plugin.role() == nx::music);
        trim.setVisible(!isHub);

        localCurrent = plugin.measurement.read(local, plugin.session());
        rows.clear();
        focusRow = otherRow = -1;
        auto append = [&](Processor& track)
        {
            TrackRow row {track.identity, track.group(), track.musicalRole(), track.value("trim"), false, {}, {}, 0, track.displayName()};
            row.current = track.eligible(plugin.session())
                && track.measurement.read(row.reading, plugin.session(), &row.timestamp);
            row.peakProfile = static_cast<int>(track.value("peakReference"));
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

        finishAnalysis();
        updateButtons();
        repaint();
    }

    void updatePrincipalButtons()
    {
        for (int i = 0; i < visibleRows; ++i)
        {
            auto& button = principalButtons[static_cast<std::size_t>(i)];
            const auto index = static_cast<std::size_t>(firstRow + i);
            const bool present = plugin.hub() && index < rows.size();
            button.setVisible(present);
            if (present)
            {
                const auto& row = rows[index];
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
            plugin.setParameter("operation", 1);
        }
        analyzing = false;
        discard(rows[index].name + utf8(" é a Principal. Reproduza o trecho e clique em ANALYZER."));
        timerCallback();
    }

    void updateButtons()
    {
        analyze.setToggleState(analyzing, juce::dontSendNotification);
        const bool writable = !analysisOnly.getToggleState() && plugin.value("bypass") < 0.5f;
        analyze.setEnabled(writable);
        review.setEnabled(!analyzing);
        apply.setEnabled(writable && !analyzing && !pending.empty());
        audition.setEnabled(writable && !analyzing && !pending.empty());
        cancel.setEnabled(analyzing || !pending.empty());
        undo.setEnabled(writable && !analyzing && !plugin.history.empty());
    }

    std::array<float, 6> analysisSettings() const
    {
        return {plugin.value("instance"), plugin.value("session"), plugin.value("operation"), plugin.value("reference"),
                plugin.value("limit"), plugin.value("headroom")};
    }

    void startAnalysis()
    {
        if (analysisOnly.getToggleState() || analyzing || plugin.value("bypass") > 0.5f)
            return;
        discard(plugin.hub()
            ? utf8("ANALYZER  /  Mantenha o trecho em reprodução. O trim será aplicado a cada pista ativa.")
            : utf8("ANALYZER  /  Mantenha a pista em reprodução. O trim local será ajustado."));
        requestedSettings = analysisSettings();
        analysisStarted = juce::Time::getMillisecondCounter();
        analyzing = true;
        updateButtons();
        repaint();
    }

    void finishAnalysis()
    {
        if (!analyzing)
            return;
        if (plugin.value("bypass") > 0.5f || requestedSettings != analysisSettings())
        {
            analyzing = false;
            status = utf8("ANALYZER cancelado após alteração das configurações.");
            return;
        }
        if (juce::Time::getMillisecondCounter() - analysisStarted < 1200)
            return;

        analyzing = false;
        prepareChanges(true);
        if (!pending.empty())
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
            if (row.role == nx::focus && row.current && row.reading.active())
            {
                focusRow = i;
                break;
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

    void updateSuggestions()
    {
        const auto focusAnchor = focusRow >= 0
            ? rows[static_cast<std::size_t>(focusRow)].reading.rms + rows[static_cast<std::size_t>(focusRow)].trim : 0.0f;
        const auto anchor = mode() == nx::GainMode::calibration ? plugin.value("reference") : focusAnchor;
        for (auto& row : rows)
            if (canSuggest(row))
                row.proposed = nx::suggestTrim(row.reading, mode(), anchor, row.role,
                                               plugin.value("limit"), plugin.value("headroom"), row.peakProfile);
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
            return row.current && (!automatic || (elapsed >= 1000
                && elapsed <= juce::Time::getMillisecondCounter() - analysisStarted));
        };
        const auto usable = [&](const TrackRow& row) { return fresh(row) && canSuggest(row); };
        if (mode() == nx::GainMode::musical
            && (focusRow < 0 || !fresh(rows[static_cast<std::size_t>(focusRow)])))
        {
            status = "Escolha uma pista de Foco e reproduza um trecho ativo.";
            return;
        }
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
        const bool commonCalibration = !automatic && capturedHub && mode() == nx::GainMode::calibration && scopeId != 1;
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
        const float commonStep = calibration.step(plugin.value("reference"));
        for (const auto& row : rows)
        {
            if (!inScope(row))
                continue;
            if (!usable(row) || (!commonCalibration && row.proposed.headroomUnmet))
            {
                ++skipped;
                continue;
            }
            const float proposed = commonCalibration ? row.trim + commonStep : row.proposed.trim;
            const float safe = -capturedHeadroom - row.reading.peak;
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
                ? utf8("ANALYZER concluído sem ajustes. Confira o sinal, a referência e a margem de pico.")
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
        if (track == nullptr || !track->eligible(capturedSession)
            || std::abs(track->value("trim") - change.previous) > 0.011f
            || (track->previewOwner != 0 && track->previewOwner != plugin.identity))
            return false;
        nx::Reading reading;
        return track->measurement.read(reading, capturedSession) && reading.active()
            && change.next + reading.peak <= -capturedHeadroom + 0.001f;
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
        batch.erase(std::remove_if(batch.begin(), batch.end(), [](const auto& change)
        {
            auto* track = findTrack(change.identity);
            if (track == nullptr || track->hub() || track->previewOwner != 0
                || std::abs(track->value("trim") - change.next) > 0.011f)
                return false;
            track->setTrim(change.previous);
            track->lastAppliedTrim = change.previousApplied;
            return true;
        }), batch.end());
        const auto remaining = batch.size();
        if (batch.empty())
            plugin.history.pop_back();
        status = juce::String(oldSize - remaining) + " trims restaurados."
            + (remaining == 0 ? juce::String{} : utf8(" Há pistas indisponíveis ou com trim alterado."));
    }

    static constexpr int visibleRows = 6;
    static constexpr juce::uint32 amber = 0xffffb64a;
    int firstRow = 0;
    Processor& plugin;
    GlassLook glass;
    juce::ComboBox instance, role, operation, scope, pick, support, peakReference;
    juce::Slider session, group, trim, reference, limit, trackLimit, headroom;
    juce::TextButton analyze, review, apply, audition, undo, cancel, analysisOnly;
    std::array<juce::ToggleButton, visibleRows> principalButtons;
    juce::ToggleButton bypass;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> choices;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliders;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::vector<TrackRow> rows;
    std::vector<Change> pending;
    nx::Reading local;
    bool localCurrent = false, listening = false, analyzing = false;
    std::array<float, 6> requestedSettings {};
    juce::uint32 analysisStarted = 0;
    int focusRow = -1, otherRow = -1, capturedSession = 0;
    float capturedHeadroom = 0;
    bool capturedHub = false;
    juce::String status = "Reproduza um trecho representativo antes de capturar.";
    juce::String tonalStatus;
};

juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Processor; }
