#!/usr/bin/env python3
"""
Convert a mono 16-bit PCM WAV into raw PCM for the gong player.
Usage: python3 tools/convert_wav_to_pcm.py input.wav data/gong.pcm
"""

import sys
import wave


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: python3 tools/convert_wav_to_pcm.py input.wav data/gong.pcm")
        return 1

    wav_path = sys.argv[1]
    pcm_path = sys.argv[2]

    with wave.open(wav_path, "rb") as wav_file:
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        sample_rate = wav_file.getframerate()
        if channels != 1 or sample_width != 2:
            print("Error: WAV must be mono 16-bit PCM.")
            return 1
        if sample_rate != 16000:
            print(f"Warning: sample rate is {sample_rate} Hz (recommended: 16000 Hz).")
        frames = wav_file.readframes(wav_file.getnframes())

    with open(pcm_path, "wb") as pcm_file:
        pcm_file.write(frames)

    print(f"Wrote {pcm_path} ({len(frames)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
