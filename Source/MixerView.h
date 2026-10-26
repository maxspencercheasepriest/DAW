#pragma once

#include "ArrangeView.h"

//==============================================================================
// One mixer channel strip. A null track means the Stereo Out (master) strip.
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (AppContext& c, TrackPtr t) : ctx (c), track (std::move (t))
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (-60.0, 6.0, 0.1);
        fader.setSkewFactorFromMidPoint (-12.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextValueSuffix (" dB");
        fader.setPopupDisplayEnabled (true, true, nullptr);
        fader.onValueChange = [this]
        {
            const float g = Theme::dbToGain (fader.getValue());
            if (track != nullptr) track->volume = g;
            else                  ctx.engine.masterVolume = g;
            changed();
        };
        fader.onDragEnd = [this] { ctx.commitEdit (track != nullptr ? "Volume" : "Master Volume"); };
        Theme::noFocus (fader);
        addAndMakeVisible (fader);

        if (track != nullptr)
        {
            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            pan.setTooltip ("Pan (double-click to centre)");
            pan.onValueChange = [this] { track->pan = (float) pan.getValue(); changed(); };
            pan.onDragEnd = [this] { ctx.commitEdit ("Pan"); };
            Theme::noFocus (pan);
            addAndMakeVisible (pan);

            setupToggle (muteB, "M", Theme::mute, "Mute");
            setupToggle (soloB, "S", Theme::solo, "Solo");
            setupToggle (armB, "R", Theme::record, "Record enable");
            setupToggle (monB, "I", Theme::monitor, "Input monitoring");
            muteB.onClick = [this] { track->mute = muteB.getToggleState(); changed(); ctx.commitEdit ("Mute"); };
            soloB.onClick = [this] { track->solo = soloB.getToggleState(); changed(); ctx.commitEdit ("Solo"); };
            monB.onClick  = [this] { track->monitor = monB.getToggleState(); changed(); };
            armB.onClick  = [this]
            {
                if (ctx.engine.isRecording()) { armB.setToggleState (track->armed.load(), juce::dontSendNotification); return; }
                track->armed = armB.getToggleState();
                changed();
            };
            for (auto* b : { &muteB, &soloB, &armB, &monB })
                addAndMakeVisible (b);

            input.onChange = [this] { applyInputChoice (input.getSelectedId(), *track); ctx.engine.dirty = true; ctx.commitEdit ("Change Input"); };
            input.setTooltip ("Input channel");
            Theme::noFocus (input);
            if (track->isInstrument)
            {
                instrumentSlot = std::make_unique<InsertSlot> (ctx, track, -1);
                addAndMakeVisible (*instrumentSlot);
                monB.setVisible (false);
            }
            else
            {
                addAndMakeVisible (input);
            }
            refreshInputs();

            for (int i = 0; i < numInserts; ++i)
            {
                auto s = std::make_unique<InsertSlot> (ctx, track, i);
                addAndMakeVisible (*s);
                slots.push_back (std::move (s));
            }
        }

        sync();
    }

    void refreshInputs()
    {
        if (track != nullptr)
            fillInputCombo (input, ctx.engine.getActiveInputNames(), *track);
    }

    void sync()
    {
        const float vol = track != nullptr ? track->volume.load() : ctx.engine.masterVolume.load();
        if (! fader.isMouseButtonDown())
            fader.setValue (Theme::gainToDb (vol), juce::dontSendNotification);

        if (track != nullptr)
        {
            if (! pan.isMouseButtonDown())
                pan.setValue (track->pan.load(), juce::dontSendNotification);
            muteB.setToggleState (track->mute.load(), juce::dontSendNotification);
            soloB.setToggleState (track->solo.load(), juce::dontSendNotification);
            armB.setToggleState (track->armed.load(), juce::dontSendNotification);
            monB.setToggleState (track->monitor.load(), juce::dontSendNotification);
            for (auto& s : slots)
                s->refresh();
            if (instrumentSlot != nullptr)
                instrumentSlot->refresh();
        }
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (track != nullptr)
            ctx.selectTrack (track);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f, 0.0f);
        const bool sel = track != nullptr && ctx.view.selectedTrack == track;
        g.setColour (sel ? Theme::panelLight : Theme::panel);
        g.fillRoundedRectangle (r, 3.0f);

        // name plate
        auto plate = nameArea.toFloat();
        const auto col = track != nullptr ? track->colour : juce::Colour (0xff9a9aa4);
        g.setColour (col.withAlpha (0.85f));
        g.fillRoundedRectangle (plate, 3.0f);
        g.setColour (juce::Colour (0xff111111));
        g.setFont (Theme::font (12.0f, true));
        g.drawFittedText (track != nullptr ? track->name : juce::String ("Stereo Out"),
                          nameArea.reduced (3, 0), juce::Justification::centred, 2);

        if (track != nullptr)
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (9.5f, true));
            g.drawText ("AUDIO FX", g_labelArea, juce::Justification::centredLeft);
        }

        // dB / pan readouts
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.0f));
        const double db = fader.getValue();
        g.drawText (db <= -59.95 ? juce::String ("-inf") : juce::String (db, 1), dbArea, juce::Justification::centred);

        if (track != nullptr)
        {
            const int p = juce::roundToInt (track->pan.load() * 64.0f);
            g.drawText (p == 0 ? juce::String ("C") : (p < 0 ? "L" + juce::String (-p) : "R" + juce::String (p)),
                        panText, juce::Justification::centred);
        }

        const float l = track != nullptr ? track->dispL : ctx.engine.masterDispL;
        const float rr = track != nullptr ? track->dispR : ctx.engine.masterDispR;
        Theme::drawStereoMeter (g, meterArea.toFloat(), l, rr);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 6);
        nameArea = r.removeFromBottom (32);
        r.removeFromBottom (6);

        if (track != nullptr)
        {
            auto row2 = r.removeFromBottom (22);
            armB.setBounds (row2.removeFromLeft (row2.getWidth() / 2).reduced (1, 0));
            monB.setBounds (row2.reduced (1, 0));
            r.removeFromBottom (3);
            auto row1 = r.removeFromBottom (22);
            muteB.setBounds (row1.removeFromLeft (row1.getWidth() / 2).reduced (1, 0));
            soloB.setBounds (row1.reduced (1, 0));
            r.removeFromBottom (6);

            g_labelArea = r.removeFromTop (12);
            for (auto& s : slots)
            {
                s->setBounds (r.removeFromTop (18));
                r.removeFromTop (2);
            }
            r.removeFromTop (4);
            input.setBounds (r.removeFromTop (22));
            if (instrumentSlot != nullptr)
                instrumentSlot->setBounds (input.getBounds().reduced (0, 2));
            r.removeFromTop (4);
            pan.setBounds (r.removeFromTop (42).withSizeKeepingCentre (42, 42));
            panText = r.removeFromTop (14);
        }
        else
        {
            r.removeFromTop (4);
        }

        dbArea = r.removeFromTop (16);
        r.removeFromTop (4);
        auto faderArea = r;
        meterArea = faderArea.removeFromRight (12);
        fader.setBounds (faderArea.withSizeKeepingCentre (juce::jmin (36, faderArea.getWidth()), faderArea.getHeight()));
    }

    void repaintMeter() { repaint (meterArea); }

private:
    void changed()
    {
        ctx.engine.dirty = true;
        repaint();
        if (ctx.mixChanged) ctx.mixChanged();
    }

    AppContext& ctx;
    TrackPtr track;
    juce::Slider fader, pan;
    juce::TextButton muteB, soloB, armB, monB;
    juce::ComboBox input;
    std::vector<std::unique_ptr<InsertSlot>> slots;
    std::unique_ptr<InsertSlot> instrumentSlot;
    juce::Rectangle<int> nameArea, dbArea, panText, meterArea, g_labelArea;
};

//==============================================================================
class MixerView : public juce::Component
{
public:
    explicit MixerView (AppContext& c) : ctx (c), master (c, nullptr)
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setScrollBarThickness (10);
        Theme::noFocus (viewport);
        addAndMakeVisible (master);
        rebuild();
    }

    static constexpr int stripWidth = 96;

    void rebuild()
    {
        strips.clear();
        for (auto& t : ctx.engine.tracks)
        {
            auto s = std::make_unique<ChannelStrip> (ctx, t);
            holder.addAndMakeVisible (*s);
            strips.push_back (std::move (s));
        }
        resized();
    }

    void syncControls()
    {
        for (auto& s : strips) s->sync();
        master.sync();
    }

    void refreshInputs()
    {
        for (auto& s : strips) s->refreshInputs();
    }

    void updateMeters()
    {
        for (auto& s : strips) s->repaintMeter();
        master.repaintMeter();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        g.setColour (juce::Colours::black);
        g.drawHorizontalLine (0, 0.0f, (float) getWidth());
        g.drawVerticalLine (master.getX() - 3, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop (4);
        master.setBounds (r.removeFromRight (stripWidth + 4).withTrimmedRight (4).withTrimmedBottom (4));
        r.removeFromRight (6);
        viewport.setBounds (r);

        const int h = juce::jmax (0, r.getHeight() - (strips.size() * stripWidth > (size_t) r.getWidth() ? 10 : 0) - 4);
        holder.setSize ((int) strips.size() * stripWidth + 4, h);
        for (size_t i = 0; i < strips.size(); ++i)
            strips[i]->setBounds (4 + (int) i * stripWidth, 0, stripWidth, h);
    }

private:
    AppContext& ctx;
    juce::Viewport viewport;
    juce::Component holder;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    ChannelStrip master;
};
