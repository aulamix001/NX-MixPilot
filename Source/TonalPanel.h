#pragma once

#include "TonalDSP.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace nx
{
class TonalPanel final : public juce::Component
{
public:
    enum Action { selectTracks, capture, captureSignal, import, suggest, masking, preview, apply, cancel, undo, bypass, lowEnd, vocal, library, removeBand };

    explicit TonalPanel(std::function<void(Action)> callback)
        : action(std::move(callback))
    {
        for (std::size_t i = 0; i < profiles.size(); ++i)
            genre.addItem(juce::String::fromUTF8(profiles[i].name), static_cast<int>(i) + 1);
        genre.setSelectedId(1);
        addAndMakeVisible(genre);
        configure(intensity, 0, 100, 1, 35, " %");
        configure(limit, 0.25, 12, 0.25, 3, " dB");
        configure(frequency, 20, 20000, 1, 1000, " Hz");
        frequency.setSkewFactorFromMidPoint(1000);
        configure(gain, -12, 12, 0.05, 0, " dB");
        configure(q, 0.3, 8, 0.05, 0.95, "");
        addAndMakeVisible(correction);
        duration.addItemList({"15s", "20s", "30s"}, 1);
        duration.setSelectedId(1);
        duration.setTitle(juce::String::fromUTF8("Duração da captura ativa"));
        duration.setTooltip(juce::String::fromUTF8("Duração da captura em segundos de áudio ativo."));
        addAndMakeVisible(duration);
        smoothing.addItemList({"Suav 0", "Suav 1", "Suav 2", "Suav 3", "Suav 4", "Suav 5", "Suav 6"}, 1);
        smoothing.setSelectedId(3);
        smoothing.setTitle(juce::String::fromUTF8("Suavização espectral"));
        smoothing.setTooltip(juce::String::fromUTF8("Suaviza as diferenças espectrais antes de ajustar os filtros."));
        addAndMakeVisible(smoothing);
        range.addItemList({"20-20k Hz", "20-250 Hz", "120-5k Hz", "2k-20k Hz"}, 1);
        range.setSelectedId(1);
        range.setTitle(juce::String::fromUTF8("Faixa tonal de correção"));
        addAndMakeVisible(range);
        bind(targets, "Pistas alvo", selectTracks);
        bind(captureButton, "Capturar ref.", capture);
        bind(signalButton, "Capturar sinal", captureSignal);
        bind(importButton, "Importar", import);
        bind(suggestButton, "Sugerir EQ", suggest);
        bind(maskButton, "Mascaramento", masking);
        bind(previewButton, "Ouvir B", preview);
        bind(applyButton, "Aplicar EQ", apply);
        bind(cancelButton, "Cancelar", cancel);
        bind(undoButton, "Desfazer EQ", undo);
        bind(bypassButton, "Bypass EQ", bypass);
        bind(lowButton, "Low End", lowEnd);
        bind(vocalButton, "Vocal", vocal);
        bind(libraryButton, "Biblioteca", library);
        bind(removeButton, "Excluir banda", removeBand);
        compensation.setButtonText("Nivelar A/B");
        compensation.setTooltip(juce::String::fromUTF8("Compensa o nível RMS estimado do EQ na prévia e na aplicação."));
        compensation.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(compensation);
    }

    void resized() override
    {
        genre.setBounds(14, 39, 191, 26);
        targets.setBounds(213, 39, 111, 26);
        intensity.setBounds(77, 71, 105, 25);
        limit.setBounds(231, 71, 93, 25);
        captureButton.setBounds(14, 104, 86, 26);
        signalButton.setBounds(104, 104, 85, 26);
        importButton.setBounds(193, 104, 62, 26);
        libraryButton.setBounds(259, 104, 65, 26);
        suggestButton.setBounds(14, 139, 151, 29);
        maskButton.setBounds(174, 139, 150, 29);
        correction.setBounds(14, 194, 208, 26);
        removeButton.setBounds(228, 194, 96, 26);
        frequency.setBounds(69, 226, 255, 24);
        gain.setBounds(69, 255, 255, 24);
        q.setBounds(69, 284, 255, 24);
        duration.setBounds(14, 308, 65, 22);
        smoothing.setBounds(84, 308, 76, 22);
        range.setBounds(165, 308, 159, 22);
        compensation.setBounds(218, 9, 106, 26);
        previewButton.setBounds(14, 333, 101, 24);
        applyButton.setBounds(122, 333, 99, 24);
        cancelButton.setBounds(228, 333, 96, 24);
        undoButton.setBounds(14, 360, 151, 26);
        bypassButton.setBounds(174, 360, 150, 26);
        lowButton.setBounds(14, 391, 151, 24);
        vocalButton.setBounds(174, 391, 150, 24);
    }

    void paint(juce::Graphics& g) override
    {
        const auto frame = getLocalBounds().toFloat().reduced(0.5f);
        g.setGradientFill({juce::Colour(0xff1a2b3d), 0.0f, 0.0f, juce::Colour(0xff0b121d),
                           frame.getWidth(), frame.getHeight(), false});
        g.fillRoundedRectangle(frame, 13);
        g.setColour(juce::Colour(0xff42637a));
        g.drawRoundedRectangle(frame, 13, 1);
        text(g, "Tonal Balance Match", 14, 9, 312, 26, 17, 0xffe7edf5);
        text(g, juce::String::fromUTF8("Intensidade"), 14, 70, 65, 24);
        text(g, "Limite", 189, 70, 44, 24);
        text(g, summary, 14, 171, 309, 21, 12, 0xff30d7ef);
        text(g, "Freq.", 14, 225, 54, 24);
        text(g, "Ganho", 14, 253, 54, 24);
        text(g, "Q", 14, 282, 54, 24);
        g.setColour(juce::Colour(0xffb4c7d9));
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawFittedText(status, 14, 418, 308, 26, juce::Justification::topLeft, 2);
    }

    void update(bool hasProposal, bool isPreview, bool hasUndo, bool isBypassed, bool captureRunning = false)
    {
        previewButton.setEnabled(hasProposal);
        applyButton.setEnabled(hasProposal);
        cancelButton.setEnabled(hasProposal || captureRunning);
        removeButton.setEnabled(hasProposal && correction.getSelectedId() > 0);
        captureButton.setEnabled(!captureRunning);
        signalButton.setEnabled(!captureRunning);
        duration.setEnabled(!captureRunning);
        undoButton.setEnabled(hasUndo);
        correction.setEnabled(hasProposal);
        frequency.setEnabled(hasProposal);
        gain.setEnabled(hasProposal);
        q.setEnabled(hasProposal);
        previewButton.setButtonText(isPreview ? "Voltar a A" : "Ouvir B");
        bypassButton.setButtonText(isBypassed ? "Ativar EQ" : "Bypass EQ");
    }

    int captureSeconds() const { return std::array<int, 3>{15, 20, 30}[std::max(0, duration.getSelectedId() - 1)]; }
    double lowHz() const { return std::array<double, 4>{20, 20, 120, 2000}[std::max(0, range.getSelectedId() - 1)]; }
    double highHz() const { return std::array<double, 4>{20000, 250, 5000, 20000}[std::max(0, range.getSelectedId() - 1)]; }

    juce::ComboBox genre, correction, duration, smoothing, range;
    juce::Slider intensity, limit, frequency, gain, q;
    juce::ToggleButton compensation;
    juce::String summary, status;

private:
    static void text(juce::Graphics& g, const juce::String& value, int x, int y, int w, int h,
                     float size = 12, juce::uint32 colour = 0xffb4c7d9)
    {
        g.setColour(juce::Colour(colour));
        g.setFont(juce::Font(juce::FontOptions(size)));
        g.drawFittedText(value, x, y, w, h, juce::Justification::centredLeft, 1);
    }

    void configure(juce::Slider& slider, double low, double high, double step, double initial, const char* unit)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(low, high, step);
        slider.setValue(initial);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 24);
        slider.setTextValueSuffix(unit);
        addAndMakeVisible(slider);
    }

    void bind(juce::TextButton& button, const char* name, Action verb)
    {
        button.setButtonText(name);
        button.onClick = [this, verb] { action(verb); };
        addAndMakeVisible(button);
    }

    std::function<void(Action)> action;
    juce::TextButton targets, captureButton, signalButton, removeButton, importButton, suggestButton, maskButton,
        previewButton, applyButton, cancelButton, undoButton, bypassButton, lowButton, vocalButton, libraryButton;
};
}
