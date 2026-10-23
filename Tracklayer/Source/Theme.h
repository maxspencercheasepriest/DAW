#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

// Colours, fonts and small drawing helpers shared by the whole UI.
namespace Theme
{
    inline const juce::Colour bg         { 0xff1c1c1f };
    inline const juce::Colour panel      { 0xff2a2a2e };
    inline const juce::Colour panelLight { 0xff38383d };
    inline const juce::Colour panelDark  { 0xff222225 };
    inline const juce::Colour lane       { 0xff27272b };
    inline const juce::Colour laneAlt    { 0xff242428 };
    inline const juce::Colour laneSel    { 0xff31313a };
    inline const juce::Colour grid       { 0xff303036 };
    inline const juce::Colour gridBar    { 0xff46464e };
    inline const juce::Colour text       { 0xffdcdce1 };
    inline const juce::Colour textDim    { 0xff8e8e98 };
    inline const juce::Colour accent     { 0xff4ea1ff };
    inline const juce::Colour cycle      { 0xffe8c547 };
    inline const juce::Colour record     { 0xffe5484d };
    inline const juce::Colour play       { 0xff4cc26a };
    inline const juce::Colour mute       { 0xff4ea1ff };
    inline const juce::Colour solo       { 0xffe8c547 };
    inline const juce::Colour monitor    { 0xffef8a3d };
    inline const juce::Colour lcdBg      { 0xff10151c };
    inline const juce::Colour lcdText    { 0xffd2e6ff };
    inline const juce::Colour lcdDim     { 0xff5f7590 };

    inline juce::Colour trackColour (int index)
    {
        static const juce::uint32 palette[] = { 0xff4fb3e8, 0xff5fd16a, 0xffe8c547, 0xffef8a3d,
                                                0xffe5576b, 0xffb07be8, 0xff47d1c4, 0xffe87bc3 };
        return juce::Colour (palette[(size_t) index % std::size (palette)]);
    }

    inline juce::Font font (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    // Fader values are shown in dB; -60 dB means "off".
    inline float dbToGain (double db)        { return db <= -59.95 ? 0.0f : juce::Decibels::decibelsToGain ((float) db); }
    inline double gainToDb (float gain)      { return juce::jmax (-60.0, (double) juce::Decibels::gainToDecibels (gain, -60.0f)); }

    // Vertical peak meter. level is linear (1.0 = 0 dBFS).
    inline void drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float level)
    {
        g.setColour (juce::Colour (0xff141416));
        g.fillRect (r);

        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 63.0f);
        if (frac <= 0.0f)
            return;

        auto lit = r.withTop (r.getBottom() - r.getHeight() * frac);
        juce::ColourGradient grad (juce::Colour (0xff3fcf5e), r.getBottomLeft(),
                                   juce::Colour (0xffe5484d), r.getTopLeft(), false);
        grad.addColour (0.80, juce::Colour (0xffd8d045));
        g.setGradientFill (grad);
        g.fillRect (lit);

        if (level >= 1.0f)
        {
            g.setColour (juce::Colour (0xffff3030));
            g.fillRect (r.withHeight (3.0f));
        }
    }

    inline void drawStereoMeter (juce::Graphics& g, juce::Rectangle<float> r, float l, float rLevel)
    {
        auto left = r.removeFromLeft (r.getWidth() * 0.5f).reduced (0.5f, 0.0f);
        drawMeter (g, left, l);
        drawMeter (g, r.reduced (0.5f, 0.0f), rLevel);
    }

    // Stop buttons, sliders and combo boxes stealing keyboard focus so
    // DAW shortcuts (space, R, C ...) keep working after clicking them.
    inline void noFocus (juce::Component& c)
    {
        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            auto scheme = getDarkColourScheme();
            scheme.setUIColour (ColourScheme::windowBackground, bg);
            scheme.setUIColour (ColourScheme::widgetBackground, panelLight);
            scheme.setUIColour (ColourScheme::menuBackground, panel);
            scheme.setUIColour (ColourScheme::outline, juce::Colour (0xff45454b));
            scheme.setUIColour (ColourScheme::defaultText, text);
            scheme.setUIColour (ColourScheme::defaultFill, juce::Colour (0xff5a5a62));
            scheme.setUIColour (ColourScheme::highlightedText, juce::Colours::white);
            scheme.setUIColour (ColourScheme::highlightedFill, accent.withAlpha (0.6f));
            scheme.setUIColour (ColourScheme::menuText, text);
            setColourScheme (scheme);

            setColour (juce::TextButton::buttonColourId, panelLight);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::Slider::thumbColourId, juce::Colour (0xffc8c8cf));
            setColour (juce::Slider::trackColourId, juce::Colour (0xff6c6c75));
            setColour (juce::Slider::backgroundColourId, juce::Colour (0xff18181a));
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff18181a));
            setColour (juce::ComboBox::backgroundColourId, panelDark);
            setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3e3e44));
            setColour (juce::PopupMenu::backgroundColourId, panel);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.5f));
            setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff55555d));
            setColour (juce::Label::textColourId, text);
            setColour (juce::TooltipWindow::backgroundColourId, panel);
            setColour (juce::BubbleComponent::backgroundColourId, panel);
            setColour (juce::BubbleComponent::outlineColourId, juce::Colour (0xff55555d));
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                                   bool highlighted, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;
            if (down)              c = c.brighter (0.15f);
            else if (highlighted)  c = c.brighter (0.07f);
            g.setColour (c);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return font (juce::jmin (13.0f, (float) buttonHeight * 0.6f), true);
        }
    };
}
