#pragma once

#include "TonalDSP.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace nx
{
class TonalPanel final : public juce::Component
{
public:
    enum Action { selectTracks, capture, import, suggest, masking, preview, apply, cancel, undo, bypass, lowEnd, vocal, library };

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
        correction.addItemList({"Banda 1", "Banda 2", "Banda 3"}, 1);
        correction.setSelectedId(1);
        addAndMakeVisible(correction);
        bind(targets, "Pistas alvo", selectTracks);
        bind(captureButton, "Capturar ref.", capture);
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
        compensation.setButtonText("Compensar ganho");
        compensation.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(compensation);
    }

    void resized() override
    {
        genre.setBounds(14, 39, 191, 26);
        targets.setBounds(213, 39, 111, 26);
        intensity.setBounds(77, 71, 105, 25);
        limit.setBounds(231, 71, 93, 25);
        captureButton.setBounds(14, 104, 99, 26);
        importButton.setBounds(119, 104, 96, 26);
        libraryButton.setBounds(224, 104, 100, 26);
        suggestButton.setBounds(14, 139, 151, 29);
        maskButton.setBounds(174, 139, 150, 29);
        correction.setBounds(14, 194, 96, 26);
        compensation.setBounds(119, 193, 195, 27);
        frequency.setBounds(69, 226, 255, 24);
        gain.setBounds(69, 255, 255, 24);
        q.setBounds(69, 284, 255, 24);
        previewButton.setBounds(14, 324, 101, 28);
        applyButton.setBounds(122, 324, 99, 28);
        cancelButton.setBounds(228, 324, 96, 28);
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

    void update(bool hasProposal, bool isPreview, bool hasUndo, bool isBypassed)
    {
        previewButton.setEnabled(hasProposal);
        applyButton.setEnabled(hasProposal);
        cancelButton.setEnabled(hasProposal);
        undoButton.setEnabled(hasUndo);
        correction.setEnabled(hasProposal);
        frequency.setEnabled(hasProposal);
        gain.setEnabled(hasProposal);
        q.setEnabled(hasProposal);
        previewButton.setButtonText(isPreview ? "Voltar a A" : "Ouvir B");
        bypassButton.setButtonText(isBypassed ? "Ativar EQ" : "Bypass EQ");
    }

    juce::ComboBox genre, correction;
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
    juce::TextButton targets, captureButton, importButton, suggestButton, maskButton,
        previewButton, applyButton, cancelButton, undoButton, bypassButton, lowButton, vocalButton, libraryButton;
};
}
