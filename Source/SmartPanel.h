#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace nx
{
class SmartPanel final : public juce::Component
{
public:
    enum Action { analyse, preview, apply, cancel, undo, redo, headroom };

    explicit SmartPanel(std::function<void(Action)> callback)
        : action(std::move(callback))
    {
        mode.addItemList({"Technical Calibration", "Musical Balance", "Preserve Existing Mix", "Hybrid Intelligent"}, 1);
        addAndMakeVisible(mode);
        function.addItemList({"Auto", "Principal", juce::String::fromUTF8("Rítmico"), juce::String::fromUTF8("Harmônico"), juce::String::fromUTF8("Melódico"), "Ambiental"}, 1);
        addAndMakeVisible(function);
        addAndMakeVisible(diagnosticTrack);
        configure(priority, " %");
        configure(busTarget, " dBFS");
        toggle(volumeLock, "Travar volume");
        toggle(eqLock, "Travar EQ");
        toggle(referenceLock, juce::String::fromUTF8("Referência"));
        toggle(gainModule, "Gain");
        toggle(toneModule, "Tonal");
        toggle(maskModule, juce::String::fromUTF8("Másc."));
        gainModule.setToggleState(true, juce::dontSendNotification);
        bind(analyseButton, "Analisar trecho", analyse);
        bind(previewButton, "Ouvir B", preview);
        bind(applyButton, "Aplicar", apply);
        bind(cancelButton, "Cancelar", cancel);
        bind(undoButton, "Desfazer", undo);
        bind(redoButton, "Refazer", redo);
        bind(busButton, juce::String::fromUTF8("Ajustar saída"), headroom);
        details.setMultiLine(true);
        details.setReadOnly(true);
        details.setScrollbarsShown(true);
        details.setCaretVisible(false);
        details.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0a1420));
        details.setColour(juce::TextEditor::textColourId, juce::Colour(0xffd8e9f5));
        addAndMakeVisible(details);
    }

    void layout(bool hub)
    {
        mode.setBounds(14, 39, 309, 26);
        diagnosticTrack.setBounds(14, 71, 309, 26);
        diagnosticTrack.setVisible(hub);
        function.setBounds(14, 71, 125, 26);
        priority.setBounds(206, 71, 117, 26);
        function.setVisible(!hub);
        priority.setVisible(!hub);
        volumeLock.setBounds(14, 107, 111, 23);
        eqLock.setBounds(130, 107, 90, 23);
        referenceLock.setBounds(221, 107, 104, 23);
        volumeLock.setVisible(!hub);
        eqLock.setVisible(!hub);
        referenceLock.setVisible(!hub);
        busTarget.setBounds(124, 103, 199, 25);
        busTarget.setVisible(hub);
        gainModule.setBounds(14, 136, 64, 23);
        toneModule.setBounds(87, 136, 65, 23);
        maskModule.setBounds(162, 136, 70, 23);
        busButton.setBounds(231, 133, 92, 28);
        busButton.setVisible(hub);
        analyseButton.setBounds(14, 168, 116, 27);
        previewButton.setBounds(138, 168, 90, 27);
        applyButton.setBounds(234, 168, 89, 27);
        cancelButton.setBounds(14, 201, 116, 27);
        undoButton.setBounds(138, 201, 90, 27);
        redoButton.setBounds(234, 201, 89, 27);
        details.setBounds(14, 239, 309, 192);
        hubMode = hub;
    }

    void paint(juce::Graphics& g) override
    {
        const auto box = getLocalBounds().toFloat().reduced(0.5f);
        g.setGradientFill({juce::Colour(0xff1a2b3d), 0, 0, juce::Colour(0xff0b121d), box.getWidth(), box.getHeight(), false});
        g.fillRoundedRectangle(box, 13);
        g.setColour(juce::Colour(0xff42637a));
        g.drawRoundedRectangle(box, 13, 1);
        g.setColour(juce::Colour(0xffe7edf5));
        g.setFont(juce::Font(juce::FontOptions(17.0f)));
        g.drawText("Smart Mix Prepare", 14, 8, 310, 26, juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(hubMode ? juce::String::fromUTF8("Pico pré-master") : juce::String("Prioridade"), hubMode ? 14 : 146, hubMode ? 104 : 72,
                   hubMode ? 104 : 65, 24, juce::Justification::centredLeft);
    }

    void update(bool analysing, bool hasProposal, bool isPreview, bool canUndo, bool canRedo, bool writable)
    {
        analyseButton.setEnabled(!analysing);
        previewButton.setEnabled(!analysing && hasProposal && writable);
        applyButton.setEnabled(!analysing && hasProposal && writable);
        cancelButton.setEnabled(analysing || hasProposal);
        undoButton.setEnabled(canUndo && writable && !analysing);
        redoButton.setEnabled(canRedo && writable && !analysing);
        busButton.setEnabled(writable && !analysing);
        previewButton.setButtonText(isPreview ? "Voltar a A" : "Ouvir B");
    }

    void report(const juce::String& value)
    {
        if (details.getText() != value)
        {
            const auto caret = details.getCaretPosition();
            details.setText(value, false);
            details.setCaretPosition(std::min(caret, value.length()));
        }
    }

    juce::ComboBox mode, function, diagnosticTrack;
    juce::Slider priority, busTarget;
    juce::ToggleButton volumeLock, eqLock, referenceLock, gainModule, toneModule, maskModule;

private:
    void configure(juce::Slider& item, const char* suffix)
    {
        item.setSliderStyle(juce::Slider::LinearHorizontal);
        item.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 24);
        item.setTextValueSuffix(suffix);
        addAndMakeVisible(item);
    }

    void toggle(juce::ToggleButton& item, const juce::String& title)
    {
        item.setButtonText(title);
        addAndMakeVisible(item);
    }

    void bind(juce::TextButton& item, const juce::String& title, Action verb)
    {
        item.setButtonText(title);
        item.onClick = [this, verb] { action(verb); };
        addAndMakeVisible(item);
    }

    std::function<void(Action)> action;
    juce::TextButton analyseButton, previewButton, applyButton, cancelButton, undoButton, redoButton, busButton;
    juce::TextEditor details;
    bool hubMode = true;
};
}
