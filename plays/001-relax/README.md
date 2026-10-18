<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# 001 - Relax Time

A randomized break and focus companion toy application built for FoloToy AI Passport (ESP32-C3).

## 1. Features and Positioning

- **Random Duration Picker**: Press OK to randomly select a 15, 20, 25, or 30-minute timer session with real-time UI animation.
- **Single-Opportunity Adjustment**: During countdown, fine-tune the timer once with UP (+5 min) or DOWN (-5 min).
- **Audio & Visual Completion Alert**: Mascot animation on timeout with authentic Chinese human voice alert ("Break is over, time to get back to work!").
- **Unified Status Bar**: Real-time Beijing time synced in the background via Wi-Fi SNTP along with compact 10px battery SOC monitoring in the top-right corner.
- **Embedded CJK Typography**: Custom generated 14px Source Han Sans SC font ensuring sharp, clean Chinese characters.

## 2. Key Controls

| Button | Action | Description |
| --- | --- | --- |
| **OK** | Short press | Start random timer / Pause & resume |
| **OK** | Long press | Exit to main menu |
| **UP** | Short press | Add 5 minutes (single opportunity) |
| **DOWN** | Short press | Subtract 5 minutes (single opportunity) |

## 3. Architecture & Sources

Source files inside `main/`:
- `main/demo_relax.c`: State machine, timer loop, and LVGL animations
- `main/voice_prompt.c` & `voice_prompt.h`: Human voice PCM data & I2S playback worker
- `main/status_bar.c` & `status_bar.h`: Integrated top-right clock & battery badge
- `main/time_manager.c` & `time_manager.h`: Background Wi-Fi & NTP synchronization
- `main/font_chinese_14.c` & `font_chinese_14.h`: Generated custom 14px Chinese font
