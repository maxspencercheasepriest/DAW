#pragma once

#include "AppContext.h"
#include "Drummer.h"

// The Session Drummer editor: settings for the selected drummer region.
// Every change regenerates the region's MIDI straight away.
class DrummerPanel : public juce::Component, private juce::Timer
{
public:
    explicit DrummerPanel (AppContext& c) : ctx (c)
    {
        style.addItemList (Drummer::styleNames(), 1);
        style.onChange = [this] { apply ("Drummer Style"); };
        addAndMakeVisible (style);

        auto setupSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& left, const juce::String& right)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (0.0, 1.0, 0.01);
            s.setColour (juce::Slider::thumbColourId, Theme::cycle);
            s.setColour (juce::Slider::trackColourId, Theme::cycle.withAlpha (0.6f));
            s.onValueChange = [this] { apply ({}); };          // live while dragging
            s.onDragEnd = [this] { ctx.commitEdit ("Drummer Settings"); };
            Theme::noFocus (s);
            addAndMakeVisible (s);
            l.setText (left + "|" + right, juce::dontSendNotification);   // drawn as two ends in paint()
        };
        setupSlider (complexity, complexityLabel, "SIMPLE", "COMPLEX");
        setupSlider (loudness, loudnessLabel, "SOFT", "LOUD");
        setupSlider (fills, fillsLabel, "FEW FILLS", "MORE FILLS");

        variation.onClick = [this]
        {
            if (clip == nullptr || clip->drummer == nullptr) return;
            auto s = *clip->drummer;
            s.seed = juce::Random::getSystemRandom().nextInt (100000) + 1;
            ctx.engine.updateDrummer (clip, s);
            ctx.commitEdit ("New Drummer Variation");
            refreshArrange();
        };
        addAndMakeVisible (variation);

        convert.onClick = [this]
        {
            if (clip == nullptr) return;
            ctx.engine.convertToMidi (clip);
            ctx.commitEdit ("Convert to MIDI");
            setClip (nullptr);
            refreshArrange();
        };
        convert.setTooltip ("Turn this region into ordinary MIDI notes the drummer won't change again");
        addAndMakeVisible (convert);

        for (auto* comp : std::initializer_list<juce::Component*> { &style, &variation, &convert })
            Theme::noFocus (*comp);

        setSize (420, 300);
        setClip (nullptr);
        startTimerHz (4);
    }

    void setClip (const ClipPtr& c)
    {
        clip = (c != nullptr && c->drummer != nullptr) ? c : nullptr;
        const bool on = clip != nullptr;
        for (auto* comp : std::initializer_list<juce::Component*> { &style, &complexity, &loudness, &fills, &variation, &convert })
            comp->setEnabled (on);

        if (on)
        {
            const auto& d = *clip->drummer;
            style.setSelectedItemIndex (d.style, juce::dontSendNotification);
            complexity.setValue (d.complexity, juce::dontSendNotification);
            loudness.setValue (d.loudness, juce::dontSendNotification);
            fills.setValue (d.fills, juce::dontSendNotification);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        auto top = getLocalBounds().removeFromTop (48);
        g.setColour (Theme::panelDark);
        g.fillRect (top);
        g.setColour (Theme::cycle);
        g.fillRect (top.removeFromBottom (2));
        g.setFont (Theme::font (18.0f, true));
        g.drawText ("DRUMMER", 16, 6, 200, 22, juce::Justification::centredLeft);

        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.5f));
        juce::String info = "Select a drummer region to edit it.";
        if (clip != nullptr)
        {
            const auto& d = *clip->drummer;
            info = "Bars " + juce::String (d.startBar + 1) + "-" + juce::String (d.startBar + d.bars)
                 + "  |  " + juce::String (ctx.engine.beatsPerBar) + "/" + juce::String (ctx.engine.beatUnit)
                 + "  |  " + juce::String (ctx.engine.tempo.load(), 1) + " bpm  |  General MIDI drums";
        }
        g.drawText (info, 16, 27, getWidth() - 32, 16, juce::Justification::centredLeft);

        // slider end labels: SIMPLE ........ COMPLEX
        g.setFont (Theme::font (11.0f, true));
        for (auto* l : { &complexityLabel, &loudnessLabel, &fillsLabel })
        {
            auto r = l->getBounds();
            g.setColour (isEnabled() && clip != nullptr ? Theme::textDim : Theme::textDim.withAlpha (0.4f));
            g.drawText (l->getText().upToFirstOccurrenceOf ("|", false, false), r, juce::Justification::centredLeft);
            g.drawText (l->getText().fromFirstOccurrenceOf ("|", false, false), r, juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16).withTrimmedTop (48);
        style.setBounds (r.removeFromTop (26));
        r.removeFromTop (14);
        for (auto [s, l] : { std::pair<juce::Slider*, juce::Label*> { &complexity, &complexityLabel },
                             { &loudness, &loudnessLabel }, { &fills, &fillsLabel } })
        {
            l->setBounds (r.removeFromTop (16));
            s->setBounds (r.removeFromTop (24));
            r.removeFromTop (8);
        }
        auto row = r.removeFromBottom (28);
        variation.setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        convert.setBounds (row);
    }

private:
    void apply (const juce::String& commitLabel)
    {
        if (clip == nullptr || clip->drummer == nullptr) return;
        auto s = *clip->drummer;
        s.style = juce::jmax (0, style.getSelectedItemIndex());
        s.complexity = (float) complexity.getValue();
        s.loudness = (float) loudness.getValue();
        s.fills = (float) fills.getValue();
        ctx.engine.updateDrummer (clip, s);
        if (commitLabel.isNotEmpty())
            ctx.commitEdit (commitLabel);
        refreshArrange();
    }

    void refreshArrange()
    {
        if (ctx.selectionChanged) ctx.selectionChanged();
        repaint();
    }

    void timerCallback() override
    {
        // the region was deleted or replaced (e.g. by undo)
        if (clip != nullptr && ctx.engine.findOwner (clip) == nullptr)
            setClip (nullptr);
        else
            repaint (0, 0, getWidth(), 48);
    }

    AppContext& ctx;
    ClipPtr clip;
    juce::ComboBox style;
    juce::Slider complexity, loudness, fills;
    juce::Label complexityLabel, loudnessLabel, fillsLabel;
    juce::TextButton variation { "New Variation" }, convert { "Convert to MIDI" };
};

//==============================================================================
class DrummerWindow : public juce::DocumentWindow
{
public:
    DrummerWindow (AppContext& c, std::function<void()> onClose)
        : DocumentWindow ("Drummer", Theme::panel, DocumentWindow::closeButton), closeCallback (std::move (onClose))
    {
        panel = new DrummerPanel (c);
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setAlwaysOnTop (true);
        // bottom-right of the screen, out of the way of the arrange area
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                        : juce::Rectangle<int> (0, 0, 1280, 800);
        setBounds (area.getRight() - panel->getWidth() - 40, area.getBottom() - panel->getHeight() - 60,
                   panel->getWidth(), panel->getHeight());
        setVisible (true);
    }

    void setClip (const ClipPtr& c) { panel->setClip (c); }
    void closeButtonPressed() override { if (closeCallback) closeCallback(); }

private:
    DrummerPanel* panel = nullptr;
    std::function<void()> closeCallback;
};
