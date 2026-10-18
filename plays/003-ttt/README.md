<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# 003 - Tic-Tac-Toe

Handheld 3x3 Tic-Tac-Toe game on FoloToy AI Passport (ESP32-C3) featuring a setup screen, 3-vs-4 piece FIFO elimination, Best-of-Three (BO3) match mode, and dual turn/match timers.

## 1. Gameplay & Mechanics

- **Initial Setup Screen**:
  - Interactive launcher configuring difficulty (Easy, Normal, Unbeatable) and team allegiance (Blue Flag 3-piece limit or Red Flag 4-piece limit).
- **FIFO Piece Elimination**:
  - Blue side holds max 3 pieces; Red side holds max 4 pieces.
  - When quota is reached, placing a new piece automatically removes the earliest placed piece from the board.
- **Best-of-Three (BO3) Match**:
  - Up to 3 rounds. First to 2 wins takes the grand championship.
- **Dual Timers**:
  - **Round Timer**: 2 minutes (120s) countdown per round. Reaching 0 results in a draw.
  - **Turn Timer**: 10s countdown for player; 5s thinking pause before AI plays.
- **Zero Missing Glyphs**:
  - Embedded 1056-character CJK font avoiding all missing glyph square blocks.

## 2. Button Controls

| Screen | Button | Action |
| --- | --- | --- |
| **Setup** | UP / DOWN | Select item (Difficulty, Team, Start) |
| **Setup** | OK | Cycle option or Launch match |
| **Playing** | UP / DOWN | Move cursor across 3x3 grid |
| **Playing** | OK | Place piece / Advance round when over |
| **Any** | OK (Hold) | Exit back to Main Menu |
