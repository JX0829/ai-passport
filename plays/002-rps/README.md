<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# 002 - Rock Paper Scissors

A handheld Rock-Paper-Scissors battle game with multi-difficulty AI built for FoloToy AI Passport (ESP32-C3).

## 1. Gameplay & Mechanics

- **Multi-Tier Difficulty AI**:
  - 🟢 **Easy**: Friendly handicap with ~60% probability of yielding winning counters for player.
  - 🟡 **Normal**: Completely fair, 1/3 uniform random secret choices.
  - 🔴 **Hard**: Advanced psychological counter-prediction (analyzing win-stay / lose-shift habits and high-frequency patterns).
- **Secret Pick Safeguard**: AI calculates its move secretly before the round begins without exposing any hint on screen.
- **Graphic Choice Cards**: Three pixel-art cards (Scissors, Rock, Paper).
- **Classic Rules & Reactions**:
  - Scissors cuts Paper
  - Paper covers Rock
  - Rock crushes Scissors
  - Winner banner, detailed feedback, and mascot celebration on victory.
- **Score Tracker**: Displays cumulative stats (Wins, Losses, Draws, and current Difficulty).

## 2. Button Controls

| Button | Trigger | Action |
| --- | --- | --- |
| **UP** | Long press or Double click | **Cycle difficulty** (Easy ⇄ Normal ⇄ Hard) |
| **UP / DOWN** | Short click | Select choice (Scissors / Rock / Paper) |
| **OK** | Short click | Confirm & reveal / Start next round |
| **OK** | Hold | Exit back to Main Menu |
