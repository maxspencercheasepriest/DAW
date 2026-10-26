#pragma once

#include "Model.h"

// The Session Drummer: turns DrummerSettings into General MIDI drum notes that
// fit the project's tempo and time signature. Deterministic: the same settings
// (including the variation seed) always give the same groove.
namespace Drummer
{
    // General MIDI drum map (works with nearly every drum plugin)
    enum Note
    {
        kick = 36, sideStick = 37, snare = 38, clap = 39,
        floorTomLow = 41, closedHat = 42, floorTomHigh = 43, pedalHat = 44,
        tomLow = 45, openHat = 46, tomLowMid = 47, tomHighMid = 48,
        crash = 49, tomHigh = 50, ride = 51, rideBell = 53
    };

    juce::StringArray styleNames();
    juce::StringArray cymbalNames();

    // Notes for a whole region, times in seconds from the region start.
    std::vector<MidiNote> generate (const DrummerSettings&, double tempoBpm, int numerator, int denominator);

    // Bar groupings in beat units, e.g. 7/8 -> {2,2,3}
    std::vector<int> groupsFor (int numerator, int denominator);

    juce::String noteName (int note);
}
