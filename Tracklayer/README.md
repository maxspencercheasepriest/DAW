# Tracklayer

A small Logic-style multitrack recorder for Windows, written in C++ with [JUCE](https://juce.com).

![Tracklayer](docs/screenshot.png)

## What it does (v0.1)

- **Arrange window**: tracks with coloured regions and waveforms, bar ruler, beat grid, snapping, zoom.
- **Recording**: arm any number of tracks (R), pick a mono or stereo input per track, and press R to record. The app compensates for latency automatically, and recordings are saved as 24-bit WAVs in the project's `Audio` folder.
- **Input monitoring** (I) so you can hear yourself through the track.
- **Editing**: drag regions (between tracks too), trim either edge, split at the playhead (Ctrl+T), delete. Hold Alt to drag without snapping.
- **Cycle mode**: drag in the yellow strip at the top of the ruler and click it to toggle, like Logic.
- **Metronome**, tempo (drag or double-click the LCD), and a bars/beats/div/tick display.
- **Mixer** (X): faders, pan, mute/solo, peak meters, Stereo Out.
- **Import** WAV, AIFF, FLAC, Ogg and MP3 files by drag-and-drop from Explorer or with Ctrl+I.
- **Bounce** the mix to a 24-bit WAV with Ctrl+B.
- **Save and open** `.tlproj` projects, which are plain JSON and point at the audio files.

## Building it

### Option A: let GitHub build it (no software to install)

1. Create a new GitHub repository and upload this folder to it.
2. Open the **Actions** tab. The "Build Windows" workflow runs automatically, or you can start it with **Run workflow**.
3. When it finishes (about 10 minutes), download **Tracklayer-Windows** from the run's Artifacts and unzip `Tracklayer.exe`.

### Option B: build it on your PC

1. Install [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/) with the **Desktop development with C++** workload.
2. Open the **Developer Command Prompt for VS 2022**, `cd` to this folder, and run `build-windows.bat`.
   The first build downloads JUCE and takes a while.
3. `Tracklayer.exe` appears in this folder.

To work on the code, open the folder in Visual Studio (File → Open → Folder). It picks up the CMake project automatically.

## Try it

Open `Demo/Demo.tlproj` (File → Open, or drag it onto `Tracklayer.exe`) for a short drums, bass and guitar sketch.

## Audio setup

Open **Settings** (Ctrl+,) and pick your interface. Use **Windows Audio (Exclusive Mode)** or **Low Latency Mode** for the lowest latency. Enable the input channels you want to record from.

For **ASIO**: download the Steinberg ASIO SDK, then build with `build-windows.bat -DASIO_SDK_DIR=C:\path\to\asiosdk`.

## Keyboard shortcuts (Logic-style)

| Key | Action |
|---|---|
| Space | Play / Stop |
| R | Record |
| C | Cycle on/off |
| K | Metronome |
| Return | Go to beginning |
| , / . | Back / forward one bar |
| X | Mixer |
| Z | Zoom to fit (Ctrl+wheel or Ctrl+←/→ to zoom) |
| Ctrl+T | Split region at playhead |
| Delete | Delete region |
| Ctrl+Alt+N | New track |
| Ctrl+I | Import audio |
| Ctrl+B | Bounce mix |
| Ctrl+S / Ctrl+O / Ctrl+N | Save / Open / New |

## Command line

```
Tracklayer.exe "My Song.tlproj"                        open a project
Tracklayer.exe --bounce "My Song.tlproj" "Mix.wav"     render without opening the window
```

## Roadmap

1. Undo/redo
2. MIDI tracks: piano roll, MIDI input, a built-in synth
3. VST3 plugin hosting (inserts on every channel)
4. Sends, buses and automation
5. Importing Logic projects: stems and MIDI first, then AAF, then reading `.logicx` directly

## Licence note

JUCE is free for personal use under its personal licence or AGPLv3. Check the JUCE licence before distributing builds commercially.
