#include "MainComponent.h"

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::ApplicationProperties& props)
        : DocumentWindow ("Tracklayer", Theme::bg, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        main = new MainComponent (props);
        setContentOwned (main, true);
        setResizable (true, true);
        setResizeLimits (960, 600, 10000, 10000);
        centreWithSize (juce::jmin (1440, getParentWidth() - 40), juce::jmin (880, getParentHeight() - 60));
        main->onTitleChanged = [this] (const juce::String& t) { setName (t); };
        addKeyListener (main);
        setVisible (true);
        main->grabKeyboardFocus();
    }

    void closeButtonPressed() override { main->requestQuit(); }
    MainComponent& getMain() { return *main; }

private:
    MainComponent* main = nullptr;   // owned by the window
};

class TracklayerApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Tracklayer"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Headless bounce:  Tracklayer --bounce "Song.tlproj" "Mix.wav"
        auto args = juce::StringArray::fromTokens (commandLine, true);
        if (args.size() == 3 && args[0] == "--bounce")
        {
            auto cwd = juce::File::getCurrentWorkingDirectory();
            Engine engine;
            juce::String err;
            const bool ok = engine.loadProject (cwd.getChildFile (args[1].unquoted()), err)
                         && engine.exportMix (cwd.getChildFile (args[2].unquoted()), err);
            std::cout << (ok ? "Bounced OK" : "Bounce failed") << (err.isNotEmpty() ? ": " + err : juce::String()) << std::endl;
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        juce::PropertiesFile::Options o;
        o.applicationName = "Tracklayer";
        o.filenameSuffix = ".settings";
        o.folderName = "Tracklayer";
        o.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (o);

        window = std::make_unique<MainWindow> (properties);

        // Opening a .tlproj by double-click / drag onto the exe passes it here.
        auto arg = juce::File::getCurrentWorkingDirectory().getChildFile (getCommandLineParameters().unquoted());
        if (arg.hasFileExtension ("tlproj") && arg.existsAsFile())
            window->getMain().loadProjectFile (arg);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        auto arg = juce::File::getCurrentWorkingDirectory().getChildFile (commandLine.unquoted());
        if (window != nullptr && arg.hasFileExtension ("tlproj") && arg.existsAsFile())
            window->getMain().loadProjectFile (arg);
    }

    void shutdown() override
    {
        window = nullptr;
        properties.closeFiles();
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr) window->closeButtonPressed();
        else                   quit();
    }

private:
    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (TracklayerApp)
