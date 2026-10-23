#pragma once

#include "AppContext.h"

// A transport button that draws its own icon.
class TransportButton : public juce::Button
{
public:
    enum class Icon { toStart, back, forward, stop, play, record, cycle, metronome };

    TransportButton (Icon i, juce::Colour onColour, const juce::String& tip)
        : juce::Button (tip), icon (i), on (onColour)
    {
        setTooltip (tip);
        Theme::noFocus (*this);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool lit = getToggleState();

        auto bgc = lit ? on : Theme::panelLight;
        if (down)             bgc = bgc.brighter (0.15f);
        else if (highlighted) bgc = bgc.brighter (0.08f);
        g.setColour (bgc);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        auto fg = lit ? juce::Colour (0xff141414) : Theme::text;
        if (icon == Icon::record && ! lit)
            fg = Theme::record;

        auto c = r.getCentre();
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
        juce::Path p;

        auto tri = [&p] (float x, float y, float w, float h, bool right)
        {
            if (right) p.addTriangle (x, y - h, x, y + h, x + w, y);
            else       p.addTriangle (x + w, y - h, x + w, y + h, x, y);
        };

        switch (icon)
        {
            case Icon::toStart:
                p.addRectangle (c.x - s * 1.2f, c.y - s, s * 0.35f, s * 2.0f);
                tri (c.x - s * 0.8f, c.y, s * 1.8f, s, false);
                break;
            case Icon::back:
                tri (c.x - s * 1.4f, c.y, s * 1.3f, s * 0.9f, false);
                tri (c.x - s * 0.1f, c.y, s * 1.3f, s * 0.9f, false);
                break;
            case Icon::forward:
                tri (c.x - s * 1.2f, c.y, s * 1.3f, s * 0.9f, true);
                tri (c.x + s * 0.1f, c.y, s * 1.3f, s * 0.9f, true);
                break;
            case Icon::stop:
                p.addRectangle (c.x - s * 0.9f, c.y - s * 0.9f, s * 1.8f, s * 1.8f);
                break;
            case Icon::play:
                tri (c.x - s * 0.8f, c.y, s * 1.9f, s * 1.05f, true);
                break;
            case Icon::record:
                p.addEllipse (c.x - s, c.y - s, s * 2.0f, s * 2.0f);
                break;
            case Icon::cycle:
            {
                juce::Path loop;
                loop.addRoundedRectangle (c.x - s * 1.4f, c.y - s * 0.8f, s * 2.8f, s * 1.6f, s * 0.8f);
                g.setColour (fg);
                g.strokePath (loop, juce::PathStrokeType (s * 0.32f));
                tri (c.x - s * 0.2f, c.y - s * 0.8f, s * 0.75f, s * 0.5f, true);
                break;
            }
            case Icon::metronome:
            {
                p.startNewSubPath (c.x - s * 0.9f, c.y + s * 1.1f);
                p.lineTo (c.x + s * 0.9f, c.y + s * 1.1f);
                p.lineTo (c.x + s * 0.35f, c.y - s * 1.2f);
                p.lineTo (c.x - s * 0.35f, c.y - s * 1.2f);
                p.closeSubPath();
                g.setColour (fg);
                g.strokePath (p, juce::PathStrokeType (s * 0.25f));
                p.clear();
                juce::Path needle;
                needle.startNewSubPath (c.x, c.y + s * 0.7f);
                needle.lineTo (c.x + s * 0.9f, c.y - s * 0.9f);
                g.strokePath (needle, juce::PathStrokeType (s * 0.25f));
                break;
            }
        }

        g.setColour (fg);
        g.fillPath (p);
    }

private:
    Icon icon;
    juce::Colour on;
};

//==============================================================================
// Tempo display you can drag up/down or double-click to type into.
class TempoLabel : public juce::Label
{
public:
    explicit TempoLabel (Engine& e) : engine (e)
    {
        setEditable (false, true, false);
        setJustificationType (juce::Justification::centred);
        setFont (Theme::monoFont (20.0f, true));
        setColour (textColourId, Theme::lcdText);
        setColour (juce::TextEditor::backgroundColourId, Theme::lcdBg);
        setColour (juce::TextEditor::textColourId, Theme::lcdText);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTooltip ("Tempo: drag up/down or double-click to type");
        onTextChange = [this]
        {
            const double v = getText().getDoubleValue();
            if (v >= 20.0 && v <= 300.0) { engine.tempo = v; engine.dirty = true; }
            refresh();
        };
    }

    void refresh()
    {
        if (! isBeingEdited())
            setText (juce::String (engine.tempo.load(), 2), juce::dontSendNotification);
    }

    void mouseDown (const juce::MouseEvent& e) override  { dragStart = engine.tempo.load(); juce::Label::mouseDown (e); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double fine = e.mods.isShiftDown() ? 0.01 : 0.25;
        const double v = juce::jlimit (20.0, 300.0, dragStart - e.getDistanceFromDragStartY() * fine);
        engine.tempo = std::round (v * 100.0) / 100.0;
        engine.dirty = true;
        refresh();
    }

private:
    Engine& engine;
    double dragStart = 120.0;
};

//==============================================================================
// Logic-style LCD: bars/beats/division/ticks, clock time, tempo, signature.
class Lcd : public juce::Component
{
public:
    explicit Lcd (Engine& e) : engine (e), tempo (e)
    {
        addAndMakeVisible (tempo);
    }

    void refresh()
    {
        tempo.refresh();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::lcdBg);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (juce::Colour (0xff2b3644));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

        const double pos = engine.getPosition();
        const double beats = pos / engine.secondsPerBeat();
        const int bpb = engine.beatsPerBar;
        const auto wholeBeats = (juce::int64) std::floor (beats);
        const int bar = (int) (wholeBeats / bpb) + 1;
        const int beat = (int) (wholeBeats % bpb) + 1;
        const double frac = beats - (double) wholeBeats;
        const int div = (int) std::floor (frac * 4.0) + 1;
        const int tick = (int) std::floor ((frac * 4.0 - std::floor (frac * 4.0)) * 240.0) + 1;

        auto area = getLocalBounds().reduced (10, 4);
        auto bbt = area.removeFromLeft (170);
        auto clock = area.removeFromLeft (128);

        const juce::String fields[] = { juce::String (bar), juce::String (beat), juce::String (div),
                                        juce::String (tick).paddedLeft ('0', 3) };
        const char* labels[] = { "BAR", "BEAT", "DIV", "TICK" };
        const int widths[] = { 50, 36, 34, 50 };

        int x = bbt.getX();
        for (int i = 0; i < 4; ++i)
        {
            juce::Rectangle<int> cell (x, bbt.getY(), widths[i], bbt.getHeight());
            g.setColour (Theme::lcdText);
            g.setFont (Theme::monoFont (24.0f, true));
            g.drawText (fields[i], cell.withTrimmedBottom (13), juce::Justification::centredBottom);
            g.setColour (Theme::lcdDim);
            g.setFont (Theme::font (9.5f, true));
            g.drawText (labels[i], cell.removeFromBottom (12), juce::Justification::centred);
            x += widths[i];
        }

        const int totalMs = (int) (pos * 1000.0);
        const auto timeStr = juce::String::formatted ("%02d:%02d:%02d.%03d",
                                                      totalMs / 3600000, (totalMs / 60000) % 60,
                                                      (totalMs / 1000) % 60, totalMs % 1000);
        g.setColour (Theme::lcdText.withAlpha (0.85f));
        g.setFont (Theme::monoFont (15.0f));
        g.drawText (timeStr, clock.withTrimmedBottom (13), juce::Justification::centredBottom);
        g.setColour (Theme::lcdDim);
        g.setFont (Theme::font (9.5f, true));
        g.drawText ("TIME", clock.removeFromBottom (12), juce::Justification::centred);

        auto tArea = tempoArea;
        g.drawText ("TEMPO", tArea.removeFromBottom (12), juce::Justification::centred);
        auto sig = sigArea;
        g.drawText ("SIGNATURE", sig.removeFromBottom (12), juce::Justification::centred);
        g.setColour (Theme::lcdText);
        g.setFont (Theme::monoFont (20.0f, true));
        g.drawText (juce::String (bpb) + "/4", sig, juce::Justification::centredBottom);

        if (engine.isRecording())
        {
            g.setColour (Theme::record);
            g.fillEllipse ((float) getWidth() - 14.0f, 6.0f, 8.0f, 8.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10, 4);
        area.removeFromLeft (170 + 128);
        tempoArea = area.removeFromLeft (84);
        sigArea = area.removeFromLeft (80);
        tempo.setBounds (tempoArea.withTrimmedBottom (12));
    }

private:
    Engine& engine;
    TempoLabel tempo;
    juce::Rectangle<int> tempoArea, sigArea;
};

//==============================================================================
class TransportBar : public juce::Component
{
public:
    explicit TransportBar (AppContext& c)
        : ctx (c), lcd (c.engine),
          toStart (TransportButton::Icon::toStart, Theme::text, "Go to beginning (Return)"),
          back (TransportButton::Icon::back, Theme::text, "Back one bar (,)"),
          forward (TransportButton::Icon::forward, Theme::text, "Forward one bar (.)"),
          stopB (TransportButton::Icon::stop, Theme::text, "Stop (Space)"),
          playB (TransportButton::Icon::play, Theme::play, "Play (Space)"),
          recB (TransportButton::Icon::record, Theme::record, "Record (R)"),
          cycleB (TransportButton::Icon::cycle, Theme::cycle, "Cycle (C)"),
          metroB (TransportButton::Icon::metronome, Theme::accent, "Metronome (K)")
    {
        auto add = [this] (juce::Button& b, int cmd)
        {
            addAndMakeVisible (b);
            b.onClick = [this, cmd] { if (ctx.runCommand) ctx.runCommand (cmd); };
        };
        add (toStart, cmdGoToStart);
        add (back, cmdBarBack);
        add (forward, cmdBarForward);
        stopB.onClick = [this] { ctx.engine.stop(); ctx.engine.setPosition (ctx.engine.getPosition()); sync(); };
        addAndMakeVisible (stopB);
        add (playB, cmdPlayStop);
        add (recB, cmdRecord);
        add (cycleB, cmdCycle);
        add (metroB, cmdMetronome);

        addAndMakeVisible (lcd);

        master.setSliderStyle (juce::Slider::LinearHorizontal);
        master.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        master.setRange (-60.0, 6.0, 0.1);
        master.setSkewFactorFromMidPoint (-12.0);
        master.setDoubleClickReturnValue (true, 0.0);
        master.setTextValueSuffix (" dB");
        master.setPopupDisplayEnabled (true, true, nullptr);
        master.setTooltip ("Master volume");
        master.onValueChange = [this]
        {
            ctx.engine.masterVolume = Theme::dbToGain (master.getValue());
            if (ctx.mixChanged) ctx.mixChanged();
        };
        Theme::noFocus (master);
        addAndMakeVisible (master);

        mixerB.setTooltip ("Show/hide the mixer (X)");
        mixerB.setClickingTogglesState (false);
        mixerB.setColour (juce::TextButton::buttonOnColourId, Theme::accent.withAlpha (0.7f));
        add (mixerB, cmdToggleMixer);
        Theme::noFocus (mixerB);

        settingsB.setTooltip ("Audio device settings (Ctrl+,)");
        add (settingsB, cmdSettings);
        Theme::noFocus (settingsB);

        sync();
    }

    void sync()
    {
        auto& e = ctx.engine;
        playB.setToggleState (e.isPlaying() && ! e.isRecording(), juce::dontSendNotification);
        recB.setToggleState (e.isRecording(), juce::dontSendNotification);
        cycleB.setToggleState (e.cycleOn.load(), juce::dontSendNotification);
        metroB.setToggleState (e.metronomeOn.load(), juce::dontSendNotification);
        if (! master.isMouseButtonDown())
            master.setValue (Theme::gainToDb (e.masterVolume.load()), juce::dontSendNotification);
        lcd.refresh();
    }

    void setMixerShown (bool shown) { mixerB.setToggleState (shown, juce::dontSendNotification); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.0f, true));
        g.drawText ("MASTER", masterLabel, juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);

        auto right = r.removeFromRight (80);
        settingsB.setBounds (right.withSizeKeepingCentre (80, 28));
        r.removeFromRight (8);
        mixerB.setBounds (r.removeFromRight (64).withSizeKeepingCentre (64, 28));
        r.removeFromRight (12);
        master.setBounds (r.removeFromRight (130).withSizeKeepingCentre (130, 24));
        masterLabel = r.removeFromRight (56);

        const int bw = 38;
        for (auto* b : { (juce::Button*) &toStart, (juce::Button*) &back, (juce::Button*) &forward,
                         (juce::Button*) &stopB, (juce::Button*) &playB, (juce::Button*) &recB })
        {
            b->setBounds (r.removeFromLeft (bw).withSizeKeepingCentre (bw - 2, 32));
        }
        r.removeFromLeft (10);
        cycleB.setBounds (r.removeFromLeft (bw).withSizeKeepingCentre (bw - 2, 32));
        metroB.setBounds (r.removeFromLeft (bw).withSizeKeepingCentre (bw - 2, 32));
        r.removeFromLeft (14);

        const int lcdW = juce::jmin (r.getWidth(), 480);
        lcd.setBounds (r.removeFromLeft (lcdW).withSizeKeepingCentre (lcdW, 46));
    }

private:
    AppContext& ctx;
    Lcd lcd;
    TransportButton toStart, back, forward, stopB, playB, recB, cycleB, metroB;
    juce::Slider master;
    juce::TextButton mixerB { "Mixer" }, settingsB { "Settings" };
    juce::Rectangle<int> masterLabel;
};
