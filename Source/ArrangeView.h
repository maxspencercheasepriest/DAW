#pragma once

#include "InsertSlot.h"

//==============================================================================
// Small toggle button used on track headers and channel strips.
inline void setupToggle (juce::TextButton& b, const juce::String& text, juce::Colour onColour, const juce::String& tip)
{
    b.setButtonText (text);
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff111111));
    b.setTooltip (tip);
    Theme::noFocus (b);
}

inline void fillInputCombo (juce::ComboBox& box, const juce::StringArray& inputs, const Track& t)
{
    box.clear (juce::dontSendNotification);
    for (int i = 0; i < inputs.size(); ++i)
        box.addItem ("In " + juce::String (i + 1), i + 1);
    for (int i = 0; i + 1 < inputs.size(); i += 2)
        box.addItem ("In " + juce::String (i + 1) + "-" + juce::String (i + 2), 1000 + i + 1);

    if (inputs.isEmpty())
    {
        box.addItem ("No input", 9999);
        box.setSelectedId (9999, juce::dontSendNotification);
        return;
    }

    const int ch = t.inputChannel.load();
    box.setSelectedId (t.stereoInput.load() ? 1000 + ch + 1 : ch + 1, juce::dontSendNotification);
}

inline void applyInputChoice (int id, Track& t)
{
    if (id <= 0 || id == 9999)
        return;
    if (id > 1000) { t.stereoInput = true;  t.inputChannel = id - 1001; }
    else           { t.stereoInput = false; t.inputChannel = id - 1; }
}

//==============================================================================
class TrackHeader : public juce::Component
{
public:
    TrackHeader (AppContext& c, TrackPtr t) : ctx (c), track (std::move (t))
    {
        name.setText (track->name, juce::dontSendNotification);
        name.setEditable (false, true, false);
        name.setFont (Theme::font (14.0f, true));
        name.setTooltip ("Double-click to rename");
        name.onTextChange = [this]
        {
            auto n = name.getText().trim();
            if (n.isNotEmpty()) track->name = n;
            ctx.engine.dirty = true;
            if (ctx.tracksChanged) ctx.tracksChanged();
            ctx.commitEdit ("Rename Track");
        };
        addAndMakeVisible (name);

        setupToggle (muteB, "M", Theme::mute, "Mute");
        setupToggle (soloB, "S", Theme::solo, "Solo");
        setupToggle (armB, "R", Theme::record, "Record enable");
        setupToggle (monB, "I", Theme::monitor, "Input monitoring (hear the input while armed)");
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

        input.setTooltip ("Input channel");
        input.onChange = [this] { applyInputChoice (input.getSelectedId(), *track); ctx.engine.dirty = true; ctx.commitEdit ("Change Input"); };
        Theme::noFocus (input);

        if (track->isInstrument)
        {
            instrumentSlot = std::make_unique<InsertSlot> (ctx, track, -1);
            addAndMakeVisible (*instrumentSlot);
            monB.setVisible (false);   // instruments always hear their keyboard when selected or armed
        }
        else
        {
            addAndMakeVisible (input);
        }

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (-60.0, 6.0, 0.1);
        volume.setSkewFactorFromMidPoint (-12.0);
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.setTextValueSuffix (" dB");
        volume.setPopupDisplayEnabled (true, true, nullptr);
        volume.setTooltip ("Volume (double-click for 0 dB)");
        volume.onValueChange = [this] { track->volume = Theme::dbToGain (volume.getValue()); changed(); };
        volume.onDragEnd = [this] { ctx.commitEdit ("Volume"); };
        Theme::noFocus (volume);
        addAndMakeVisible (volume);

        // receive clicks on child components too, so any click selects the track
        addMouseListener (this, true);
        refreshInputs();
        sync();
    }

    const TrackPtr& getTrack() const { return track; }

    void refreshInputs() { fillInputCombo (input, ctx.engine.getActiveInputNames(), *track); }

    void sync()
    {
        if (instrumentSlot != nullptr)
            instrumentSlot->refresh();
        muteB.setToggleState (track->mute.load(), juce::dontSendNotification);
        soloB.setToggleState (track->solo.load(), juce::dontSendNotification);
        armB.setToggleState (track->armed.load(), juce::dontSendNotification);
        monB.setToggleState (track->monitor.load(), juce::dontSendNotification);
        if (! volume.isMouseButtonDown())
            volume.setValue (Theme::gainToDb (track->volume.load()), juce::dontSendNotification);
        if (! name.isBeingEdited())
            name.setText (track->name, juce::dontSendNotification);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (ctx.view.selectedTrack != track)
            ctx.selectTrack (track);

        if (e.eventComponent == this && e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem ("Rename", [this] { name.showEditor(); });
            m.addItem ("Delete Track", [this] { ctx.selectTrack (track); ctx.runCommand (cmdDeleteTrack); });
            m.showMenuAsync (juce::PopupMenu::Options());
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool sel = ctx.view.selectedTrack == track;
        g.fillAll (sel ? Theme::panelLight : Theme::panel);
        g.setColour (track->colour);
        g.fillRect (0, 0, 5, getHeight());
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

        auto meter = getLocalBounds().removeFromRight (12).reduced (2, 6).toFloat();
        Theme::drawStereoMeter (g, meter, track->dispL, track->dispR);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (11).withTrimmedRight (16).reduced (0, 5);
        name.setBounds (r.removeFromTop (22).withTrimmedLeft (-4));
        r.removeFromTop (3);

        auto row = r.removeFromTop (22);
        for (auto* b : { &muteB, &soloB, &armB, &monB })
        {
            b->setBounds (row.removeFromLeft (24));
            row.removeFromLeft (3);
        }
        row.removeFromLeft (4);
        input.setBounds (row);
        if (instrumentSlot != nullptr)
            instrumentSlot->setBounds (row.reduced (0, 1));

        r.removeFromTop (5);
        volume.setBounds (r.removeFromTop (20));
    }

private:
    void changed()
    {
        ctx.engine.dirty = true;
        if (ctx.mixChanged) ctx.mixChanged();
    }

    AppContext& ctx;
    TrackPtr track;
    juce::Label name;
    juce::TextButton muteB, soloB, armB, monB;
    juce::ComboBox input;
    juce::Slider volume;
    std::unique_ptr<InsertSlot> instrumentSlot;
};

//==============================================================================
// The timeline area: lanes, regions, grid, cycle area and playhead.
class LanesComponent : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit LanesComponent (AppContext& c) : ctx (c)
    {
        setWantsKeyboardFocus (false);
    }

    std::function<void()> onViewChanged;
    std::function<void (double factor, float x)> onZoom;

    void paint (juce::Graphics& g) override
    {
        auto& e = ctx.engine;
        auto& v = ctx.view;
        const int th = v.trackHeight;
        const int w = getWidth();
        const int n = (int) e.tracks.size();
        const int lanesBottom = n * th;

        g.fillAll (Theme::panelDark);

        for (int i = 0; i < n; ++i)
        {
            const bool sel = e.tracks[(size_t) i] == v.selectedTrack;
            g.setColour (sel ? Theme::laneSel : (i % 2 == 0 ? Theme::lane : Theme::laneAlt));
            g.fillRect (0, i * th, w, th);
        }

        // ---- grid ----
        const double t0 = v.scrollX, t1 = v.xToSec (w);
        const double spb = e.secondsPerBeat(), spbar = e.secondsPerBar();
        const int bpb = e.beatsPerBar;
        const int gridBottom = juce::jmax (lanesBottom, getHeight());

        if (spb * v.pxPerSec >= 12.0)
        {
            g.setColour (Theme::grid);
            for (auto b = (juce::int64) std::floor (t0 / spb); (double) b * spb <= t1; ++b)
                if (b % bpb != 0)
                    g.drawVerticalLine ((int) v.secToX ((double) b * spb), 0.0f, (float) gridBottom);
        }

        int barStep = 1;
        while (spbar * barStep * v.pxPerSec < 18.0) barStep *= 2;
        g.setColour (Theme::gridBar);
        for (auto bar = (juce::int64) std::floor (t0 / spbar); (double) bar * spbar <= t1; ++bar)
            if (bar % barStep == 0)
                g.drawVerticalLine ((int) v.secToX ((double) bar * spbar), 0.0f, (float) gridBottom);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        for (int i = 1; i <= n; ++i)
            g.drawHorizontalLine (i * th - 1, 0.0f, (float) w);

        if (e.cycleOn)
        {
            const float x0 = v.secToX (e.cycleStart), x1 = v.secToX (e.cycleEnd);
            g.setColour (Theme::cycle.withAlpha (0.06f));
            g.fillRect (x0, 0.0f, x1 - x0, (float) gridBottom);
        }

        // ---- regions ----
        for (int i = 0; i < n; ++i)
        {
            auto& t = *e.tracks[(size_t) i];
            for (auto& c : t.clips)
                drawClip (g, *c, t.colour, i, c == v.selectedClip);

            // live recording region
            const auto rs = e.getRecordingStart (t);
            if (rs >= 0)
            {
                const float x0 = v.secToX ((double) rs / e.getSampleRate());
                const float x1 = v.secToX (e.getPosition());
                auto r = juce::Rectangle<float> (x0, (float) (i * th + 2), juce::jmax (0.0f, x1 - x0), (float) (th - 4));
                g.setColour (Theme::record.withAlpha (0.35f));
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (Theme::record);
                g.drawRoundedRectangle (r, 4.0f, 1.2f);
            }
        }

        // ---- playhead ----
        const float px = v.secToX (e.getPosition());
        if (px >= 0.0f && px <= (float) w)
        {
            g.setColour (e.isRecording() ? Theme::record : juce::Colours::white.withAlpha (0.85f));
            g.fillRect (px - 0.5f, 0.0f, 1.5f, (float) gridBottom);
        }

        if (dropHighlightY >= 0)
        {
            g.setColour (Theme::accent.withAlpha (0.25f));
            g.fillRect (0, (dropHighlightY / th) * th, w, th);
        }
    }

    // ---- mouse ----------------------------------------------------------
    void mouseMove (const juce::MouseEvent& e) override
    {
        ClipPtr c; int idx = -1;
        auto mode = hitTestClip (e.position, c, idx);
        setMouseCursor (mode == Drag::trimL || mode == Drag::trimR ? juce::MouseCursor::LeftRightResizeCursor
                                                                   : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& eng = ctx.engine;
        ClipPtr clip; int idx = -1;
        drag = hitTestClip (e.position, clip, idx);
        dragChanged = false;

        const int trackIdx = (int) (e.position.y / ctx.view.trackHeight);
        if (trackIdx >= 0 && trackIdx < (int) eng.tracks.size())
            ctx.view.selectedTrack = eng.tracks[(size_t) trackIdx];

        ctx.view.selectedClip = clip;
        if (clip != nullptr)
        {
            origStart = clip->start;
            origOffset = clip->offset;
            origLength = clip->length;
            downSec = ctx.view.xToSec (e.position.x);
        }

        if (ctx.selectionChanged) ctx.selectionChanged();

        if (e.mods.isPopupMenu())
        {
            drag = Drag::none;
            showContextMenu (clip, ctx.view.xToSec (e.position.x));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto clip = ctx.view.selectedClip;
        if (clip == nullptr || drag == Drag::none || ctx.engine.isRecording())
            return;

        if (! dragChanged && e.getDistanceFromDragStart() < 3)
            return;

        auto& eng = ctx.engine;
        const bool free = e.mods.isAltDown();
        const double dsec = ctx.view.xToSec (e.position.x) - downSec;
        const double fileLen = clip->fileLength();
        const double minLen = 0.01;
        const double origEnd = origStart + origLength;

        {
            const juce::ScopedLock sl (eng.lock);
            if (drag == Drag::move)
            {
                clip->start = juce::jmax (0.0, ctx.snap (origStart + dsec, free));
            }
            else if (drag == Drag::trimL)
            {
                double ns = ctx.snap (origStart + dsec, free);
                ns = juce::jlimit (juce::jmax (0.0, origStart - origOffset), origEnd - minLen, ns);
                clip->offset = origOffset + (ns - origStart);
                clip->length = origEnd - ns;
                clip->start = ns;
            }
            else if (drag == Drag::trimR)
            {
                double ne = ctx.snap (origEnd + dsec, free);
                ne = juce::jlimit (origStart + minLen, origStart + (fileLen - origOffset), ne);
                clip->length = ne - origStart;
            }
        }

        if (drag == Drag::move)
        {
            const int idx = juce::jlimit (0, (int) eng.tracks.size() - 1, (int) (e.position.y / ctx.view.trackHeight));
            auto dest = eng.tracks[(size_t) idx];
            if (eng.findOwner (clip) != dest)
            {
                eng.moveClipToTrack (clip, dest);
                ctx.view.selectedTrack = dest;
                if (ctx.selectionChanged) ctx.selectionChanged();
            }
        }

        dragChanged = true;
        eng.dirty = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto finished = drag;
        drag = Drag::none;
        if (! dragChanged || ctx.view.selectedClip == nullptr)
            return;

        dragChanged = false;
        auto clip = ctx.view.selectedClip;
        if (clip->drummer != nullptr)
            ctx.engine.conformDrummerRegion (clip);   // drummer regions snap to whole bars
        ctx.commitEdit (finished == Drag::move ? "Move Region" : "Resize Region");
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        ClipPtr clip; int idx = -1;
        if (hitTestClip (e.position, clip, idx) != Drag::none && clip->drummer != nullptr)
        {
            if (ctx.openDrummer) ctx.openDrummer (clip);
            return;
        }

        // double-click empty space below the tracks to add a track
        if (e.position.y > (float) (ctx.engine.tracks.size() * (size_t) ctx.view.trackHeight))
            ctx.runCommand (cmdNewTrack);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            if (onZoom) onZoom (w.deltaY > 0 ? 1.25 : 0.8, e.position.x);
            return;
        }

        const float d = e.mods.isShiftDown() ? (! juce::approximatelyEqual (w.deltaY, 0.0f) ? w.deltaY : w.deltaX) : w.deltaX;
        if (std::abs (d) > 0.0f)
        {
            const double visible = getWidth() / ctx.view.pxPerSec;
            ctx.view.scrollX = juce::jmax (0.0, ctx.view.scrollX - d * visible * 0.25);
            if (onViewChanged) onViewChanged();
            return;
        }

        juce::Component::mouseWheelMove (e, w);   // vertical scroll goes to the viewport
    }

    // ---- file drag & drop from Explorer --------------------------------------
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        auto wildcard = ctx.engine.formatManager.getWildcardForAllFormats();
        for (auto& f : files)
            if (juce::File (f).hasFileExtension (wildcard.removeCharacters ("*")))
                return true;
        return false;
    }

    void fileDragMove (const juce::StringArray&, int, int y) override  { dropHighlightY = y; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { dropHighlightY = -1; repaint(); }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        dropHighlightY = -1;
        auto& eng = ctx.engine;
        int idx = y / ctx.view.trackHeight;
        const double start = juce::jmax (0.0, ctx.snap (ctx.view.xToSec (x), false));
        juce::StringArray errors;

        for (auto& path : files)
        {
            juce::File f (path);
            TrackPtr target = idx < (int) eng.tracks.size() ? eng.tracks[(size_t) idx]
                                                              : eng.addTrack (f.getFileNameWithoutExtension());
            juce::String err;
            if (! eng.importFile (f, target, start, err))
                errors.add (err);
            ++idx;
        }

        if (ctx.tracksChanged) ctx.tracksChanged();
        if (errors.size() > 0 && ctx.showMessage)
            ctx.showMessage ("Import", errors.joinIntoString ("\n"));
        ctx.commitEdit ("Import Audio");
        repaint();
    }

private:
    enum class Drag { none, move, trimL, trimR };

    Drag hitTestClip (juce::Point<float> p, ClipPtr& out, int& trackIdx)
    {
        auto& eng = ctx.engine;
        const int th = ctx.view.trackHeight;
        trackIdx = (int) (p.y / th);
        if (p.y < 0 || trackIdx >= (int) eng.tracks.size())
            return Drag::none;

        auto& clips = eng.tracks[(size_t) trackIdx]->clips;
        for (auto it = clips.rbegin(); it != clips.rend(); ++it)
        {
            auto r = clipRect (**it, trackIdx);
            if (r.contains (p))
            {
                out = *it;
                const float edge = juce::jmin (8.0f, r.getWidth() * 0.25f);
                if (p.x < r.getX() + edge)     return Drag::trimL;
                if (p.x > r.getRight() - edge) return Drag::trimR;
                return Drag::move;
            }
        }
        return Drag::none;
    }

    juce::Rectangle<float> clipRect (const Clip& c, int trackIdx) const
    {
        const int th = ctx.view.trackHeight;
        return { ctx.view.secToX (c.start), (float) (trackIdx * th + 2),
                 (float) (c.length * ctx.view.pxPerSec), (float) (th - 4) };
    }

    void drawClip (juce::Graphics& g, const Clip& c, juce::Colour col, int trackIdx, bool selected)
    {
        auto r = clipRect (c, trackIdx);
        if (r.getRight() < 0.0f || r.getX() > (float) getWidth())
            return;

        g.setColour (col.withAlpha (selected ? 0.5f : 0.32f));
        g.fillRoundedRectangle (r, 4.0f);

        auto hdr = r.withHeight (15.0f);
        g.setColour (col.withAlpha (selected ? 1.0f : 0.82f));
        g.fillRoundedRectangle (hdr, 4.0f);
        g.fillRect (hdr.withTrimmedTop (6.0f));

        if (r.getWidth() > 24.0f)
        {
            g.setColour (juce::Colour (0xff111111));
            g.setFont (Theme::font (11.0f, true));
            auto textArea = hdr.reduced (5.0f, 0.0f);
            if (textArea.getX() < 0.0f)
                textArea.setLeft (2.0f);
            g.drawText (c.displayName(), textArea, juce::Justification::centredLeft, true);
        }

        auto body = r.withTrimmedTop (16.0f).reduced (0.0f, 2.0f);
        g.setColour (col.brighter (0.7f).withAlpha (0.9f));
        if (c.isMidi)
            drawNotes (g, c, body);
        else
            drawWave (g, c, body);

        g.setColour (selected ? juce::Colours::white : col.darker (0.4f));
        g.drawRoundedRectangle (r, 4.0f, selected ? 1.6f : 1.0f);
    }

    void drawNotes (juce::Graphics& g, const Clip& c, juce::Rectangle<float> body)
    {
        if (c.notes == nullptr || c.notes->empty() || body.getHeight() < 4.0f)
            return;

        int lo = 127, hi = 0;
        for (auto& n : *c.notes) { lo = juce::jmin (lo, n.note); hi = juce::jmax (hi, n.note); }
        lo = juce::jmax (0, lo - 2);
        hi = juce::jmin (127, hi + 2);
        const float rowH = juce::jlimit (1.5f, 6.0f, body.getHeight() / (float) (hi - lo + 1));
        const double origin = c.start - c.offset;

        g.saveState();
        g.reduceClipRegion (body.toNearestInt());
        for (auto& n : *c.notes)
        {
            const double on = origin + n.time;
            if (on + n.length < c.start || on >= c.end())
                continue;
            const float x0 = ctx.view.secToX (juce::jmax (on, c.start));
            const float x1 = ctx.view.secToX (juce::jmin (on + n.length, c.end()));
            const float y = body.getBottom() - (float) (n.note - lo + 1) * (body.getHeight() / (float) (hi - lo + 1));
            g.fillRect (x0, y, juce::jmax (1.5f, x1 - x0), rowH);
        }
        g.restoreState();
    }

    void drawWave (juce::Graphics& g, const Clip& c, juce::Rectangle<float> body)
    {
        if (c.peaks == nullptr || c.peaks->empty() || body.getHeight() < 4.0f)
            return;

        const auto& pk = *c.peaks;
        const int numPeaks = (int) pk.size();
        const double scale = c.fileSampleRate / Clip::peakBlock;
        const float mid = body.getCentreY(), half = body.getHeight() * 0.5f;
        const int x0 = (int) juce::jmax (body.getX(), 0.0f);
        const int x1 = (int) juce::jmin (body.getRight(), (float) getWidth());

        for (int x = x0; x < x1; ++x)
        {
            const double s0 = (ctx.view.xToSec (x) - c.start + c.offset) * scale;
            const double s1 = (ctx.view.xToSec (x + 1) - c.start + c.offset) * scale;
            const int i0 = juce::jmax (0, (int) s0);
            const int i1 = juce::jmin (numPeaks, juce::jmax (i0 + 1, (int) s1));
            float mn = 0.0f, mx = 0.0f;
            for (int i = i0; i < i1; ++i)
            {
                mn = juce::jmin (mn, pk[(size_t) i].first);
                mx = juce::jmax (mx, pk[(size_t) i].second);
            }
            g.drawVerticalLine (x, mid - juce::jmin (1.0f, mx) * half, mid - juce::jmax (-1.0f, mn) * half + 1.0f);
        }
    }

    void showContextMenu (const ClipPtr& clip, double atSec)
    {
        juce::PopupMenu m;
        if (clip != nullptr)
        {
            if (clip->drummer != nullptr)
            {
                m.addItem ("Drummer Settings...", [this, clip] { if (ctx.openDrummer) ctx.openDrummer (clip); });
                m.addItem ("Convert to MIDI Region", [this, clip]
                {
                    ctx.engine.convertToMidi (clip);
                    ctx.commitEdit ("Convert to MIDI");
                    repaint();
                });
            }
            m.addItem ("Split at Playhead", [this] { ctx.runCommand (cmdSplitAtPlayhead); });
            m.addItem ("Delete Region", [this] { ctx.runCommand (cmdDeleteClip); });
            m.addSeparator();
        }

        auto track = ctx.view.selectedTrack;
        if (clip == nullptr && track != nullptr && track->isDrummer)
        {
            m.addItem ("Add Drummer Region Here", [this, track, atSec]
            {
                const int bar = juce::jmax (0, (int) std::floor (atSec / ctx.engine.secondsPerBar()));
                auto c = ctx.engine.addDrummerRegion (track, bar, 8);
                ctx.view.selectedClip = c;
                ctx.commitEdit ("Add Drummer Region");
                repaint();
            });
            m.addSeparator();
        }
        m.addItem ("Import Audio File...", [this] { ctx.runCommand (cmdImportAudio); });
        m.addItem ("New Audio Track", [this] { ctx.runCommand (cmdNewTrack); });
        m.addItem ("New Instrument Track", [this] { ctx.runCommand (cmdNewInstrumentTrack); });
        m.addItem ("New Drummer Track", [this] { ctx.runCommand (cmdNewDrummerTrack); });
        m.showMenuAsync (juce::PopupMenu::Options());
    }

    AppContext& ctx;
    Drag drag = Drag::none;
    bool dragChanged = false;
    double origStart = 0, origOffset = 0, origLength = 0, downSec = 0;
    int dropHighlightY = -1;
};

//==============================================================================
// Bar ruler with cycle area (top strip) and locate area (bottom).
class Ruler : public juce::Component
{
public:
    explicit Ruler (AppContext& c) : ctx (c) {}

    std::function<void()> onViewChanged;
    static constexpr int cycleHeight = 13;

    void paint (juce::Graphics& g) override
    {
        auto& e = ctx.engine;
        auto& v = ctx.view;
        const int w = getWidth(), h = getHeight();

        g.fillAll (Theme::panel);
        g.setColour (Theme::panelDark);
        g.fillRect (0, 0, w, cycleHeight);

        // cycle area
        {
            const float x0 = v.secToX (e.cycleStart), x1 = v.secToX (e.cycleEnd);
            g.setColour (e.cycleOn ? Theme::cycle : Theme::textDim.withAlpha (0.35f));
            g.fillRoundedRectangle (x0, 2.0f, juce::jmax (2.0f, x1 - x0), (float) cycleHeight - 3.0f, 2.0f);
        }

        const double t0 = v.scrollX, t1 = v.xToSec (w);
        const double spb = e.secondsPerBeat(), spbar = e.secondsPerBar();

        int barStep = 1;
        while (spbar * barStep * v.pxPerSec < 34.0) barStep *= 2;

        g.setFont (Theme::font (11.0f, true));
        for (auto bar = (juce::int64) std::floor (t0 / spbar); (double) bar * spbar <= t1; ++bar)
        {
            const float x = v.secToX ((double) bar * spbar);
            if (bar % barStep == 0)
            {
                g.setColour (Theme::textDim);
                g.drawVerticalLine ((int) x, (float) cycleHeight, (float) h);
                g.setColour (Theme::text);
                g.drawText (juce::String (bar + 1), (int) x + 3, cycleHeight + 1, 40, h - cycleHeight - 2,
                            juce::Justification::topLeft);
            }
            else if (spbar * v.pxPerSec >= 6.0)
            {
                g.setColour (Theme::textDim.withAlpha (0.5f));
                g.drawVerticalLine ((int) x, (float) h - 6.0f, (float) h);
            }

            if (spb * v.pxPerSec >= 10.0)
            {
                g.setColour (Theme::textDim.withAlpha (0.45f));
                for (int b = 1; b < e.beatsPerBar; ++b)
                    g.drawVerticalLine ((int) v.secToX ((double) bar * spbar + b * spb), (float) h - 4.0f, (float) h);
            }
        }

        // playhead marker
        const float px = v.secToX (e.getPosition());
        juce::Path tri;
        tri.addTriangle (px - 6.0f, (float) h - 9.0f, px + 6.0f, (float) h - 9.0f, px, (float) h);
        g.setColour (e.isRecording() ? Theme::record : juce::Colours::white);
        g.fillPath (tri);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (h - 1, 0.0f, (float) w);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        inCycleArea = e.position.y < cycleHeight;
        cycleDragged = false;
        cycleAnchor = ctx.snap (ctx.view.xToSec (e.position.x), e.mods.isAltDown());
        if (! inCycleArea)
            locate (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! inCycleArea)
        {
            locate (e);
            return;
        }

        if (e.getDistanceFromDragStart() < 3)
            return;

        auto& eng = ctx.engine;
        const double s = juce::jmax (0.0, ctx.snap (ctx.view.xToSec (e.position.x), e.mods.isAltDown()));
        const double a = juce::jmax (0.0, cycleAnchor);
        if (std::abs (s - a) > 0.01)
        {
            eng.cycleStart = juce::jmin (a, s);
            eng.cycleEnd = juce::jmax (a, s);
            eng.cycleOn = true;
            eng.dirty = true;
        }
        cycleDragged = true;
        changed();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (inCycleArea && ! cycleDragged)
            ctx.runCommand (cmdCycle);   // click the cycle area to toggle it, like Logic
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (onZoom) onZoom (w.deltaY > 0 ? 1.25 : 0.8, e.position.x);
    }

    std::function<void (double, float)> onZoom;

private:
    void locate (const juce::MouseEvent& e)
    {
        ctx.engine.setPosition (juce::jmax (0.0, ctx.snap (ctx.view.xToSec (e.position.x), e.mods.isAltDown())));
        changed();
    }

    void changed()
    {
        if (onViewChanged) onViewChanged();
    }

    AppContext& ctx;
    bool inCycleArea = false, cycleDragged = false;
    double cycleAnchor = 0.0;
};

//==============================================================================
// Holds the track headers on the left and the lanes on the right; lives in a Viewport.
class TracksContent : public juce::Component
{
public:
    explicit TracksContent (AppContext& c) : ctx (c), lanes (c)
    {
        addAndMakeVisible (lanes);
    }

    void rebuild()
    {
        headers.clear();
        for (auto& t : ctx.engine.tracks)
        {
            auto h = std::make_unique<TrackHeader> (ctx, t);
            addAndMakeVisible (*h);
            headers.push_back (std::move (h));
        }
        resized();
    }

    void resized() override
    {
        const int th = ctx.view.trackHeight;
        const int hw = ctx.view.headerWidth;
        for (size_t i = 0; i < headers.size(); ++i)
            headers[i]->setBounds (0, (int) i * th, hw, th);
        lanes.setBounds (hw, 0, juce::jmax (0, getWidth() - hw), getHeight());
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::panelDark);
        g.fillRect (0, (int) headers.size() * ctx.view.trackHeight, ctx.view.headerWidth, getHeight());
        g.setColour (juce::Colours::black);
        g.drawVerticalLine (ctx.view.headerWidth - 1, 0.0f, (float) getHeight());
    }

    AppContext& ctx;
    LanesComponent lanes;
    std::vector<std::unique_ptr<TrackHeader>> headers;
};

//==============================================================================
class ArrangeView : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit ArrangeView (AppContext& c) : ctx (c), ruler (c), content (c)
    {
        addAndMakeVisible (ruler);
        addAndMakeVisible (viewport);
        addAndMakeVisible (hbar);
        addAndMakeVisible (addTrackB);

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (12);
        Theme::noFocus (viewport);

        hbar.addListener (this);
        hbar.setAutoHide (false);
        Theme::noFocus (hbar);

        addTrackB.setTooltip ("New track");
        addTrackB.onClick = [this]
        {
            juce::PopupMenu m;
            m.addItem ("New Audio Track", [this] { ctx.runCommand (cmdNewTrack); });
            m.addItem ("New Instrument Track", [this] { ctx.runCommand (cmdNewInstrumentTrack); });
            m.addItem ("New Drummer Track", [this] { ctx.runCommand (cmdNewDrummerTrack); });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addTrackB));
        };
        Theme::noFocus (addTrackB);

        auto viewChanged = [this] { updateScroll(); repaintTimeline(); };
        ruler.onViewChanged = viewChanged;
        content.lanes.onViewChanged = viewChanged;
        ruler.onZoom = [this] (double f, float x) { zoom (f, x); };
        content.lanes.onZoom = [this] (double f, float x) { zoom (f, x); };

        rebuild();
    }

    void rebuild()
    {
        content.rebuild();
        layoutContent();
        repaintTimeline();
    }

    void syncControls()
    {
        for (auto& h : content.headers)
            h->sync();
    }

    void refreshInputs()
    {
        for (auto& h : content.headers)
            h->refreshInputs();
    }

    void selectionChanged()
    {
        for (auto& h : content.headers)
            h->repaint();
        repaintTimeline();
    }

    void updateMeters()
    {
        for (auto& h : content.headers)
            h->repaint (h->getWidth() - 14, 0, 14, h->getHeight());
    }

    // Called ~30 times a second.
    void tick()
    {
        auto& e = ctx.engine;
        const double pos = e.getPosition();

        if (e.isPlaying())
        {
            const double visible = lanesWidth() / ctx.view.pxPerSec;
            if (pos > ctx.view.scrollX + visible * 0.92 || pos < ctx.view.scrollX)
            {
                ctx.view.scrollX = juce::jmax (0.0, pos - visible * 0.08);
                updateScroll();
            }
        }

        if (e.isPlaying() || ! juce::approximatelyEqual (pos, lastPos))
            repaintTimeline();
        lastPos = pos;
    }

    void zoom (double factor, float xInLanes = -1.0f)
    {
        auto& v = ctx.view;
        if (xInLanes < 0.0f)
        {
            const float px = v.secToX (ctx.engine.getPosition());
            xInLanes = (px >= 0.0f && px <= (float) lanesWidth()) ? px : 0.0f;
        }
        const double anchor = v.xToSec (xInLanes);
        v.pxPerSec = juce::jlimit (2.0, 4000.0, v.pxPerSec * factor);
        v.scrollX = juce::jmax (0.0, anchor - xInLanes / v.pxPerSec);
        updateScroll();
        repaintTimeline();
    }

    void zoomToFit()
    {
        const double end = juce::jmax (ctx.engine.getProjectEnd(), ctx.engine.secondsPerBar() * 8.0);
        ctx.view.pxPerSec = juce::jlimit (2.0, 4000.0, (lanesWidth() - 20) / end);
        ctx.view.scrollX = 0.0;
        updateScroll();
        repaintTimeline();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (34);
        auto corner = top.removeFromLeft (ctx.view.headerWidth);
        addTrackB.setBounds (corner.removeFromRight (34).reduced (5));
        cornerArea = corner;
        ruler.setBounds (top.withWidth (juce::jmax (0, top.getWidth() - viewport.getScrollBarThickness())));

        auto bottom = r.removeFromBottom (14);
        hbar.setBounds (bottom.withTrimmedLeft (ctx.view.headerWidth).withTrimmedRight (viewport.getScrollBarThickness()));
        viewport.setBounds (r);
        layoutContent();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.0f, true));
        g.drawText ("TRACKS", cornerArea.withTrimmedLeft (12), juce::Justification::centredLeft);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (33, 0.0f, (float) getWidth());
    }

    void repaintTimeline()
    {
        ruler.repaint();
        content.lanes.repaint();
    }

private:
    int lanesWidth() const { return juce::jmax (1, content.lanes.getWidth()); }

    void layoutContent()
    {
        const int h = juce::jmax ((int) ctx.engine.tracks.size() * ctx.view.trackHeight + 120, viewport.getHeight());
        content.setSize (viewport.getMaximumVisibleWidth(), h);
        updateScroll();
    }

    void updateScroll()
    {
        const double visible = lanesWidth() / ctx.view.pxPerSec;
        const double total = juce::jmax (ctx.engine.getProjectEnd() + visible * 0.5,
                                         ctx.view.scrollX + visible,
                                         ctx.engine.secondsPerBar() * 64.0);
        hbar.setRangeLimits (0.0, total, juce::dontSendNotification);
        hbar.setCurrentRange (ctx.view.scrollX, visible, juce::dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override
    {
        ctx.view.scrollX = juce::jmax (0.0, newRangeStart);
        repaintTimeline();
    }

    AppContext& ctx;
    Ruler ruler;
    TracksContent content;
    juce::Viewport viewport;
    juce::ScrollBar hbar { false };
    juce::TextButton addTrackB { "+" };
    juce::Rectangle<int> cornerArea;
    double lastPos = -1.0;
};
