#include "MainComponent.h"

using namespace juce;

MainComponent::MainComponent (ApplicationProperties& props) : properties (props)
{
    LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    setLookAndFeel (&lookAndFeel);

    SafePointer<MainComponent> safeThis (this);
    ctx.tracksChanged    = [this, safeThis]
    {
        // deferred: the caller may be a component that the rebuild deletes
        if (rebuildPending) return;
        rebuildPending = true;
        auto sp = safeThis;
        MessageManager::callAsync ([sp]
        {
            if (sp != nullptr) { sp->rebuildPending = false; sp->rebuildTracks(); }
        });
    };
    ctx.mixChanged       = [this] { syncControls(); };
    ctx.selectionChanged = [this] { arrange.selectionChanged(); mixer.repaint(); };
    ctx.runCommand       = [this] (int c) { runCommand (c); };
    ctx.showMessage      = [this] (const String& t, const String& m) { showMessage (t, m); };

    ctx.loadPlugin   = [this] (const TrackPtr& t, int s, const PluginDescription& d) { loadPluginInto (t, s, d); };
    ctx.openPlugin   = [this] (const TrackPtr& t, int s) { pluginWindows.open (t, s); };
    ctx.removePlugin = [this] (const TrackPtr& t, int s)
    {
        pluginWindows.closeFor (t->pluginAt (s));
        disposeLater (engine.setInsert (*t, s, nullptr));
        syncControls();
        updateTitle();
    };

    // Remember which plugins were found by previous scans.
    if (auto xml = properties.getUserSettings()->getXmlValue ("knownPlugins"))
        engine.knownPlugins.recreateFromXml (*xml);
    engine.knownPlugins.addChangeListener (this);

    engine.onRecordingFinished = [this]
    {
        transport.sync();
        arrange.repaintTimeline();
        updateTitle();
    };

    // ---- audio device ----
    auto saved = properties.getUserSettings()->getXmlValue ("audioDeviceState");
    const auto err = engine.deviceManager.initialise (2, 2, saved.get(), true);
    engine.deviceManager.addChangeListener (this);

    // MIDI keyboards: switch on every connected input and feed them to the engine.
    engine.enableAllMidiInputs();
    engine.deviceManager.addMidiInputDeviceCallback ({}, &engine.midiCollector);

    if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
    {
        const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
        for (size_t i = 0; i < engine.tracks.size() && numIn > 0; ++i)
            engine.tracks[i]->inputChannel = (int) i % numIn;
    }
    engine.dirty = false;

   #if JUCE_MAC
    MenuBarModel::setMacMainMenu (this);   // use the Mac's top menu bar
   #else
    addAndMakeVisible (menuBar);
   #endif
    addAndMakeVisible (transport);
    addAndMakeVisible (arrange);
    addChildComponent (mixer);

    setWantsKeyboardFocus (true);
    rebuildTracks();
    setSize (1440, 880);
    startTimerHz (30);

    if (err.isNotEmpty())
        MessageManager::callAsync ([this, err] { showMessage ("Audio device", "Couldn't open the audio device:\n" + err
                                                              + "\n\nChoose one in Settings (Ctrl+,)."); });
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveDeviceState();
    engine.deviceManager.removeChangeListener (this);
    engine.deviceManager.removeMidiInputDeviceCallback ({}, &engine.midiCollector);
    engine.knownPlugins.removeChangeListener (this);
    pluginManager.reset();
    pluginWindows.closeAll();
   #if JUCE_MAC
    MenuBarModel::setMacMainMenu (nullptr);
   #endif
    setLookAndFeel (nullptr);
    LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::paint (Graphics& g)
{
    g.fillAll (Theme::bg);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
   #if ! JUCE_MAC
    menuBar.setBounds (r.removeFromTop (LookAndFeel::getDefaultLookAndFeel().getDefaultMenuBarHeight()));
   #endif
    transport.setBounds (r.removeFromTop (60));

    mixer.setVisible (mixerVisible);
    if (mixerVisible)
        mixer.setBounds (r.removeFromBottom (jmin (500, r.getHeight() * 3 / 5)));

    arrange.setBounds (r);
}

//==============================================================================
void MainComponent::timerCallback()
{
    auto decay = [] (std::atomic<float>& peak, float& disp)
    {
        const float p = peak.exchange (0.0f);
        disp = jmax (p, disp * 0.84f);
        if (disp < 0.0005f) disp = 0.0f;
    };

    for (auto& t : engine.tracks)
    {
        decay (t->peakL, t->dispL);
        decay (t->peakR, t->dispR);
    }
    decay (engine.masterPeakL, engine.masterDispL);
    decay (engine.masterPeakR, engine.masterDispR);

    transport.sync();
    arrange.tick();
    arrange.updateMeters();
    if (mixerVisible)
        mixer.updateMeters();

    if (++meterDivider % 15 == 0)
        updateTitle();

    // The selected instrument track plays whatever you play on the keyboard.
    auto sel = ctx.view.selectedTrack;
    engine.midiFocus = (sel != nullptr && sel->isInstrument) ? sel.get() : nullptr;

    // Pick up keyboards plugged in while the app is running.
    if (meterDivider % 90 == 0)
        engine.enableAllMidiInputs();

    // Keep DAW shortcuts working if focus got lost (e.g. after a dialog) - but only
    // while the main window itself is the active one. Plugin windows (and the text
    // boxes inside them) must keep the keyboard while you're typing in them.
    auto* top = dynamic_cast<TopLevelWindow*> (getTopLevelComponent());
    if (top != nullptr && top->isActiveWindow()
        && Component::getCurrentlyFocusedComponent() == nullptr && isShowing() && ! hasKeyboardFocus (true)
        && ! top->isCurrentlyBlockedByAnotherModalComponent())
        grabKeyboardFocus();
}

void MainComponent::changeListenerCallback (ChangeBroadcaster* source)
{
    if (source == &engine.knownPlugins)
    {
        if (auto xml = engine.knownPlugins.createXml())
            properties.getUserSettings()->setValue ("knownPlugins", xml.get());
        properties.saveIfNeeded();
        return;
    }

    arrange.refreshInputs();
    mixer.refreshInputs();
    saveDeviceState();
}

void MainComponent::rebuildTracks()
{
    auto& t = engine.tracks;
    if (std::find (t.begin(), t.end(), ctx.view.selectedTrack) == t.end())
        ctx.view.selectedTrack = t.empty() ? nullptr : t.front();
    if (ctx.view.selectedClip != nullptr && engine.findOwner (ctx.view.selectedClip) == nullptr)
        ctx.view.selectedClip = nullptr;

    arrange.rebuild();
    mixer.rebuild();
    transport.sync();
    updateTitle();
}

void MainComponent::syncControls()
{
    arrange.syncControls();
    mixer.syncControls();
    transport.sync();
}

void MainComponent::updateTitle()
{
    auto title = engine.getProjectName() + (engine.dirty ? " *" : "") + " - Tracklayer";
    if (title != lastTitle)
    {
        lastTitle = title;
        if (onTitleChanged) onTitleChanged (title);
    }
}

void MainComponent::showMessage (const String& title, const String& text)
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, title, text);
}

void MainComponent::saveDeviceState()
{
    if (auto xml = engine.deviceManager.createStateXml())
        properties.getUserSettings()->setValue ("audioDeviceState", xml.get());
    properties.saveIfNeeded();
}

void MainComponent::confirmDiscard (std::function<void()> then)
{
    if (! engine.dirty)
    {
        then();
        return;
    }

    NativeMessageBox::showYesNoCancelBox (MessageBoxIconType::QuestionIcon, "Unsaved changes",
                                          "Save changes to \"" + engine.getProjectName() + "\" first?",
                                          this,
                                          ModalCallbackFunction::create ([this, then] (int result)
                                          {
                                              if (result == 1)      saveProject (then);
                                              else if (result == 2) then();
                                          }));
}

void MainComponent::requestQuit()
{
    engine.stop();
    confirmDiscard ([] { JUCEApplication::quit(); });
}

//==============================================================================
// Commands
//==============================================================================
void MainComponent::runCommand (int cmd)
{
    auto& e = engine;
    auto& v = ctx.view;

    switch (cmd)
    {
        case cmdNewProject:
            confirmDiscard ([this]
            {
                pluginWindows.closeAll();
                keepAliveBriefly (engine.tracks);
                engine.newProject();
                if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
                {
                    const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
                    for (size_t i = 0; i < engine.tracks.size() && numIn > 0; ++i)
                        engine.tracks[i]->inputChannel = (int) i % numIn;
                }
                engine.dirty = false;
                ctx.view.selectedClip = nullptr;
                ctx.view.selectedTrack = nullptr;
                ctx.view.scrollX = 0.0;
                rebuildTracks();
            });
            break;

        case cmdOpen:         confirmDiscard ([this] { openProject(); }); break;
        case cmdSave:         saveProject(); break;
        case cmdSaveAs:       saveProjectAs(); break;
        case cmdImportAudio:  importAudio(); break;
        case cmdExportMix:    exportMix(); break;
        case cmdSettings:     showSettings(); break;
        case cmdPluginManager: showPluginManager(); break;
        case cmdQuit:         requestQuit(); break;

        case cmdDeleteClip:
            if (v.selectedClip != nullptr && ! e.isRecording())
            {
                e.deleteClip (v.selectedClip);
                v.selectedClip = nullptr;
            }
            break;

        case cmdSplitAtPlayhead:
        {
            const double at = e.getPosition();
            if (v.selectedClip != nullptr)
            {
                e.splitClip (v.selectedClip, at);
            }
            else if (v.selectedTrack != nullptr)
            {
                auto clips = v.selectedTrack->clips;   // copy: splitting adds clips
                for (auto& c : clips)
                    e.splitClip (c, at);
            }
            break;
        }

        case cmdNewTrack:
        {
            auto t = e.addTrack();
            v.selectedTrack = t;
            rebuildTracks();
            break;
        }

        case cmdNewInstrumentTrack:
        {
            auto t = e.addTrack ({}, true);
            v.selectedTrack = t;
            rebuildTracks();

            // Go straight to choosing a synth, like Logic does.
            auto instruments = e.getInstrumentPlugins();
            if (instruments.isEmpty())
            {
                showMessage ("Instrument track",
                             "No instrument plugins have been found yet.\n\n"
                             "Open File > Plugin Manager and scan for VST3 plugins, then click the "
                             "track's \"Choose instrument...\" box.");
            }
            else
            {
                PopupMenu m;
                KnownPluginList::addToMenu (m, instruments, KnownPluginList::sortByManufacturer);
                SafePointer<MainComponent> safe (this);
                m.showMenuAsync (PopupMenu::Options().withTargetComponent (&arrange)
                                     .withMousePosition(),
                                 [safe, t, instruments] (int result)
                {
                    if (safe == nullptr || result == 0) return;
                    const int idx = KnownPluginList::getIndexChosenByMenu (instruments, result);
                    if (idx >= 0)
                        safe->loadPluginInto (t, -1, instruments.getReference (idx));
                });
            }
            break;
        }

        case cmdDeleteTrack:
            if (v.selectedTrack != nullptr && ! e.isRecording())
            {
                pluginWindows.closeForTrack (*v.selectedTrack);
                keepAliveBriefly ({ v.selectedTrack });
                e.removeTrack (v.selectedTrack);
                v.selectedTrack = nullptr;
                v.selectedClip = nullptr;
                rebuildTracks();
            }
            break;

        case cmdPlayStop:
            e.togglePlay();
            break;

        case cmdRecord:
            e.toggleRecord();
            if (e.lastError.isNotEmpty())
            {
                showMessage ("Can't record", e.lastError);
                e.lastError.clear();
            }
            break;

        case cmdGoToStart:
            e.setPosition (0.0);
            break;

        case cmdBarBack:
        {
            const double bar = e.secondsPerBar(), pos = e.getPosition();
            const double idx = std::floor (pos / bar + 1e-6);
            e.setPosition (pos - idx * bar < 0.05 ? (idx - 1.0) * bar : idx * bar);
            break;
        }

        case cmdBarForward:
        {
            const double bar = e.secondsPerBar();
            e.setPosition ((std::floor (e.getPosition() / bar + 1e-6) + 1.0) * bar);
            break;
        }

        case cmdCycle:
            e.cycleOn = ! e.cycleOn.load();
            e.dirty = true;
            break;

        case cmdMetronome:
            e.metronomeOn = ! e.metronomeOn.load();
            break;

        case cmdToggleMixer:
            mixerVisible = ! mixerVisible;
            transport.setMixerShown (mixerVisible);
            resized();
            break;

        case cmdZoomIn:    arrange.zoom (1.5); break;
        case cmdZoomOut:   arrange.zoom (1.0 / 1.5); break;
        case cmdZoomToFit: arrange.zoomToFit(); break;

        default: break;
    }

    transport.sync();
    arrange.repaintTimeline();
    updateTitle();
}

bool MainComponent::keyPressed (const KeyPress& k)
{
    const auto ctrl  = ModifierKeys::commandModifier;
    const auto shift = ModifierKeys::shiftModifier;
    const auto alt   = ModifierKeys::altModifier;

    struct Binding { KeyPress key; int cmd; };
    const Binding bindings[] =
    {
        { KeyPress (KeyPress::spaceKey),               cmdPlayStop },
        { KeyPress ('r'),                              cmdRecord },
        { KeyPress ('c'),                              cmdCycle },
        { KeyPress ('k'),                              cmdMetronome },
        { KeyPress ('x'),                              cmdToggleMixer },
        { KeyPress ('z'),                              cmdZoomToFit },
        { KeyPress (KeyPress::returnKey),              cmdGoToStart },
        { KeyPress (KeyPress::homeKey),                cmdGoToStart },
        { KeyPress (','),                              cmdBarBack },
        { KeyPress ('.'),                              cmdBarForward },
        { KeyPress (KeyPress::deleteKey),              cmdDeleteClip },
        { KeyPress (KeyPress::backspaceKey),           cmdDeleteClip },
        { KeyPress (KeyPress::deleteKey, ctrl, 0),     cmdDeleteTrack },
        { KeyPress (KeyPress::backspaceKey, ctrl, 0),  cmdDeleteTrack },
        { KeyPress ('t', ctrl, 0),                     cmdSplitAtPlayhead },
        { KeyPress ('n', ctrl | alt, 0),               cmdNewTrack },
        { KeyPress ('s', ctrl | alt, 0),               cmdNewInstrumentTrack },
        { KeyPress ('n', ctrl, 0),                     cmdNewProject },
        { KeyPress ('o', ctrl, 0),                     cmdOpen },
        { KeyPress ('s', ctrl, 0),                     cmdSave },
        { KeyPress ('s', ctrl | shift, 0),             cmdSaveAs },
        { KeyPress ('i', ctrl, 0),                     cmdImportAudio },
        { KeyPress ('b', ctrl, 0),                     cmdExportMix },
        { KeyPress (',', ctrl, 0),                     cmdSettings },
        { KeyPress ('=', ctrl, 0),                     cmdZoomIn },
        { KeyPress ('-', ctrl, 0),                     cmdZoomOut },
        { KeyPress (KeyPress::rightKey, ctrl, 0),      cmdZoomIn },
        { KeyPress (KeyPress::leftKey, ctrl, 0),       cmdZoomOut },
    };

    for (auto& b : bindings)
    {
        if (k == b.key)
        {
            runCommand (b.cmd);
            return true;
        }
    }
    return false;
}

//==============================================================================
// Menus
//==============================================================================
StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "Transport", "View", "Help" };
}

static void addItem (PopupMenu& m, int id, const String& text, const String& shortcut,
                     bool enabled = true, bool ticked = false)
{
    PopupMenu::Item item (text);
    item.itemID = id;
    item.isEnabled = enabled;
    item.isTicked = ticked;
    item.shortcutKeyDescription = shortcut;
    m.addItem (item);
}

PopupMenu MainComponent::getMenuForIndex (int index, const String&)
{
    PopupMenu m;
    auto& e = engine;
    const bool rec = e.isRecording();

    switch (index)
    {
        case 0:
            addItem (m, cmdNewProject, "New Project", "Ctrl+N", ! rec);
            addItem (m, cmdOpen, "Open...", "Ctrl+O", ! rec);
            addItem (m, cmdSave, "Save", "Ctrl+S", ! rec);
            addItem (m, cmdSaveAs, "Save As...", "Ctrl+Shift+S", ! rec);
            m.addSeparator();
            addItem (m, cmdImportAudio, "Import Audio File...", "Ctrl+I", ! rec);
            addItem (m, cmdExportMix, "Bounce Mix to WAV...", "Ctrl+B", ! rec);
            m.addSeparator();
            addItem (m, cmdSettings, "Audio Settings...", "Ctrl+,");
            addItem (m, cmdPluginManager, "Plugin Manager...", "");
            m.addSeparator();
            addItem (m, cmdQuit, "Quit", "Alt+F4");
            break;

        case 1:
            addItem (m, cmdSplitAtPlayhead, "Split Region at Playhead", "Ctrl+T", ! rec);
            addItem (m, cmdDeleteClip, "Delete Region", "Delete", ctx.view.selectedClip != nullptr && ! rec);
            break;

        case 2:
            addItem (m, cmdNewTrack, "New Audio Track", "Ctrl+Alt+N");
            addItem (m, cmdNewInstrumentTrack, "New Instrument Track", "Ctrl+Alt+S");
            addItem (m, cmdDeleteTrack, "Delete Track", "Ctrl+Delete", ctx.view.selectedTrack != nullptr && ! rec);
            break;

        case 3:
            addItem (m, cmdPlayStop, e.isPlaying() ? "Stop" : "Play", "Space");
            addItem (m, cmdRecord, rec ? "Stop Recording" : "Record", "R");
            addItem (m, cmdGoToStart, "Go to Beginning", "Return");
            addItem (m, cmdBarBack, "Back One Bar", ",");
            addItem (m, cmdBarForward, "Forward One Bar", ".");
            m.addSeparator();
            addItem (m, cmdCycle, "Cycle", "C", true, e.cycleOn.load());
            addItem (m, cmdMetronome, "Metronome Click", "K", true, e.metronomeOn.load());
            break;

        case 4:
            addItem (m, cmdToggleMixer, "Mixer", "X", true, mixerVisible);
            m.addSeparator();
            addItem (m, cmdZoomIn, "Zoom In", "Ctrl+Right");
            addItem (m, cmdZoomOut, "Zoom Out", "Ctrl+Left");
            addItem (m, cmdZoomToFit, "Zoom to Fit", "Z");
            break;

        case 5:
            m.addItem ("Keyboard Shortcuts", [this]
            {
                showMessage ("Keyboard shortcuts",
                             "Space  Play / Stop\n"
                             "R  Record\n"
                             "C  Cycle on/off (or click the yellow strip in the ruler)\n"
                             "K  Metronome\n"
                             "Return  Go to beginning\n"
                             ",  .  Back / forward one bar\n"
                             "X  Mixer\n"
                             "Z  Zoom to fit,  Ctrl+mouse wheel to zoom\n"
                             "Ctrl+T  Split region at playhead\n"
                             "Delete  Delete region\n"
                             "Ctrl+Alt+N  New audio track\n"
                             "Ctrl+Alt+S  New instrument track\n"
                             "Ctrl+I  Import audio,  Ctrl+B  Bounce mix\n\n"
                             "Hold Alt while dragging to turn off snapping.\n"
                             "Drag in the top strip of the ruler to set a cycle area.\n"
                             "Drag audio files from Explorer onto the tracks.\n\n"
                             "Plugins: open the Mixer (X), click an empty Audio FX slot\n"
                             "to add an effect, click it again to open it, right-click\n"
                             "to bypass or remove it. Scan for plugins in\n"
                             "File > Plugin Manager.");
            });
            m.addItem ("About Tracklayer", [this]
            {
                showMessage ("About Tracklayer", "Tracklayer 0.2\nA small multitrack recorder built with JUCE.\n\nTracklayer Keys uses the Salamander Grand Piano samples by Alexander Holm,\nlicensed under CC-BY 3.0.\n\nVST is a trademark of Steinberg Media Technologies GmbH.");
            });
            break;

        default: break;
    }
    return m;
}

void MainComponent::menuItemSelected (int itemID, int)
{
    if (itemID > 0)
        runCommand (itemID);
}

//==============================================================================
// Files
//==============================================================================
static File defaultProjectsFolder()
{
    auto f = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Tracklayer Projects");
    f.createDirectory();
    return f;
}

void MainComponent::saveProject (std::function<void()> after)
{
    if (! engine.projectFile.existsAsFile())
    {
        saveProjectAs (after);
        return;
    }

    String err;
    if (! engine.saveProject (engine.projectFile, err))
        showMessage ("Save failed", err);
    else if (after)
        after();
    updateTitle();
}

void MainComponent::saveProjectAs (std::function<void()> after)
{
    auto start = engine.projectFile.existsAsFile() ? engine.projectFile
                                                   : defaultProjectsFolder().getChildFile ("My Song.tlproj");

    chooser = std::make_unique<FileChooser> ("Save project as", start, "*.tlproj");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::warnAboutOverwriting,
                          [this, after] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File())
            return;

        f = f.withFileExtension ("tlproj");
        const auto name = f.getFileNameWithoutExtension();
        if (f.getParentDirectory().getFileName() != name)   // give each project its own folder
            f = f.getParentDirectory().getChildFile (name).getChildFile (f.getFileName());

        String err;
        if (! engine.saveProject (f, err))
            showMessage ("Save failed", err);
        else if (after)
            after();
        updateTitle();
    });
}

void MainComponent::openProject()
{
    chooser = std::make_unique<FileChooser> ("Open project", defaultProjectsFolder(), "*.tlproj");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f.existsAsFile())
            loadProjectFile (f);
    });
}

void MainComponent::loadProjectFile (const File& f)
{
    pluginWindows.closeAll();
    keepAliveBriefly (engine.tracks);
    String err;
    const bool ok = engine.loadProject (f, err);
    ctx.view.selectedClip = nullptr;
    ctx.view.selectedTrack = nullptr;
    ctx.view.scrollX = 0.0;
    rebuildTracks();
    syncControls();
    if (ok)
        {
        SafePointer<MainComponent> sp (this);
        MessageManager::callAsync ([sp] { if (sp != nullptr) sp->arrange.zoomToFit(); });
    }
    if (err.isNotEmpty())
        showMessage (ok ? "Opened with problems" : "Open failed", err);
}

void MainComponent::importAudio()
{
    chooser = std::make_unique<FileChooser> ("Import audio", File::getSpecialLocation (File::userMusicDirectory),
                                             engine.formatManager.getWildcardForAllFormats());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::canSelectMultipleItems,
                          [this] (const FileChooser& fc)
    {
        auto files = fc.getResults();
        if (files.isEmpty())
            return;

        auto target = ctx.view.selectedTrack;
        const double start = jmax (0.0, ctx.snap (engine.getPosition(), false));
        StringArray errors;

        for (int i = 0; i < files.size(); ++i)
        {
            if (target == nullptr || i > 0)
                target = engine.addTrack (files[i].getFileNameWithoutExtension());

            String err;
            if (! engine.importFile (files[i], target, start, err))
                errors.add (err);
        }

        ctx.view.selectedTrack = target;
        rebuildTracks();
        if (errors.size() > 0)
            showMessage ("Import", errors.joinIntoString ("\n"));
    });
}

void MainComponent::exportMix()
{
    auto folder = engine.projectFile.existsAsFile() ? engine.projectFolder : defaultProjectsFolder();
    chooser = std::make_unique<FileChooser> ("Bounce mix to WAV", folder.getChildFile (engine.getProjectName() + ".wav"), "*.wav");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File())
            return;

        f = f.withFileExtension ("wav");
        String err;
        if (engine.exportMix (f, err))
            showMessage ("Bounce complete", "Saved the mix to:\n" + f.getFullPathName());
        else
            showMessage ("Bounce failed", err);
        transport.sync();
    });
}

void MainComponent::showSettings()
{
    auto* selector = new AudioDeviceSelectorComponent (engine.deviceManager, 0, 32, 0, 2, true, false, true, false);
    selector->setSize (560, 560);

    DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = Theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.launchAsync();
}

//==============================================================================
// Plugins
//==============================================================================
void MainComponent::showPluginManager()
{
    if (pluginManager == nullptr)
    {
        SafePointer<MainComponent> safe (this);
        pluginManager = std::make_unique<PluginManagerWindow> (engine, properties.getUserSettings(), [safe]
        {
            MessageManager::callAsync ([safe] { if (safe != nullptr) safe->pluginManager.reset(); });
        });
    }
    pluginManager->toFront (true);
}

void MainComponent::loadPluginInto (const TrackPtr& track, int slot, const PluginDescription& desc)
{
    SafePointer<MainComponent> safe (this);
    std::weak_ptr<Track> weakTrack = track;

    engine.pluginFormats.createPluginInstanceAsync (desc, engine.getSampleRate(), Engine::pluginMaxBlock,
        [safe, weakTrack, slot, desc] (std::unique_ptr<AudioPluginInstance> plugin, const String& error)
        {
            auto t = weakTrack.lock();
            if (safe == nullptr || t == nullptr)
                return;

            auto& self = *safe;
            if (plugin == nullptr)
            {
                self.showMessage ("Plugin", "Couldn't load " + desc.name + (error.isNotEmpty() ? ":\n" + error : String()));
                return;
            }

            self.pluginWindows.closeFor (t->pluginAt (slot));
            self.disposeLater (self.engine.setInsert (*t, slot, std::move (plugin), desc));
            self.syncControls();
            self.updateTitle();
            self.pluginWindows.open (t, slot);
        });
}

void MainComponent::disposeLater (std::unique_ptr<AudioPluginInstance> plugin)
{
    if (plugin == nullptr)
        return;

    // Let the plugin's just-closed editor finish tearing down first.
    auto shared = std::shared_ptr<AudioPluginInstance> (std::move (plugin));
    Timer::callAfterDelay (300, [shared] {});
}

void MainComponent::keepAliveBriefly (std::vector<TrackPtr> oldTracks)
{
    // Tracks own their plugins; hold them a moment so editors finish closing first.
    Timer::callAfterDelay (300, [oldTracks] {});
}
