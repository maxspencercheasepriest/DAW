#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

// Tracklayer's own instruments and effects. They appear in the plugin menus under
// "Tracklayer" and behave exactly like VST3/AU plugins (slots, windows, saving).
namespace BuiltIn
{
    // Every built-in plugin, as descriptions the plugin menus can list.
    juce::Array<juce::PluginDescription> getTypes();

    // A plugin "format" that creates the built-ins, so the rest of the app needs
    // no special cases: projects save and reload them like any other plugin.
    class Format : public juce::AudioPluginFormat
    {
    public:
        juce::String getName() const override { return "Built-in"; }
        void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override;
        bool fileMightContainThisPluginType (const juce::String& id) override { return id.startsWith ("tracklayer:"); }
        juce::String getNameOfPluginFromIdentifier (const juce::String& id) override;
        bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
        bool doesPluginStillExist (const juce::PluginDescription& d) override { return fileMightContainThisPluginType (d.fileOrIdentifier); }
        bool canScanForPlugins() const override { return false; }
        bool isTrivialToScan() const override { return true; }
        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
        juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
        void createPluginInstance (const juce::PluginDescription&, double initialSampleRate,
                                   int initialBufferSize, PluginCreationCallback) override;
    };
}
