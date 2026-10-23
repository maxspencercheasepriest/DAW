#pragma once

#include "AppContext.h"

// A floating window showing one plugin's own interface (or a generic one).
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& p, const juce::String& trackName, std::function<void (PluginWindow*)> onClose)
        : DocumentWindow (trackName + " - " + p.getName(), Theme::panel, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          plugin (p), closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginWindow() override
    {
        clearContentComponent();   // editor must go before the plugin does
    }

    juce::AudioPluginInstance& plugin;

    void closeButtonPressed() override
    {
        if (closeCallback)
            closeCallback (this);
    }

private:
    std::function<void (PluginWindow*)> closeCallback;
};

//==============================================================================
// Keeps track of open plugin windows, and the plugin manager window.
class PluginWindowManager
{
public:
    explicit PluginWindowManager (AppContext& c) : ctx (c) {}

    void open (const TrackPtr& t, int slot)
    {
        auto* p = t->inserts[(size_t) slot].get();
        if (p == nullptr)
            return;

        for (auto* w : windows)
        {
            if (&w->plugin == p)
            {
                w->toFront (true);
                return;
            }
        }

        auto* self = this;
        windows.add (new PluginWindow (*p, t->name, [self] (PluginWindow* w)
        {
            juce::MessageManager::callAsync ([self, w] { self->windows.removeObject (w); });
        }));
    }

    void closeFor (juce::AudioProcessor* p)
    {
        for (int i = windows.size(); --i >= 0;)
            if (&windows[i]->plugin == p)
                windows.remove (i);
    }

    void closeForTrack (const Track& t)
    {
        for (auto& p : t.inserts)
            if (p != nullptr)
                closeFor (p.get());
    }

    void closeAll() { windows.clear(); }

private:
    AppContext& ctx;
    juce::OwnedArray<PluginWindow> windows;
};

//==============================================================================
class PluginManagerWindow : public juce::DocumentWindow
{
public:
    PluginManagerWindow (Engine& e, juce::PropertiesFile* props, std::function<void()> onClose)
        : DocumentWindow ("Plugin Manager", Theme::panel, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        auto deadMansPedal = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getChildFile ("Tracklayer").getChildFile ("ScanInProgress.txt");
        deadMansPedal.getParentDirectory().createDirectory();   // must exist for crash protection to work
        auto* list = new juce::PluginListComponent (e.pluginFormats, e.knownPlugins, deadMansPedal, props, false);
        list->setOptionsButtonText ("Scan / Options...");
        list->setSize (720, 480);

        setUsingNativeTitleBar (true);
        setContentOwned (list, true);
        setResizable (true, false);
        centreWithSize (720, 480);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (closeCallback)
            closeCallback();
    }

private:
    std::function<void()> closeCallback;
};
