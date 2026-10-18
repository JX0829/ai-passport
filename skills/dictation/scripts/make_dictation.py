#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""听写音频生成器（macOS）。

用法:
  python3 make_dictation.py --subject english --words "dance,jump,player" --out /tmp/dict.m4a
  python3 make_dictation.py --subject chinese --words "传统,节日,春节" --out /tmp/dict.m4a

说明:
  - 中文：Tingting 读全部内容
  - 英语：中文开场/结束用 Tingting，英文单词用 Samantha
  - 仅依赖 macOS 自带 say / afconvert；不要用 ffmpeg
  - 依次: say -> wav(LEI16@22050 mono) -> 拼接(带静音) -> m4a(aac@22050,64k)
"""
import argparse
import os
import subprocess
import sys
import tempfile
import wave

SR = 22050
CN_VOICE = "Tingting"
EN_VOICE = os.environ.get("DICT_EN_VOICE", "Samantha")
CN_RATE = 150
EN_RATE = 135


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("命令失败: %s\n%s\n%s\n" % (" ".join(cmd), r.stdout, r.stderr))
        raise SystemExit(1)


def speech_wav(workdir, idx, voice, rate, text):
    aiff = os.path.join(workdir, "s%03d.aiff" % idx)
    wav = os.path.join(workdir, "s%03d.wav" % idx)
    run(["say", "-v", voice, "-r", str(rate), "-o", aiff, text])
    run(["afconvert", "-f", "WAVE", "-d", "LEI16@%d" % SR, "-c", "1", aiff, wav])
    return wav


def silence_bytes(seconds):
    return b"\x00\x00" * int(SR * seconds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--subject", choices=["chinese", "english"], required=True)
    ap.add_argument("--words", required=True, help="逗号分隔的词表")
    ap.add_argument("--out", required=True, help="输出 m4a 路径")
    ap.add_argument("--repeat", type=int, default=None, help="每个词读几遍")
    ap.add_argument("--gap", type=float, default=None, help="词内间隔（秒）")
    ap.add_argument("--word-gap", type=float, default=None, help="词间间隔（秒）")
    ap.add_argument("--opening", default=None)
    ap.add_argument("--closing", default=None)
    ap.add_argument("--keep-wav", action="store_true")
    args = ap.parse_args()

    words = [w.strip() for w in args.words.split(",") if w.strip()]
    if not words:
        raise SystemExit("词表为空")

    if args.subject == "english":
        repeat = 2 if args.repeat is None else args.repeat
        gap = 3.0 if args.gap is None else args.gap
        word_gap = 8.0 if args.word_gap is None else args.word_gap
        opening = args.opening or "英语听写开始，请准备好纸和笔"
        closing = args.closing or "听写结束，请检查一遍"
    else:
        repeat = 3 if args.repeat is None else args.repeat
        gap = 2.0 if args.gap is None else args.gap
        word_gap = 5.0 if args.word_gap is None else args.word_gap
        opening = args.opening or "听写开始，请准备好纸和笔"
        closing = args.closing or "听写结束，请检查一遍"

    work = tempfile.mkdtemp(prefix="dictation_")
    idx = 0
    seq = []  # (wav_path|None, silence_seconds)

    idx += 1
    seq.append((speech_wav(work, idx, CN_VOICE, CN_RATE, opening), 0.8))
    for i, w in enumerate(words, 1):
        voice = EN_VOICE if args.subject == "english" else CN_VOICE
        rate = EN_RATE if args.subject == "english" else CN_RATE
        for k in range(repeat):
            idx += 1
            last = (k == repeat - 1)
            seq.append((speech_wav(work, idx, voice, rate, w),
                        (word_gap if last and i < len(words) else gap)))
    idx += 1
    seq.append((speech_wav(work, idx, CN_VOICE, CN_RATE, closing), 0.0))

    frames = b""
    for path, silence in seq:
        with wave.open(path, "rb") as f:
            frames += f.readframes(f.getnframes())
        if silence:
            frames += silence_bytes(silence)

    final_wav = os.path.join(work, "final.wav")
    with wave.open(final_wav, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(frames)

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    run(["afconvert", "-f", "m4af", "-d", "aac@%d" % SR, "-b", "64000", final_wav, out])

    dur = len(frames) / 2.0 / SR
    print("OK %s  词数=%d 每词遍数=%d 时长≈%.1fs" % (out, len(words), repeat, dur))
    if args.keep_wav:
        print("wav:", final_wav)


if __name__ == "__main__":
    main()
