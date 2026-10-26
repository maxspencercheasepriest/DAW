#pragma once

#include "TransportBar.h"
#include "MixerView.h"
#include "PluginWindows.h"

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::KeyListener,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (juce::ApplicationProperties& props);
    ~MainComponent() override;

    std::function<void (const juce::String&)> onTitleChanged;

    void requestQuit();
    void loadProjectFile (const juce::File&);
    void runCommand (int cmd);

    // Component
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override { return keyPressed (k); }

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int itemID, int) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuildTracks();
    void syncControls();
    void updateTitle();
    void showMessage (const juce::String& title, const juce::String& text);
    void confirmDiscard (std::function<void()> then);

    void openProject();
    void saveProject (std::function<void()> after = {});
    void saveProjectAs (std::function<void()> after = {});
    void importAudio();
    void exportMix();
    void showSettings();
    void saveDeviceState();
    void showPluginManager();
    void loadPluginInto (const TrackPtr&, int slot, const juce::PluginDescription&);
    void disposeLater (std::unique_ptr<juce::AudioPluginInstance>);
    void keepAliveBriefly (std::vector<TrackPtr>);

    juce::ApplicationProperties& properties;
    Theme::LookAndFeel lookAndFeel;
    Engine engine;
    AppContext ctx { engine };

    juce::MenuBarComponent menuBar { this };
    TransportBar transport { ctx };
    ArrangeView arrange { ctx };
    MixerView mixer { ctx };
    juce::TooltipWindow tooltips { this, 600 };

    bool mixerVisible = false;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastTitle;
    int meterDivider = 0;
    bool rebuildPending = false;

    // declared last so they're destroyed before the plugins they show
    PluginWindowManager pluginWindows { ctx };
    std::unique_ptr<PluginManagerWindow> pluginManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
