## Rotating HRTF v2.0.1

A calibration, reliability and identity release of the real-time 3D binaural spatialiser. Sessions saved with v2.0.0 do not carry over: the plugin now ships under its own identity (vendor **psipi**, CLAP ID `psipi.hrtf`, new VST3 class IDs) with a new state layout.

### Level and controls
- **Calibrated output level**: Pink noise at the default position (2 m, front, 15% Space) now comes out 2 dB below the input, instead of about 15.6 dB below. Beyond 2 m the level falls by the 1/d law (about -8 dB at 4 m, -22 dB at 20 m); closer in it rises gently to a ceiling. No ear is ever louder than the input, in any direction, at any Space or Ear Scale setting (checked with long pink-noise sweeps).
- **Space is loudness-neutral**: Raising Space re-balances direct sound and room without raising the level. The direct-to-reverberant ratio still tracks distance exactly as before.
- **Logarithmic Distance control**: Every doubling of distance takes the same travel, so 5 cm to 1 m is now half the knob instead of under 5%.
- **Reflections switch**: A new On/Off parameter fades the room out and back in over about 20 ms without losing the Space setting.
- **Near-field ILD**: The 9.5 dB near-field low-frequency ILD is now a cut on the far ear rather than a boost/cut split, so the near ear is never pushed above the calibrated level.

### Fixes
- Room reflections no longer click while rotating or changing Ear Scale (reflection delays now glide instead of stepping a whole sample).
- A single NaN or infinite input sample no longer silences the plugin for the rest of the session.
- The render is now bit-identical whatever block size the host uses.
- Filters no longer become unstable at very low sample rates, and the wall absorption no longer changes colour with the sample rate.
- The Test Pulse is click-free: starting playback in the middle of a pulse ramps in, and switching it on or off crossfades over 5 ms. Very short pulses close smoothly.
- CLAP: stereo tracks are downmixed instead of losing the right channel; typing "0.4 deg" no longer gives 144 degrees; parameter events sent with an inactive output or stamped past the block are applied, not dropped.
- VST3: requests tempo, position and transport from the host (Test Pulse sync in hosts such as Cubase), declares an infinite tail so silent-input suspension cannot stop the Test Pulse, applies parameter changes and clears outputs on unrenderable blocks, and links the C runtime statically (no Visual C++ redistributable needed). Passes the Steinberg VST3 validator (47 of 47).
- Installer: elevation no longer splits "C:\Program Files\..." into several arguments.
- Build: finds any Visual Studio 2022 or later edition with vswhere, runs from any directory, and keeps batch files in CRLF so cmd always finds its labels.

### Included Assets
- `RotatingHRTF_v2-windows-x64.zip`: Complete Windows 64-bit release package (VST3 bundle, CLAP plugin, standalone test pulse generator CLI, 1-click installer and README).
- `RotatingHRTF_v2.clap`: Standalone CLAP plugin binary.
