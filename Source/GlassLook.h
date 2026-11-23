#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class GlassLook final : public juce::LookAndFeel_V4
{
public:
    GlassLook()
    {
        using namespace juce;
        setColour(ComboBox::backgroundColourId, Colour(0xff0e1822));
        setColour(ComboBox::outlineColourId, Colour(0xff3b5368));
        setColour(ComboBox::textColourId, Colour(0xffeaf4ff));
        setColour(ComboBox::arrowColourId, Colour(0xff70dcec));
        setColour(PopupMenu::backgroundColourId, Colour(0xff131f2c));
        setColour(PopupMenu::highlightedBackgroundColourId, Colour(0xff176378));
        setColour(Slider::trackColourId, Colour(0xff2ac8e7));
        setColour(Slider::thumbColourId, Colour(0xffa0f1ff));
        setColour(Slider::backgroundColourId, Colour(0xff304657));
        setColour(Slider::textBoxOutlineColourId, Colour(0xff3b5368));
        setColour(Slider::textBoxBackgroundColourId, Colour(0xff101c28));
        setColour(ToggleButton::tickColourId, Colour(0xff3addf9));
        setColour(TextButton::buttonColourId, Colour(0xff23364a));
        setColour(TextButton::textColourOffId, Colour(0xffeaf4ff));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& base, bool hovered, bool pressed) override
    {
        const auto area = button.getLocalBounds().toFloat().reduced(1);
        const auto color = base.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.40f);
        auto light = color.brighter(pressed ? 0.05f : hovered ? 0.39f : 0.21f);
        if (button.getToggleState() && button.isEnabled())
        {
            g.setColour(color.brighter(0.8f).withAlpha(0.22f));
            g.fillRoundedRectangle(area.expanded(1), 8);
        }
        g.setGradientFill({light, area.getX(), area.getY(), color.darker(0.42f),
                           area.getX(), area.getBottom(), false});
        g.fillRoundedRectangle(area, 7);
        g.setColour(color.brighter(button.hasKeyboardFocus(true) ? 1.15f : 0.5f));
        g.drawRoundedRectangle(area, 7, 1);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position,
                          float start, float end, juce::Slider&) override
    {
        using namespace juce;
        const auto radius = static_cast<float>(jmin(width, height)) * 0.37f;
        if (radius < 6.0f)
            return;
        const Point<float> center(static_cast<float>(x) + width * 0.5f, static_cast<float>(y) + height * 0.5f);
        const Rectangle<float> face(center.x - radius, center.y - radius, radius * 2, radius * 2);
        const auto angle = jmap(position, start, end);
        auto ring = [&](float from, float to, Colour colour, float thickness)
        {
            if (to <= from)
                return;
            Path arc;
            arc.addCentredArc(center.x, center.y, radius + 5, radius + 5, 0, from, to, true);
            g.setColour(colour);
            g.strokePath(arc, PathStrokeType(thickness, PathStrokeType::curved, PathStrokeType::rounded));
        };
        ring(start, end, Colour(0xff263c4c), 4);
        ring(start, angle, Colour(0x2838e1ff), 10);
        ring(start, angle, Colour(0xff38dff8), 3);
        g.setColour(Colour(0xc800050a));
        g.fillEllipse(face.translated(0, 2).expanded(2));
        g.setGradientFill({Colour(0xff638292), face.getX(), face.getY(), Colour(0xff0e1924),
                           face.getRight(), face.getBottom(), false});
        g.fillEllipse(face);
        g.setColour(Colour(0xff233648));
        g.drawEllipse(face.reduced(3), 1);
        Path marker;
        marker.addRoundedRectangle(-1.4f, -radius + 4, 2.8f, radius * 0.35f, 1.4f);
        g.setColour(Colour(0xffedfaff));
        g.fillPath(marker, AffineTransform::rotation(angle).translated(center.x, center.y));
    }
};
