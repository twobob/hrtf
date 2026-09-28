"""
Generate psychoacoustically calibrated pulsed pink noise test signal.
Standard: 48 kHz, 24-bit PCM mono WAV, loopable 8-beat pattern at 120 BPM.
Each beat contains a 200 ms burst of 1/f pink noise with 5 ms Hann onset and decay.
"""

import math
import struct
import wave
import os

def generate_pulsed_pink_noise(filename="pulsed_pink_noise_48k.wav", num_bars=2, bpm=120.0, fs=48000):
    beats_per_bar = 4
    total_beats = num_bars * beats_per_bar
    sec_per_beat = 60.0 / bpm
    total_seconds = total_beats * sec_per_beat
    total_samples = int(total_seconds * fs)

    # 1/f pink noise filter using Paul Kellet's refined 7-pole method
    b0, b1, b2, b3, b4, b5, b6 = 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0

    # 32-bit PRNG state
    prng_state = 0x5A17F00D

    pulse_dur_s = 0.200 # 200 ms
    t_att_s = 0.005     # 5 ms attack
    t_rel_s = 0.005     # 5 ms release

    samples = []

    for i in range(total_samples):
        # Time position within current beat
        t_global = i / fs
        beat_num = t_global / sec_per_beat
        phase_in_beat = beat_num - math.floor(beat_num)
        t_in_beat = phase_in_beat * sec_per_beat

        # Raised-cosine (Hann) envelope
        envelope = 0.0
        if 0.0 <= t_in_beat < pulse_dur_s:
            if t_in_beat < t_att_s:
                envelope = 0.5 * (1.0 - math.cos(math.pi * t_in_beat / t_att_s))
            elif t_in_beat > pulse_dur_s - t_rel_s:
                envelope = 0.5 * (1.0 + math.cos(math.pi * (t_in_beat - (pulse_dur_s - t_rel_s)) / t_rel_s))
            else:
                envelope = 1.0

        # PRNG xorshift32
        prng_state ^= (prng_state << 13) & 0xFFFFFFFF
        prng_state ^= (prng_state >> 17) & 0xFFFFFFFF
        prng_state ^= (prng_state << 5)  & 0xFFFFFFFF
        
        # Signed 32-bit conversion to [-1.0, 1.0]
        signed_val = prng_state if prng_state < 0x80000000 else prng_state - 0x100000000
        white = signed_val / 2147483648.0

        # 7-pole filter
        b0 = 0.99886 * b0 + white * 0.0555179
        b1 = 0.99332 * b1 + white * 0.0750759
        b2 = 0.96900 * b2 + white * 0.1538520
        b3 = 0.86650 * b3 + white * 0.3104856
        b4 = 0.55000 * b4 + white * 0.5329522
        b5 = -0.7616  * b5 - white * 0.0168980
        pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362
        b6 = white * 0.115926

        # Scale to calibrated test level (-14 dBFS nominal burst RMS)
        sample = pink * 0.18 * envelope

        # Hard safety clamp to [-1.0, +1.0]
        if sample > 0.999999:
            sample = 0.999999
        elif sample < -0.999999:
            sample = -0.999999

        samples.append(sample)

    # Write 24-bit PCM WAV
    # 24-bit signed int range: [-8388608, 8388607]
    scale24 = 8388607.0

    raw_bytes = bytearray()
    for s in samples:
        val24 = int(round(s * scale24))
        val24 = max(-8388608, min(8388607, val24))
        # Pack 3 bytes little-endian
        raw_bytes.append(val24 & 0xFF)
        raw_bytes.append((val24 >> 8) & 0xFF)
        raw_bytes.append((val24 >> 16) & 0xFF)

    os.makedirs(os.path.dirname(os.path.abspath(filename)), exist_ok=True)
    with wave.open(filename, 'wb') as wav_file:
        wav_file.setnchannels(1)      # Mono
        wav_file.setsampwidth(3)      # 24-bit = 3 bytes
        wav_file.setframerate(fs)
        wav_file.writeframes(raw_bytes)

    print(f"Generated {filename}: {total_samples} samples, {total_seconds:.2f} s, 24-bit @ {fs} Hz")

if __name__ == "__main__":
    generate_pulsed_pink_noise("pulsed_pink_noise_48k.wav")
    generate_pulsed_pink_noise("test_signals/pulsed_pink_noise_48k.wav")
