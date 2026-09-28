# Rotating HRTF v2 Audio Plugin (CLAP + VST3 for Ableton Live)

A 3D binaural spatializer plugin built around an acoustically verified HRTF DSP core. It simulates head-related transfer functions including Interaural Time Differences (ITD), spherical head-shadow attenuation (ILD), distance attenuation, and pinna spectral cues (biquad peaking and shelf filters).

---

## Formats Available

1. **VST3 (`RotatingHRTF_v2.vst3`)**
   - **Plugin Name:** `Rotating HRTF v2`
   - **Fully compatible with Ableton Live (Live 10, 11, 12)** and all VST3 DAWs.
   - Deployed directly to `C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3`.
   - Accepts both **Mono and Stereo tracks** (automatically downmixes stereo input to mono for spatialization), and outputs full 2-channel binaural stereo.
   - Full parameter automation support for **Distance** and **Rotation**.
   - Preset and project state save/recall (`getState` / `setState`).

2. **CLAP (`RotatingHRTF_v2.clap`)**
   - Compatible with CLAP hosts such as Bitwig Studio, Reaper, FL Studio, etc.

---

## Quick Start: Using in Ableton Live

### Automatic Installation
Run [install_ableton.bat](file:///g:/dev/hrtf/install_ableton.bat) as Administrator. It will copy the VST3 bundle directly into:
```
C:\Program Files\Common Files\VST3\RotatingHRTF.vst3\Contents\x86_64-win\RotatingHRTF.vst3
```

### Manual Installation
Simply copy [RotatingHRTF.vst3](file:///g:/dev/hrtf/RotatingHRTF.vst3) into:
```
C:\Program Files\Common Files\VST3\
```

### In Ableton Live:
1. Go to **Preferences -> Plug-Ins**.
2. Ensure **"Use VST3 Plug-In System Folders"** is turned **ON**.
3. Click **"Rescan Plug-ins"** (or hold `Alt` while clicking for a full deep rescan).
4. In Ableton's Browser under **Plug-Ins -> VST3 -> Example Audio**, drag **Rotating HRTF** onto any audio track or MIDI instrument track.

---

## Parameters

| Parameter | Range | Default | Unit | Description |
|---|---|---|---|---|
| **Distance** | 0.05 – 20.0 | 2.0 | meters (`m`) | Simulates source distance with inverse-distance law attenuation and near-field spectral compensation. |
| **Rotation** | 0.0 – 360.0 | 0.0 | degrees (`deg`) | Horizontal azimuth angle. `0°` = Front, `90°` = Right ear, `180°` = Rear, `270°` = Left ear. |

Both parameters feature smooth exponential slewing (20 ms for rotation, 50 ms for distance) inside [hrtf_core.c](file:///g:/dev/hrtf/hrtf_core.c) to prevent clicking or zipper noise during automation.

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
- `RotatingHRTF.clap`
- `RotatingHRTF.vst3` (and populates `bundle\RotatingHRTF.vst3\Contents\x86_64-win\RotatingHRTF.vst3`)
- Runs `test_clap.exe` (tests CLAP loading, initialization, processing, and output audio energy)
- Runs `test_vst3.exe` (tests VST3 loading, COM factory, parameter enumeration, realtime audio processing, and parameter automation)

---

## Project Structure

- [hrtf_core.h](file:///g:/dev/hrtf/hrtf_core.h) / [hrtf_core.c](file:///g:/dev/hrtf/hrtf_core.c): Core HRTF DSP engine (biquad filters, fractional ITD delay buffer, head shadow).
- [hrtf_clap.c](file:///g:/dev/hrtf/hrtf_clap.c): CLAP plugin implementation.
- [vst3/](file:///g:/dev/hrtf/vst3): Native VST3 plugin implementation:
  - [vst3/plugprocessor.h](file:///g:/dev/hrtf/vst3/plugprocessor.h) / [vst3/plugprocessor.cpp](file:///g:/dev/hrtf/vst3/plugprocessor.cpp): Audio effect processor with stereo downmixing, HRTF processing, and automation.
  - [vst3/plugcontroller.h](file:///g:/dev/hrtf/vst3/plugcontroller.h) / [vst3/plugcontroller.cpp](file:///g:/dev/hrtf/vst3/plugcontroller.cpp): Parameter controller (Distance, Rotation) and state serialization.
  - [vst3/plugfactory.cpp](file:///g:/dev/hrtf/vst3/plugfactory.cpp): Steinberg VST3 class factory exports.
  - [vst3/plugids.h](file:///g:/dev/hrtf/vst3/plugids.h): Unique GUIDs for processor and controller.
  - [vst3/version.h](file:///g:/dev/hrtf/vst3/version.h): Plugin version metadata.
- [test_clap.c](file:///g:/dev/hrtf/test_clap.c): Automated unit test host for the CLAP plugin.
- [test_vst3.cpp](file:///g:/dev/hrtf/test_vst3.cpp): Automated unit test host for the VST3 plugin with automation verification.
- [build_all.bat](file:///g:/dev/hrtf/build_all.bat): 1-click build and test script.
- [install_ableton.bat](file:///g:/dev/hrtf/install_ableton.bat): Installer into Windows standard VST3 directory for Ableton Live.
