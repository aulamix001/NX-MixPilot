#include "Analysis.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <mutex>
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

    bool read(nx::Reading& reading, int group) const
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
        const auto age = juce::Time::getMillisecondCounter() - stamp.load();
        return session.load() == group && age < 500 && before == version.load();
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
        return result;
    }

    float value(const char* name) const { return parameters.getRawParameterValue(name)->load(); }
    int session() const { return static_cast<int>(value("session")); }
    bool hub() const { return value("instance") > 0.5f; }
    nx::Role role() const { return static_cast<nx::Role>(static_cast<int>(value("role"))); }
    int group() const { return static_cast<int>(value("group")); }

    void setTrim(float dB)
    {
        auto* parameter = parameters.getParameter("trim");
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(dB));
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
    std::vector<std::vector<Change>> history; // Message thread.
    unsigned previewOwner = 0; // Accessed under the registry mutex.

private:
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

juce::String trackName(unsigned identity)
{
    return "Pista " + juce::String(identity);
}

class Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor(Processor& processor)
        : AudioProcessorEditor(processor), plugin(processor)
    {
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
        attachChoice(operation, "operation", {utf8("Calibração técnica"), utf8("Balanço musical")});
        attachSlider(session, "session", "");
        attachSlider(group, "group", "");
        attachSlider(trim, "trim", " dB");
        attachSlider(reference, "reference", " dBFS");
        attachSlider(limit, "limit", " dB");
        attachSlider(headroom, "headroom", " dB");

        scope.addItemList({"Uma pista", "Um grupo", "Todas as pistas"}, 1);
        scope.setSelectedId(1);
        addAndMakeVisible(scope);
        addAndMakeVisible(pick);
        configureButton(review, utf8("Capturar prévia"), [this] { capture(); });
        configureButton(apply, "Aplicar trim", [this] { commit(); });
        configureButton(audition, "Ouvir B", [this] { togglePreview(); });
        configureButton(undo, "Desfazer", [this] { undoLast(); });
        configureButton(cancel, utf8("Cancelar prévia"), [this] { discard(utf8("Prévia cancelada.")); });
        bypass.setButtonText("Bypass");
        addAndMakeVisible(bypass);
        bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            plugin.parameters, "bypass", bypass);

        setSize(1050, 685);
        timerCallback();
        startTimerHz(12);
    }

    ~Editor() override
    {
        stopTimer();
        std::lock_guard<std::mutex> lock(registry().mutex);
        clearPreview();
    }

    void resized() override
    {
        instance.setBounds(845, 25, 100, 30);
        bypass.setBounds(954, 25, 85, 30);
        session.setBounds(95, 82, 160, 28);
        operation.setBounds(282, 82, 188, 28);
        reference.setBounds(588, 82, 143, 28);
        limit.setBounds(826, 82, 191, 28);
        role.setBounds(345, 82, 125, 28);
        trim.setBounds(588, 82, 143, 28);
        group.setBounds(826, 120, 191, 28);
        headroom.setBounds(588, 120, 143, 28);
        scope.setBounds(24, 120, 140, 28);
        pick.setBounds(174, 120, 180, 28);
        review.setBounds(24, 604, 155, 32);
        audition.setBounds(189, 604, 115, 32);
        cancel.setBounds(314, 604, 150, 32);
        undo.setBounds(690, 604, 120, 32);
        apply.setBounds(820, 604, 197, 32);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c131d));
        g.setColour(juce::Colour(0xff152131));
        g.fillRect(0, 0, getWidth(), 66);
        label(g, "NX AUDIO   /   MixPilot", 24, 12, 490, 48, 31, cyan);
        label(g, plugin.hub() ? utf8("HUB   ·   ") + juce::String(rows.size()) + utf8(" pistas na sessão")
                              : trackName(plugin.identity) + utf8("   ·   Track"),
              533, 16, 301, 42, 17, ink);
        label(g, utf8("Sessão"), 24, 82, 65, 28);
        label(g, plugin.hub() ? "Limite de trim" : "Grupo", 742, plugin.hub() ? 82 : 120, 82, 28);
        if (plugin.hub())
        {
            label(g, mode() == nx::GainMode::calibration ? "0 VU (RMS)" : utf8("Âncora: nível atual do Foco"), 488, 82, 239, 28);
            label(g, "Headroom", 488, 120, 92, 28);
            if (scope.getSelectedId() == 2)
                label(g, "Grupo", 742, 120, 82, 28);
        }
        else
        {
            label(g, "Papel", 282, 82, 60, 28);
            label(g, "Trim interno", 488, 82, 100, 28);
        }

        panel(g, 24, 168, 675, 205);
        panel(g, 715, 168, 302, 205);
        label(g, utf8("ESPECTRO  /  20 Hz — 20 kHz"), 39, 178, 640, 26, 17, cyan);
        label(g, plugin.hub() ? "MIX BUS" : utf8("MEDIÇÃO DA PISTA"), 730, 178, 275, 26, 17, cyan);
        drawSpectrum(g);
        if (localCurrent)
        {
            label(g, utf8("Nível RMS     ") + number(local.rms) + " dBFS", 730, 211, 265, 25, 16, ink);
            label(g, "Pico entrada     " + number(local.peak) + " dBFS", 730, 241, 265, 25);
            label(g, utf8("Pico saída       ") + number(local.outputPeak) + " dBFS", 730, 268, 265, 25);
            label(g, utf8("Margem saída   ") + number(-local.outputPeak) + " dB", 730, 295, 265, 25);
        }
        else
            label(g, "Sem dados recentes", 730, 215, 265, 28);
        label(g, "Medidores de pico de amostra", 730, 332, 265, 25);

        panel(g, 24, 386, 993, 203);
        if (plugin.hub())
        {
            label(g, "PISTAS   /   TRIM INTERNO", 39, 394, 460, 26, 17, cyan);
            label(g, "RMS (dBFS)", 233, 422, 126, 23);
            label(g, "Pico (dBFS)", 365, 422, 120, 23);
            label(g, utf8("Sugestão (dB)"), 491, 422, 125, 23);
            label(g, "Aplicado (dB)", 628, 422, 120, 23);
            label(g, "Estado", 750, 422, 250, 23);
            drawRows(g);
        }
        else
        {
            label(g, "TRACK  /  TRIM INTERNO", 39, 398, 940, 30, 17, cyan);
            label(g, utf8("Escolha um grupo e use o HUB na mesma sessão para calibrar ou balancear."), 39, 446, 940, 27);
            label(g, utf8("Auto reconhece predominância de graves. Para definir a prioridade musical, selecione Foco."), 39, 478, 940, 27);
            label(g, utf8("A medição usa RMS digital e pico de amostra."), 39, 510, 940, 27);
        }
        label(g, status, 24, 640, 993, 30, 14, ink);
    }

private:
    nx::GainMode mode() const
    {
        return plugin.value("operation") < 0.5f ? nx::GainMode::calibration : nx::GainMode::musical;
    }

    static void label(juce::Graphics& g, const juce::String& text, int x, int y, int w, int h,
                      float size = 14, juce::uint32 colour = muted)
    {
        g.setColour(juce::Colour(colour));
        g.setFont(juce::Font(juce::FontOptions(size)));
        g.drawFittedText(text, x, y, w, h, juce::Justification::centredLeft, 1);
    }

    static void panel(juce::Graphics& g, int x, int y, int w, int h)
    {
        juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y),
                                    static_cast<float>(w), static_cast<float>(h));
        g.setColour(juce::Colour(0xff121e2b));
        g.fillRoundedRectangle(area, 8);
        g.setColour(juce::Colour(0xff304052));
        g.drawRoundedRectangle(area, 8, 1);
    }

    void drawSpectrum(juce::Graphics& g) const
    {
        const juce::Rectangle<float> plot(50, 212, 625, 116);
        for (int i = 0; i <= 6; ++i)
        {
            g.setColour(juce::Colour(0xff2a3b4a));
            const auto x = plot.getX() + plot.getWidth() * i / 6.0f;
            g.drawVerticalLine(static_cast<int>(x), plot.getY(), plot.getBottom());
        }
        for (int i = 0; i <= 4; ++i)
        {
            g.setColour(juce::Colour(0xff2a3b4a));
            const auto y = plot.getY() + plot.getHeight() * i / 4.0f;
            g.drawHorizontalLine(static_cast<int>(y), plot.getX(), plot.getRight());
        }
        auto trace = [&](const nx::Reading& reading, juce::uint32 color)
        {
            juce::Path curve;
            for (int i = 0; i < nx::bands; ++i)
            {
                const float decibels = nx::db(std::sqrt(reading.power[i]));
                const auto x = plot.getX() + plot.getWidth() * (i + 0.5f) / nx::bands;
                const auto y = plot.getY() + plot.getHeight() * std::clamp(-decibels / 80.0f, 0.0f, 1.0f);
                if (i == 0)
                    curve.startNewSubPath(x, y);
                else
                    curve.lineTo(x, y);
            }
            g.setColour(juce::Colour(color));
            g.strokePath(curve, juce::PathStrokeType(2));
        };
        if (focusRow >= 0)
            trace(rows[static_cast<std::size_t>(focusRow)].reading, cyan);
        else if (localCurrent)
            trace(local, cyan);
        if (otherRow >= 0)
            trace(rows[static_cast<std::size_t>(otherRow)].reading, 0xffffb84d);
        label(g, "20 Hz", 51, 310, 65, 17);
        label(g, "200 Hz", 246, 310, 63, 17);
        label(g, "2 kHz", 451, 310, 63, 17);
        label(g, "20 kHz", 607, 310, 67, 17);
        label(g, tonalStatus, 39, 337, 640, 25);
    }

    void drawRows(juce::Graphics& g)
    {
        const auto selected = static_cast<unsigned>(pick.getSelectedId());
        auto rowIndex = std::find_if(rows.begin(), rows.end(), [selected](const auto& row)
            { return row.identity == selected; });
        const auto start = std::max<std::ptrdiff_t>(0, rowIndex - rows.begin() - 3);
        const auto first = std::min(start, std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(rows.size()) - 5));
        int y = 450;
        if (rows.empty())
            label(g, utf8("Insira o MixPilot em modo Track nas pistas e use o mesmo número de sessão."), 39, y, 930, 25);
        for (auto index = first; index < static_cast<std::ptrdiff_t>(rows.size()) && y <= 554; ++index, y += 26)
        {
            const auto& row = rows[static_cast<std::size_t>(index)];
            if (row.identity == selected)
            {
                g.setColour(juce::Colour(0xff1e3445));
                g.fillRect(34, y, 969, 25);
            }
            auto tag = row.role == nx::focus ? utf8(" · Foco") : "";
            label(g, trackName(row.identity) + tag + "  /  G" + juce::String(row.group), 39, y, 182, 25, 14, ink);
            label(g, row.current ? number(row.reading.rms) : utf8("—"), 233, y, 122, 25);
            label(g, row.current ? number(row.reading.peak) : utf8("—"), 365, y, 122, 25);
            const auto frozen = std::find_if(pending.begin(), pending.end(),
                [&](const auto& change) { return change.identity == row.identity; });
            const auto suggestion = pending.empty() ? (canSuggest(row) ? number(row.proposed.trim) : utf8("—"))
                : frozen == pending.end() ? juce::String(utf8("—")) : number(frozen->next);
            label(g, suggestion, 491, y, 125, 25, 14, frozen != pending.end() ? cyan : muted);
            label(g, number(row.trim), 628, y, 120, 25);
            const juce::String state = !row.current ? "Sem dados / bypass" : !row.reading.active() ? "Abaixo do limiar"
                : row.proposed.headroomUnmet ? utf8("Margem inviável no limite") : row.proposed.headroomLimited ? "Limitado pelo pico" : "Sinal ativo";
            label(g, state, 750, y, 250, 25);
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
        if ((!plugin.hub() || plugin.session() != capturedSession || plugin.value("bypass") > 0.5f)
            && !pending.empty())
            discard(utf8("Prévia cancelada após alteração de sessão, instância ou bypass."));

        const bool isHub = plugin.hub();
        for (auto* control : std::initializer_list<juce::Component*>{
                  &operation, &reference, &headroom, &limit, &scope, &review, &apply, &cancel, &audition, &undo})
            control->setVisible(isHub);
        reference.setVisible(isHub && mode() == nx::GainMode::calibration);
        pick.setVisible(isHub && scope.getSelectedId() == 1);
        group.setVisible(!isHub || scope.getSelectedId() == 2);
        role.setVisible(!isHub);
        trim.setVisible(!isHub);
        apply.setEnabled(!pending.empty());
        audition.setEnabled(!pending.empty());
        cancel.setEnabled(!pending.empty());
        undo.setEnabled(!plugin.history.empty());

        localCurrent = plugin.measurement.read(local, plugin.session());
        rows.clear();
        focusRow = otherRow = -1;
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            if (isHub)
                for (auto* track : registry().tracks)
                    if (track != nullptr && !track->hub() && track->session() == plugin.session())
                    {
                        TrackRow row {track->identity, track->group(), track->role(), track->value("trim"), false, {}, {}};
                        row.current = track->eligible(plugin.session()) && track->measurement.read(row.reading, plugin.session());
                        rows.push_back(row);
                    }
        }
        if (isHub)
            refreshHub();
        else
            tonalStatus = localCurrent ? "Espectro da entrada (dBFS por banda)" : "Reproduza um trecho com sinal.";
        repaint();
    }

    void refreshHub()
    {
        bool same = pick.getNumItems() == static_cast<int>(rows.size());
        for (int i = 0; same && i < pick.getNumItems(); ++i)
            same = pick.getItemId(i) == static_cast<int>(rows[static_cast<std::size_t>(i)].identity);
        if (!same)
        {
            const auto selected = pick.getSelectedId();
            pick.clear(juce::dontSendNotification);
            for (const auto& row : rows)
                pick.addItem(trackName(row.identity), static_cast<int>(row.identity));
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
            tonalStatus = utf8("Sobreposição instantânea  ·  ") + trackName(rows[static_cast<std::size_t>(otherRow)].identity)
                + " / " + trackName(rows[static_cast<std::size_t>(focusRow)].identity)
                + utf8("  ·  ") + juce::String(juce::roundToInt(nx::frequency(band))) + utf8("–")
                + juce::String(juce::roundToInt(nx::frequency(band + 1))) + " Hz";
        }
        else if (focusRow >= 0)
            tonalStatus = utf8("Aguardando sobreposição relevante em janelas sincronizadas.");
        else
            tonalStatus = "Marque uma pista ativa como Foco para comparar o espectro.";

        const auto focusAnchor = focusRow >= 0
            ? rows[static_cast<std::size_t>(focusRow)].reading.rms + rows[static_cast<std::size_t>(focusRow)].trim : 0.0f;
        const auto anchor = mode() == nx::GainMode::calibration ? plugin.value("reference") : focusAnchor;
        for (auto& row : rows)
            if (canSuggest(row))
                row.proposed = nx::suggestTrim(row.reading, mode(), anchor, row.role,
                                               plugin.value("limit"), plugin.value("headroom"));
    }

    void capture()
    {
        discard({});
        timerCallback();
        if (plugin.value("bypass") > 0.5f)
        {
            status = "Desative o bypass do HUB antes de capturar.";
            return;
        }
        if (mode() == nx::GainMode::musical && focusRow < 0)
        {
            status = "Escolha uma pista de Foco e reproduza um trecho ativo.";
            return;
        }
        int skipped = 0;
        capturedSession = plugin.session();
        capturedHeadroom = plugin.value("headroom");
        const int scopeId = scope.getSelectedId();
        auto inScope = [&](const auto& row)
        {
            return scopeId == 1 ? row.identity == static_cast<unsigned>(pick.getSelectedId())
                : scopeId == 2 ? row.group == plugin.group() : true;
        };
        const bool commonCalibration = mode() == nx::GainMode::calibration && scopeId != 1;
        nx::GroupCalibration calibration(plugin.value("limit"), capturedHeadroom);
        if (commonCalibration)
        {
            for (const auto& row : rows)
                if (inScope(row) && canSuggest(row))
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
            if (!canSuggest(row) || (!commonCalibration && row.proposed.headroomUnmet))
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
                pending.push_back({row.identity, row.trim, next});
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
        std::lock_guard<std::mutex> lock(registry().mutex);
        if (pending.empty() || !std::all_of(pending.begin(), pending.end(),
                                           [this](const auto& change) { return stillApplies(change); }))
        {
            clearPreview();
            status = utf8("Aplicação cancelada. Capture novamente com sinal e margem suficientes.");
            return;
        }
        for (const auto& change : pending)
            findTrack(change.identity)->setTrim(change.next);
        clearPreview();
        const auto count = pending.size();
        plugin.history.push_back(std::move(pending));
        pending.clear();
        status = juce::String(count) + " trims internos aplicados. Desfazer restaura os valores anteriores.";
    }

    void undoLast()
    {
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
            return true;
        }), batch.end());
        const auto remaining = batch.size();
        if (batch.empty())
            plugin.history.pop_back();
        status = juce::String(oldSize - remaining) + " trims restaurados."
            + (remaining == 0 ? juce::String{} : utf8(" Há pistas indisponíveis ou com trim alterado."));
    }

    Processor& plugin;
    juce::ComboBox instance, role, operation, scope, pick;
    juce::Slider session, group, trim, reference, limit, headroom;
    juce::TextButton review, apply, audition, undo, cancel;
    juce::ToggleButton bypass;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> choices;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliders;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::vector<TrackRow> rows;
    std::vector<Change> pending;
    nx::Reading local;
    bool localCurrent = false, listening = false;
    int focusRow = -1, otherRow = -1, capturedSession = 0;
    float capturedHeadroom = 0;
    juce::String status = "Reproduza um trecho representativo antes de capturar.";
    juce::String tonalStatus;
};

juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Processor; }
