# Rotating HRTF v2 Audio Plugin (CLAP + VST3 for Ableton Live)

A high-performance, real-time 3D binaural spatialiser audio plugin built upon an acoustically calibrated Head-Related Transfer Function (HRTF) DSP core. It simulates spherical head acoustic shadow, interaural time delays, distance attenuation, near-field curvature divergence, anthropometric pinna and skull scaling, and room boundary reflections for headphones and stereo monitoring.

Developed in pure C (core DSP engine, CLAP plugin, standalone CLI generator, and dogfooding test harness) with a minimal C++17 VST3 wrapper strictly for Ableton Live compatibility. Fully self-contained native C and C++ architecture built using standard MSVC tools.

---

## Table of Contents

1. [Features & Capabilities](#features--capabilities)
2. [Acoustic & DSP Foundations](#acoustic--dsp-foundations)
3. [Plugin Formats](#plugin-formats)
4. [Parameters Reference](#parameters-reference)
5. [Ableton Live Integration Guide](#ableton-live-integration-guide)
6. [Test Signal Generator & Pure C CLI](#test-signal-generator--pure-c-cli)
7. [Automated Verification & Test Harness](#automated-verification--test-harness)
8. [Building from Source](#building-from-source)
9. [Project Architecture](#project-architecture)
10. [Engineering Standards](#engineering-standards)

---

## Features & Capabilities

- **Binaural 3D Positioning**: Full $360^\circ$ horizontal azimuth rotation and $-90^\circ$ to $+90^\circ$ vertical elevation.
- **Physical Inverse-Distance Law & Near-Field Intimacy**: Follows $1/d$ free-field distance attenuation referenced to $1\text{ m}$, with smooth near-field proximity gain (up to $+3\text{ dB}$ at $5\text{ cm}$) providing natural auditory intimacy close to the ear.
- **Distance Variation Function (DVF)**: Near-field wavefront curvature produces up to $+9.5\text{ dB}$ of low-frequency Interaural Level Difference (ILD) divergence at $5\text{ cm}$.
- **Woodworth Spherical Ray-Tracing ITD**: Physical spherical head acoustic ray-tracing model with 4-point Hermite cubic fractional delay interpolation, preserving flat frequency response across all angles.
- **Frequency-Dependent Cranial Head Shadow**: Differentiates low-frequency spherical diffraction ($-4\text{ dB}$ LF shadow) from deep high-frequency shadowing (up to $-13\text{ dB}$ HF shadow).
- **3D Directional Room Boundaries & Distance-Dependent DRR**: Early reflections dynamically modulated by source direction cosines $(s, c, v)$ and decoupled from direct sound to establish an authentic Direct-to-Reverberant Ratio gradient with distance.
- **Anthropometric Head & Pinna Scaling**: Adjust head circumference and ear dimensions from $70\%$ to $130\%$, scaling ITD time delays and shifting spectral pinna notch frequencies accordingly.
- **Integrated Psychoacoustic Test Pulse Generator**: Built-in tick-box generator synthesising tempo-aligned pulses (pink noise, sine, or Dirac clicks) across a morphable low-rumble to crisp-transient timbre continuum.
- **Real-Time Thread Safety**: Strictly zero memory allocations in the audio processing thread, lock-free parameter updates, and defensive clamping against invalid input.
- **Sample-Accurate Automation**: VST3 parameter changes take effect at their exact sample offsets within the block.
- **Backwards-Compatible State Preservation**: Versioned state serialisation (Version 4) seamlessly reading presets created in Versions 1, 2, and 3.

---

## Acoustic & DSP Foundations

```
                           +----------------------+
                           |   Mono Source Input  |
                           +----------+-----------+
                                      |
                +---------------------+---------------------+
                |                                           |
                v                                           v
       +-----------------+                         +-----------------+
       | Left Delay Line |                         | Right Delay Line|
       | (4-Point Hermite|                         | (4-Point Hermite|
       |   Cubic ITD)    |                         |   Cubic ITD)    |
       +--------+--------+                         +--------+--------+
                |                                           |
                v                                           v
       +-----------------+                         +-----------------+
       | 5-Stage Biquad  |                         | 5-Stage Biquad  |
       | Filter Cascade: |                         | Filter Cascade: |
       | - Presence Peak |                         | - Presence Peak |
       | - Concha Notch  |                         | - Concha Notch  |
       | - Air High-Shelf|                         | - Air High-Shelf|
       | - Side Peaking  |                         | - Side Peaking  |
       | - Near-Field DVF|                         | - Near-Field DVF|
       +--------+--------+                         +--------+--------+
                |                                           |
                v                                           v
         [ILD Head Shadow]                           [ILD Head Shadow]
                |                                           |
                +---------------------+---------------------+
                                      |
                                      +<----+ [3D Directional Early Reflections]
                                      |       (Direction-Cosines & Diffuse DRR)
                                      v
                             [Soft-Knee Limiter]
                                      |
                                      v
                           +----------------------+
                           | Binaural L / R Out   |
                           +----------------------+
```

### 1. Coordinate System & Direction Cosines
Positions are calculated using spherical coordinate conventions:
- $\theta$: Azimuth phase $[0, 1)$ corresponding to $[0^\circ, 360^\circ)$ ($0^\circ = \text{front}$, $90^\circ = \text{right}$, $180^\circ = \text{rear}$, $270^\circ = \text{left}$).
- $\phi$: Elevation angle $[-90^\circ, +90^\circ]$ ($-90^\circ = \text{below}$, $0^\circ = \text{horizontal}$, $+90^\circ = \text{overhead}$).
- Direction cosines:
  $$s = \cos\phi \sin\theta \quad \text{(Lateral projection: } -1 = \text{left}, +1 = \text{right)}$$
  $$c = \cos\phi \cos\theta \quad \text{(Front/Rear projection: } +1 = \text{front}, -1 = \text{rear)}$$
  $$v = \sin\phi \quad \text{(Vertical projection: } +1 = \text{overhead}, -1 = \text{below)}$$

### 2. Interaural Time Difference (ITD)
Calculated via Woodworth's spherical head acoustic ray-tracing model coupled with 4-point Hermite cubic fractional delay interpolation:
$$\text{woodworth\_scale}(\theta) = \frac{\sin\theta_{\text{lat}} + \theta_{\text{lat}}}{1.0 + \frac{\pi}{2}}$$
$$\text{ITD}(s, d, \alpha) = \text{HRTF\_MAX\_ITD\_S} \cdot \text{woodworth\_scale}(\theta) \cdot \text{nf\_itd\_scale}(d) \cdot \alpha$$
where:
- $\text{HRTF\_MAX\_ITD\_S} = 0.70\text{ ms}$ (reference cranial diameter).
- $\theta_{\text{lat}} = \arcsin(|s|)$ represents the lateral incident angle, eliminating the $\sim 0.12\text{ ms}$ over-estimate of sinusoidal models at intermediate angles ($30^\circ\text{--}60^\circ$).
- 4-point Hermite cubic spline interpolation ensures a flat passband up to Nyquist without angle-dependent comb filtering on the far ear.
- $\text{nf\_itd\_scale}(d) = 1.0 + \left(\frac{0.00765}{2 d^2 + 0.00765} - \text{offset}\right)$ accounts for spherical wavefront curvature increase at distances $d < 1\text{ m}$, continuous at $1\text{ m}$.
- $\alpha \in [0.70, 1.30]$ is the anthropometric **Ear Scale** parameter.

### 3. Frequency-Dependent Interaural Level Difference (ILD)
Head shadow models frequency-dependent diffraction around the cranial sphere:
- **Low-Frequency Cranial Diffraction**: Sounds below $500\text{ Hz}$ bend around the skull with modest loss (broadband contralateral shadow of $-4.0\text{ dB}$, $\text{far\_gain} = 0.6310$).
- **Contralateral High-Frequency Shadow**: The pinna/air high-shelf cascade introduces an additional $-9.0\text{ dB}$ attenuation on the far ear (totaling $-13.0\text{ dB}$ HF shadow), accurately matching measured human HRIRs.
- Ipsilateral and contralateral gains are normalised to maintain acoustic energy balance:
  $$g_L = 1 - s(1 - \text{far\_gain}), \quad g_R = 1, \quad \text{norm} = \sqrt{\frac{g_L^2 + g_R^2}{2}}$$

### 4. Anthropometric Pinna Spectral Cues
Five cascading biquad filters in transposed direct-form II dynamically shape frequency content based on orientation and scale factor $\alpha$:
- **Presence Resonance Peak**: Centred at $f = 3900\text{ Hz} / \alpha$, boosting up to $+9\text{ dB}$ for frontal sources to provide clarity and front-image definition.
- **Dynamic Concha Notch**: Median-plane elevation notch providing pinna attenuation shifting between $3.5\text{ kHz}$ (below, $v=-1$) and $9.5\text{ kHz}$ (overhead, $v=+1$), scaled by $\alpha$:
  $$f_{\text{notch}} = \frac{6500 + 3000 v}{\alpha}$$
- **High-Frequency Air Shelf**: Cutoff at $8500\text{ Hz} / \alpha$, progressively attenuating rearward and overhead sources.
- **Lateral Pinna Asymmetry**: Peaking filter at $2200\text{ Hz} / \alpha$, differentiating lateral positions from standard intensity panning.
- **Near-Field Low-Frequency ILD Divergence (DVF)**: Low-shelf filter at $350\text{ Hz} / \alpha$ delivering up to $9.5\text{ dB}$ of low-frequency ILD divergence ($\pm 4.75\text{ dB}$ near/far ear shelving) for sources within $1\text{ metre}$.

### 5. 3D Directional Room Boundaries & Distance-Dependent DRR
A circular buffer models early reflections from five boundary surfaces:
- Floor ($4.8\text{ ms}$, modulated by vertical projection $v$)
- Ceiling ($8.1\text{ ms}$, modulated by vertical projection $v$)
- Left wall ($13.5\text{ ms}$, modulated by lateral projection $s$)
- Right wall ($16.2\text{ ms}$, modulated by lateral projection $s$)
- Back wall ($23.4\text{ ms}$, modulated by front/rear projection $c$)

Reflections exhibit acoustic ILD at the ears and pass through a 1-pole high-frequency wall absorption filter ($35\%$ damping). Rather than scaling with direct inverse distance, room reflections scale with a diffuse room factor $\frac{1}{\sqrt{1 + 0.15 d}}$, establishing a realistic Direct-to-Reverberant Ratio (DRR) gradient that serves as the primary acoustic distance cue.

### 6. Master Headroom & Soft-Knee Saturation
- **Headroom Scaling**: Direct signal scaled by $0.50$ ($-6.0\text{ dBFS}$), ensuring that $+6\text{ dB}$ pinna resonances on full-scale $0\text{ dBFS}$ inputs stay cleanly within linear headroom without driving the soft-limiter.
- **Near-Field Proximity Gain**: Gentle proximity boost below $1\text{ m}$ (up to $+3\text{ dB}$ at $5\text{ cm}$) providing natural auditory intimacy.
- **Tanh Soft-Knee Saturation**: Transparent soft knee engaging above $-1.0\text{ dBFS}$ ($0.89125$), guaranteeing peak output strictly never exceeds $0\text{ dBFS}$.

---

## Plugin Formats

### 1. VST3 (`RotatingHRTF_v2.vst3`)
- **Compatibility**: Certified for Ableton Live 10, 11, 12, Cubase, Studio One, Reaper, and all VST3 hosts on Windows x64.
- **Channel Configurations**:
  - Stereo-in / Stereo-out (downmixes stereo track input to mono for true point-source spatialisation).
  - Mono-in / Stereo-out (native mono bus support).
- **Automation**: Full sample-accurate parameter automation.
- **Installation Directory**:
  ```
  C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3
  ```

### 2. CLAP (`RotatingHRTF_v2.clap`)
- **Compatibility**: Bitwig Studio, Reaper, FL Studio, and native CLAP hosts.
- **Features**: Thread-safe parameter flush, state extension, audio-ports extension, transport synchronisation.

---

## Parameters Reference

| ID (VST3/CLAP) | Parameter | Display Name | VST3 Range & Units | CLAP Range & Units | Default | Description |
|---|---|---|---|---|---|---|
| `100` / `1` | `kParamDistance` | **Distance** | 0.05 – 20.0 m | 0.05 – 20.0 m | 2.0 m | Radial distance. Follows $1/d$ inverse-distance law with near-field DVF divergence. |
| `101` / `2` | `kParamRotation` | **Rotation** | 0.0 – 360.0 deg | 0.0 – 1.0 phase | 0.0 (front) | Horizontal azimuth angle ($0.25 = 90^\circ$ right, $0.5 = 180^\circ$ rear, $0.75 = 270^\circ$ left). |
| `102` / `3` | `kParamElevation` | **Elevation** | -90.0 – +90.0 deg | -90.0 – +90.0 deg | 0.0 deg | Vertical angle ($-90^\circ$ below, $0^\circ$ horizontal, $+90^\circ$ overhead). Modulates concha notches. |
| `103` / `4` | `kParamSpace` | **Space** | 0.0 – 100.0 % | 0.0 – 1.0 factor | 15.0 % (0.15) | Room boundary early reflection gain. Pulls perceived sound out of the skull. |
| `104` / `5` | `kParamTestPulse` | **Test Pulse** | Off / On (0 / 1) | Off / On (0.0 / 1.0) | Off | Dedicated tick-box replacing track audio with tempo-synchronised test pulses. |
| `105` / `6` | `kParamTestTone` | **Test Tone** | 0.0 – 100.0 % | 0.0 – 1.0 factor | 50.0 % (0.50) | Timbre morphing: `0%` = sub-rumble, `50%` = calibrated pink noise, `100%` = crisp transient snap. |
| `106` / `7` | `kParamEarScale` | **Ear Scale** | 70.0 – 130.0 % | 0.70 – 1.30 factor | 100.0 % (1.00) | Anthropometric skull and pinna scale $\alpha$. Scales ITD delay and shifts notch frequencies ($f' = f / \alpha$). |

All parameters feature continuous exponential slewing ($20\text{ ms}$ to $50\text{ ms}$) in `hrtf_core.c` to guarantee completely zipper-free, glitch-free automation during live playback.

---

## Ableton Live Integration Guide

### Automated 1-Click Install
Run [`install_ableton.bat`](install_ableton.bat). It self-elevates with Administrator permissions if necessary and deploys the binary into the standard Windows 64-bit VST3 bundle location:
```cmd
install_ableton.bat
```

### Manual Installation
```cmd
mkdir "C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win"
copy /y RotatingHRTF_v2.vst3 "C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win\"
```

### Loading in Ableton Live
1. Open Ableton Live **Preferences** (`Ctrl + ,`) -> **Plug-Ins**.
2. Turn **"Use VST3 Plug-In System Folders"** to **ON**.
3. Click **"Rescan Plug-ins"** (hold `Alt` while clicking for a deep rescan).
4. In Ableton's browser: navigate to **Plug-Ins** -> **VST3** -> **Example Audio** -> **Rotating HRTF v2**.
5. Drag the plugin onto any audio track or return track.
6. Check the **Test Pulse** box to immediately verify 3D rotation, elevation, and room externalisation with tempo-aligned pulses.

---

## Test Signal Generator & Pure C CLI

### Standalone CLI Tool: `tools/generate_test_pulse.c`
A pure C command-line tool directly linking with `hrtf_core.c` to generate psychoacoustically calibrated WAV files with exact DSP parity to the plugin.

#### Compilation:
```cmd
cl.exe /nologo /O2 /I. tools\generate_test_pulse.c hrtf_core.c /Fo:build\ /Fe:build\generate_test_pulse.exe
```

#### CLI Options:
```text
Rotating HRTF Test Pulse Generator (Pure C CLI)
Synthesises calibrated test signals for 3D HRTF spatialisation benchmarking.

Usage: generate_test_pulse [options]

Options:
  -o, --output <path>       Output WAV file path (default writes standard test files)
  -t, --tone <0.0..1.0>     Tone morphing: 0.0=low rumble, 0.5=pink noise, 1.0=crisp transient (default: 0.5)
  -m, --mode <mode>         Signal mode: noise, sine, click (default: noise)
  -f, --freq <Hz>           Sine test frequency in Hz (default: 1000.0)
  -b, --bpm <BPM>           Tempo in beats per minute (default: 120.0)
  -n, --beats <count>       Total number of beats to synthesise (default: 8)
  -d, --dur <ms>            Pulse duration in milliseconds (default: 200.0)
  -s, --samplerate <Hz>     Audio sample rate in Hz (default: 48000)
  -h, --help                Display this help message and exit
```

#### Examples:
```cmd
:: Generate standard 8-beat reference pink noise at 120 BPM
build\generate_test_pulse.exe -o test_signals\reference_pink.wav -t 0.5

:: Generate low-frequency rumble pulse for subwoofer/bass evaluation
build\generate_test_pulse.exe -o test_signals\sub_rumble.wav -t 0.0 -b 128 -n 4

:: Generate snappy Dirac click impulse for exact ITD delay inspection
build\generate_test_pulse.exe -o test_signals\impulse_clicks.wav -m click -b 120

:: Generate a 1 kHz sine burst pattern
build\generate_test_pulse.exe -o test_signals\sine_1k.wav -m sine -f 1000 -d 150
```

---

## Automated Verification & Test Harness

Every build executes comprehensive automated test binaries that thoroughly exercise the DSP engine, parameter automation, and edge cases:

### VST3 Test Suite (`test_vst3.cpp`)
- **Factory & Controller**: Validates COM class creation, parameter registration ($7$ parameters), and normalisation curves.
- **Binaural Energy & ILD**: Verifies that lateral panning ($90^\circ$) produces over $1.5\times$ more energy in the ipsilateral ear than the contralateral ear.
- **Sample-Accurate Automation**: Verifies that parameter change events are applied at their exact sample offsets within the block.
- **Oversize Block Handling**: Confirms that blocks larger than `maxSamplesPerBlock` ($1024+$ samples) process correctly and produce valid audio.
- **3D Elevation Symmetry**: Verifies that overhead elevation ($+90^\circ$) yields symmetric left/right energy ($|L - R| < 1.0, L > 0.1$).
- **Test Pulse Generation & Silence**: Confirms that enabling `Test Pulse` on a silent input stream generates audio ($E > 1.0$), and disabling it produces silence.
- **Test Tone Differentiation**: Confirms significant spectral variance ($\Delta > 5.0$) between low rumble and crisp transient settings.
- **Ear Scale Response**: Verifies that varying `Ear Scale` from $70\%$ to $130\%$ produces substantial ITD delay and pinna notch shifts ($\Delta > 20.0$).
- **Null Buffer Immunity**: Asserts resilience against null input/output channel arrays.

### CLAP Test Suite (`test_clap.c`)
- **Lifecycle & Activation**: Tests `init()`, `activate()`, and `deactivate()`.
- **Parameter Validation & Enumeration**: Tests parameter enumeration ($7$ parameters) and string conversions.
- **State Serialisation Round-Trip**: Verifies that saving state to a stream and restoring it accurately preserves all 7 parameter values across sessions.
- **Legacy State Migration**: Validates that legacy v1 state archives load cleanly and reset new parameters to factory defaults.
- **Un-Aliased Near-Field ITD**: Asserts that extreme near-field delay ($5\text{ cm}$, $130\%$ ear scale) arrives at sample $60\text{--}70$ without circular buffer wrapping.
- **Deterministic Spectral Differentiation**: Dogfoods internal test pulse to verify that crisp transient clicks exhibit $> 2\times$ the spectral first-difference energy of low rumble pulses.
- **Zero-Input Pulse Synthesis & Silence**: Asserts that `Test Pulse` on silent input produces audible sound, and turning it off restores absolute silence ($< 10^{-12}$).

---

## Building from Source

### 1. Prerequisites
- **Operating System**: Windows 10 or 11 (64-bit).
- **Compiler**: Visual Studio 2022 (Community, Professional, or Build Tools) with MSVC C/C++ x64 tools (`cl.exe`).
- **Native MSVC Toolchain**: Employs the MSVC C/C++ command-line compiler directly for all compilation, signal synthesis, and automated test execution.

### 2. Cloning the Repository
```cmd
git clone --recurse-submodules https://github.com/twobob/hrtf.git
cd hrtf
```
If already cloned without submodules:
```cmd
git submodule update --init --recursive
```

### 3. One-Click Build & Test
Execute `build_all.bat`:
```cmd
build_all.bat
```
This batch script performs 5 sequential stages:
1. **Build CLAP Plugin**: Compiles `RotatingHRTF_v2.clap` in pure C11.
2. **Build VST3 Plugin**: Compiles `RotatingHRTF_v2.vst3` and creates the standard bundle directory structure for Ableton Live.
3. **Generate Test Audio**: Compiles and runs `tools\generate_test_pulse.c` in pure C to synthesise reference audio into `pulsed_pink_noise_48k.wav` and `test_signals/`.
4. **Run Automated Test Suites**: Compiles and runs `test_clap.exe` (pure C dogfooding test harness) and `test_vst3.exe` (VST3 host validation).
5. **System Deployment**: Copies the VST3 bundle to `C:\Program Files\Common Files\VST3\`.

---

## Project Architecture

```
hrtf/
├── hrtf_core.h                     # Public HRTF DSP and test generator API
├── hrtf_core.c                     # Core audio DSP (biquads, delay, ILD, slewing)
├── hrtf_clap.c                     # CLAP plugin wrapper implementation
├── build_all.bat                   # Master build, test, and deployment script
├── install_ableton.bat             # UAC-elevating installer for Ableton Live
├── test_clap.c                     # Automated test harness for CLAP
├── test_vst3.cpp                   # Automated test harness for VST3
├── tools/
│   └── generate_test_pulse.c       # Pure C standalone CLI audio synthesiser
├── vst3/
│   ├── plugids.h                   # VST3 GUIDs, parameter IDs, state version
│   ├── plugprocessor.h             # VST3 audio processor declaration
│   ├── plugprocessor.cpp           # VST3 audio processor & downmixing logic
│   ├── plugcontroller.h            # VST3 controller declaration
│   ├── plugcontroller.cpp          # VST3 parameters & state deserialisation
│   ├── plugfactory.cpp             # VST3 plugin factory exports
│   └── version.h                   # Plugin versioning metadata
├── test_signals/
│   └── pulsed_pink_noise_48k.wav   # Reference 24-bit 48 kHz test audio
├── clap-src/                       # CLAP SDK headers (git submodule)
├── clap-wrapper/                   # CLAP wrapper utility (git submodule)
├── pluginterfaces/                 # Steinberg VST3 SDK interfaces (git submodule)
└── public.sdk/                     # Steinberg VST3 SDK source (git submodule)
```

---

## Engineering Standards

- **Pure Native C/C++ Toolchain**: All audio generation, automated verification suites, build automation, and CLI utilities are authored entirely in native C and C++.
- **Deterministic Audio Output**: Bit-exact mathematical parity between the standalone CLI generator and the plugin's internal engine.
- **Audio Thread Safety**: Zero heap allocations, zero system calls, zero mutexes or locks in any audio rendering callback.
