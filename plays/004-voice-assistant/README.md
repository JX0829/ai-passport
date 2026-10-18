<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# 004 - Voice Assistant

Handheld real-time conversational voice assistant built for FoloToy AI Passport (ESP32-C3) powered by OpenAI-compatible LLM multimodal audio and local TTS streaming with instant button barge-in.

## 1. Architecture & Mechanics

- **Audio Pipeline**:
  - Press **OK** to record 3s (16kHz 16-bit PCM).
  - Forwarded to local OpenAI-compatible API (`gemini-3.8-flash`) via `input_audio`.
  - Concurrently invokes local TTS service (`http://192.168.31.170:5050/v1/audio/speech`) for speech generation.
  - Streaming playback decoded on-the-fly via lightweight `minimp3`.
- **Instant Barge-in**:
  - Hitting **OK** at any point during playback halts audio immediately and opens the microphone for the next query.
- **Dual Chat UI**:
  - Displays user transcript and assistant answer with dynamic status badges.

## 2. Controls

| State | Button | Action |
| --- | --- | --- |
| **Idle** | OK (Click) | Start recording |
| **Speaking** | OK (Click) | Barge-in: immediately stop playback & listen |
| **Any** | OK (Hold 1.5s) | Exit to Main Menu |
