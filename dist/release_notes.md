## Rotating HRTF v2.0.0

A high-performance real-time 3D binaural spatialiser audio plugin built upon an acoustically calibrated Head-Related Transfer Function (HRTF) DSP core.

### Highlights
- **Binaural 3D Positioning**: Full 360-degree horizontal azimuth rotation and -90 to +90 degree vertical elevation.
- **Woodworth Spherical Ray-Tracing ITD**: Accurate interaural time delays using Woodworth's spherical head acoustic model with 4-point Hermite cubic fractional delay interpolation.
- **Frequency-Dependent Head Shadow**: Differentiates low-frequency diffraction (-4 dB LF) from deep contralateral pinna/air high-shelf shadowing (up to -13 dB HF).
- **3D Directional Room Boundaries**: Boundary reflections (floor, ceiling, lateral walls, rear wall) modulated dynamically by 3D direction cosines and decoupled from direct sound to establish an authentic Direct-to-Reverberant Ratio (DRR) gradient.
- **Distance Variation Function (DVF) & Near-Field Intimacy**: Near-field low-frequency ILD divergence up to 9.5 dB at 5 cm with smooth proximity gain below 1 metre.
- **Anthropometric Head & Pinna Scaling**: Adjust head circumference and ear dimensions from 70% to 130%.
- **Integrated Test Pulse Generator**: Built-in tempo-synchronised pulsed pink noise morphing from low rumble to crisp transient snap (with sine and click modes available in the CLI tool).
- **Plugin Formats**:
  - **CLAP**: Native support for Bitwig Studio, Reaper, FL Studio.
  - **VST3**: Standard bundle format with 1-click installer for Ableton Live.

### Included Assets
- `RotatingHRTF_v2-windows-x64.zip`: Complete Windows 64-bit release package (VST3 bundle, CLAP plugin, standalone test pulse generator CLI, and 1-click installer).
- `RotatingHRTF_v2.clap`: Standalone CLAP plugin binary.
