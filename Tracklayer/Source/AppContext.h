#pragma once

#include "Engine.h"

enum CommandIDs
{
    cmdNewProject = 1,
    cmdOpen,
    cmdSave,
    cmdSaveAs,
    cmdImportAudio,
    cmdExportMix,
    cmdSettings,
    cmdQuit,

    cmdDeleteClip = 100,
    cmdSplitAtPlayhead,

    cmdNewTrack = 200,
    cmdDeleteTrack,

    cmdPlayStop = 300,
    cmdRecord,
    cmdGoToStart,
    cmdBarBack,
    cmdBarForward,
    cmdCycle,
    cmdMetronome,

    cmdToggleMixer = 400,
    cmdZoomIn,
    cmdZoomOut,
    cmdZoomToFit
};

// What the arrange window is looking at.
struct ArrangeState
{
    double pxPerSec = 40.0;
    double scrollX = 0.0;    // seconds at the left edge of the lanes
    int trackHeight = 84;
    int headerWidth = 240;

    ClipPtr selectedClip;
    TrackPtr selectedTrack;

    float secToX (double s) const { return (float) ((s - scrollX) * pxPerSec); }
    double xToSec (double x) const { return x / pxPerSec + scrollX; }
};

// Shared between all UI components.
struct AppContext
{
    explicit AppContext (Engine& e) : engine (e) {}

    Engine& engine;
    ArrangeState view;

    std::function<void()> tracksChanged;      // track list/names changed: rebuild (deferred)
    std::function<void()> mixChanged;         // a control changed: sync all controls
    std::function<void()> selectionChanged;
    std::function<void (int)> runCommand;
    std::function<void (const juce::String&, const juce::String&)> showMessage;

    double snap (double seconds, bool bypass) const
    {
        if (bypass)
            return seconds;
        double grid = engine.secondsPerBeat();
        if (grid * view.pxPerSec < 10.0)
            grid = engine.secondsPerBar();
        return std::round (seconds / grid) * grid;
    }

    void selectTrack (const TrackPtr& t)
    {
        view.selectedTrack = t;
        if (selectionChanged) selectionChanged();
    }
};
