# Rotating HRTF v2 Audio Plugin (CLAP + VST3 for Ableton Live)

A 3D binaural spatialiser plugin built around an acoustically verified HRTF DSP core. It simulates head-related transfer functions including Interaural Time Differences (ITD), spherical head-shadow attenuation (ILD), distance attenuation, and pinna spectral cues (biquad peaking and shelf filters).

---

## Formats Available

1. **VST3 (`RotatingHRTF_v2.vst3`)**
   - **Plugin Name:** `Rotating HRTF v2`
   - **Fully compatible with Ableton Live (Live 10, 11, 12)** and all VST3 DAWs.
   - Deployed directly to `C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3`.
   - Accepts both **Mono and Stereo tracks** (automatically downmixes stereo input to mono for spatialisation), and outputs full 2-channel binaural stereo.
   - Full parameter automation support for **Distance** and **Rotation**.
   - Preset and project state save/recall (`getState` / `setState`).

2. **CLAP (`RotatingHRTF_v2.clap`)**
   - Compatible with CLAP hosts such as Bitwig Studio, Reaper, FL Studio, etc.

---

## Quick Start: Using in Ableton Live

### Automatic Installation
Run [install_ableton.bat](file:///g:/dev/hrtf/install_ableton.bat). It elevates itself through UAC if needed and installs the bundle into:
```
C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3
```
It verifies the copy before reporting success and exits with an error if anything failed. Build first — the script refuses to run without `RotatingHRTF_v2.vst3` in the project root.

### Manual Installation
Create the bundle folder and copy the built binary into it:
```
mkdir "C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win"
copy RotatingHRTF_v2.vst3 "C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\"
```

### In Ableton Live:
1. Go to **Preferences -> Plug-Ins**.
2. Ensure **"Use VST3 Plug-In System Folders"** is turned **ON**.
3. Click **"Rescan Plug-ins"** (or hold `Alt` while clicking for a full deep rescan).
4. In Ableton's Browser under **Plug-Ins -> VST3 -> Example Audio**, drag **Rotating HRTF v2** onto an audio track (it is an audio effect, not an instrument).

---

## Parameters

| Parameter | Range | Default | Unit | Description |
|---|---|---|---|---|
| **Distance** | 0.05 – 20.0 | 2.0 | metres (`m`) | Simulates source distance with inverse-distance law attenuation, near-field low-frequency ILD divergence, and proximity cue. |
| **Rotation** | 0.0 – 360.0 | 0.0 | degrees (`deg`) | Horizontal azimuth angle. `0°` = Front, `90°` = Right ear, `180°` = Rear, `270°` = Left ear. |
| **Elevation** | -90.0 – +90.0 | 0.0 | degrees (`deg`) | Vertical angle. `-90°` = Below, `0°` = Horizontal plane, `+90°` = Directly overhead. Dynamically modulates pinna concha notches. |
| **Space** | 0.0 – 100.0 | 15.0 | percent (`%`) | Room boundary early reflection externalisation engine. Pulls audio outside the skull into a natural acoustic room. |
| **Test Pulse** | Off / On (0 / 1) | Off | toggle | Built-in test pulse generator. Synthesises psychoacoustically calibrated 200 ms pink noise bursts on each beat (tempo-synchronised to the host). |

All parameters feature smooth exponential slewing (20–50 ms) inside [hrtf_core.c](file:///g:/dev/hrtf/hrtf_core.c) to prevent clicking or zipper noise during automation.

---

## Calibration & Test Signals

For critical acoustic verification, the repository includes psychoacoustically calibrated test signals:
- `pulsed_pink_noise_48k.wav`: 48 kHz, 24-bit PCM mono loopable WAV (8 beats at 120 BPM). Each beat fires a 200 ms burst of $1/f$ pink noise with 5 ms Hann onsets and decays.
- `tools/generate_test_pulse.py`: Standalone Python script to regenerate test WAV signals with custom duration, tempo, or sample rates.
- **Built-in Test Pulse Tick Box**: Available directly inside the VST3 and CLAP plugin as a dedicated toggle parameter. When enabled, it replaces the track input with the beat-synchronised pink noise pulse, allowing direct 3D positioning adjustments in Ableton Live without requiring external audio tracks.

---

## Building from Source

### Getting the source

The VST3 SDK (`public.sdk`, `base`, `pluginterfaces`), the CLAP headers (`clap-src`) and `clap-wrapper` are git submodules. Clone recursively so the tree is buildable straight away:

```cmd
git clone --recurse-submodules <repository-url>
```

Already have a clone without them? `git submodule update --init --recursive` populates them in place — no junctions, symlinks or manual copying required.

### Prerequisites

- Visual Studio 2022 (with MSVC C/C++ x64 tools)

To build both the CLAP plugin, VST3 plugin, and execute the automated verification test suite:

```cmd
build_all.bat
```

This compiles:
- `RotatingHRTF_v2.clap`
- `RotatingHRTF_v2.vst3` (and populates `bundle\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3`)
- Runs `test_clap.exe` (CLAP loading, initialisation, processing, output audio energy)
- Runs `test_vst3.exe` (VST3 loading, COM factory, parameter enumeration, audio processing, rotation automation)

Both test hosts exit non-zero when a check fails, and `build_all.bat` stops with an error in that case, so a "completed successfully" banner means the checks really passed. The script also tries to deploy the bundle to the system VST3 folder; that step needs Administrator rights and it reports plainly when it could not write there.

---

## Project Structure

- [hrtf_core.h](file:///g:/dev/hrtf/hrtf_core.h) / [hrtf_core.c](file:///g:/dev/hrtf/hrtf_core.c): Core HRTF DSP engine (biquad filters, fractional ITD delay buffer, head shadow).
- [hrtf_clap.c](file:///g:/dev/hrtf/hrtf_clap.c): CLAP plugin implementation.
- [vst3/](file:///g:/dev/hrtf/vst3): Native VST3 plugin implementation:
  - [vst3/plugprocessor.h](file:///g:/dev/hrtf/vst3/plugprocessor.h) / [vst3/plugprocessor.cpp](file:///g:/dev/hrtf/vst3/plugprocessor.cpp): Audio effect processor with stereo downmixing, HRTF processing, and automation.
  - [vst3/plugcontroller.h](file:///g:/dev/hrtf/vst3/plugcontroller.h) / [vst3/plugcontroller.cpp](file:///g:/dev/hrtf/vst3/plugcontroller.cpp): Parameter controller (Distance, Rotation) and state serialisation.
  - [vst3/plugfactory.cpp](file:///g:/dev/hrtf/vst3/plugfactory.cpp): Steinberg VST3 class factory exports.
  - [vst3/plugids.h](file:///g:/dev/hrtf/vst3/plugids.h): Unique GUIDs for processor and controller.
  - [vst3/version.h](file:///g:/dev/hrtf/vst3/version.h): Plugin version metadata.
- [test_clap.c](file:///g:/dev/hrtf/test_clap.c): Automated unit test host for the CLAP plugin.
- [test_vst3.cpp](file:///g:/dev/hrtf/test_vst3.cpp): Automated unit test host for the VST3 plugin with automation verification.
- [build_all.bat](file:///g:/dev/hrtf/build_all.bat): 1-click build and test script.
- [install_ableton.bat](file:///g:/dev/hrtf/install_ableton.bat): Installer into Windows standard VST3 directory for Ableton Live.
