# Future Roadmap & SOTA Research Tracking (TODO)

This document tracks future enhancements and research targets derived from the latest state-of-the-art (SOTA) literature in binaural spatial audio (AES69/SOFA, IEEE TASLP, arXiv 2024–2026).

---

## 1. SOFA (AES69 Standard) File Support & Measured HRIRs
- [ ] **Standard Format Reader**:
  - Integrate a minimal C/C++ reader for the Spatially Oriented Format for Acoustics (AES69-2015/2022).
  - Target embedded support for standard measured databases: KEMAR, Neumann KU100, HUTUBS, ARI, and CIPIC.
- [ ] **Low-Latency Partitioned Convolution**:
  - Implement Non-Uniform Partitioned FFT Convolution (NUPC) or Uniform WOLA convolution for rendering long impulse responses (512–2048 samples) with 0-sample algorithmic latency.
- [ ] **Spatial Grid Interpolation**:
  - Implement Barycentric interpolation over Delaunay spherical triangulation or Spherical Harmonics decomposition (Ambisonics / HOA) for continuous source positioning across discrete measurement points.

---

## 2. Dynamic 6-DoF Head Tracking
- [ ] **OSC (Open Sound Control) Input**:
  - Add an asynchronous OSC UDP listener thread to receive orientation streams (`yaw`, `pitch`, `roll`) from external head-trackers (e.g. Supperware, Bridgehead, Waves Nx, AirPods via head-tracking bridge).
- [ ] **MIDI CC Mapping for Orientation**:
  - Support high-resolution 14-bit MIDI CC mapping for external head-tracker hardware.
- [ ] **Coordinate Frame Transformation**:
  - Implement dynamic spherical rotation matrix transformations between world coordinates and listener head coordinates with $< 20\text{ ms}$ motion-to-sound latency to eliminate static front/back confusion.

---

## 3. Deep Learning & Neural HRTF Personalisation
- [ ] **Neural IIR Filter Fields (NIIRF) / Implicit Neural Representations (INR)**:
  - Investigate tiny embedded coordinate-based MLP inference (using an embedded ONNX runtime or lightweight C float matrix multiplies) mapping $(\theta, \phi, r)$ continuously to minimum-phase filter coefficients.
- [ ] **Generative HRTF Personalisation from Anthropometry**:
  - Explore loading weights from Denoising Diffusion Probabilistic Models (DDPM) or VAE latent representations conditioned on user head circumference, pinna height, and cavum concha depth.

---

## 4. Advanced Acoustic Room Modelling
- [ ] **Full 6-Sided Image Source Model**:
  - Expand early reflections to allow customisable virtual room dimensions (length, width, height) and material absorption coefficients (concrete, wood, acoustic plaster).
- [ ] **Late Diffuse Reverberation**:
  - Integrate a feedback delay network (FDN) reverberator with frequency-dependent decay ($T_{60}$) coupled to early reflections for complete binaural room impulse response (BRIR) simulation.
