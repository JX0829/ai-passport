#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
md2pdf — Markdown to PDF converter with CJK text wrapping.
Replaces the old canvas.drawString() approach with reportlab Platypus
so long Chinese text lines auto-wrap within A4 margins.
"""

import sys
import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.enums import TA_LEFT, TA_CENTER
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

# ── Font registration ──────────────────────────────────────────────
FONT_PATH = '/System/Library/Fonts/STHeiti Light.ttc'
FONT_NAME = 'Chinese'

if not os.path.exists(FONT_PATH):
    # Fallback: try PingFang (also supports CJK)
    FONT_PATH = '/System/Library/Fonts/PingFang.ttc'

pdfmetrics.registerFont(TTFont(FONT_NAME, FONT_PATH))

# ── Style definitions ───────────────────────────────────────────────
def build_styles():
    """Create paragraph styles for markdown heading levels + body text."""
    styles = getSampleStyleSheet()

    body = ParagraphStyle(
        'BodyChinese',
        parent=styles['Normal'],
        fontName=FONT_NAME,
        fontSize=11,
        leading=18,                     # line height
        spaceAfter=4,
        wordWrap='CJK',                 # ← critical: enables Chinese wrapping
        alignment=TA_LEFT,
    )

    h1 = ParagraphStyle(
        'H1Chinese',
        parent=body,
        fontSize=18,
        leading=26,
        spaceBefore=12,
        spaceAfter=10,
    )

    h2 = ParagraphStyle(
        'H2Chinese',
        parent=body,
        fontSize=14,
        leading=22,
        spaceBefore=10,
        spaceAfter=6,
    )

    h3 = ParagraphStyle(
        'H3Chinese',
        parent=body,
        fontSize=12,
        leading=19,
        spaceBefore=8,
        spaceAfter=4,
    )

    # Inline-code / small-text style
    code_style = ParagraphStyle(
        'CodeChinese',
        parent=body,
        fontSize=9,
        leading=14,
        fontName=FONT_NAME,
    )

    return {'body': body, 'h1': h1, 'h2': h2, 'h3': h3, 'code': code_style}


# ── Markdown → Flowables ────────────────────────────────────────────
def md_to_flowables(text, styles):
    """
    Convert raw markdown text into a list of Platypus flowables.
    Supported: #, ##, ### headings, blank lines (spacer), regular paragraphs.
    """
    flowables = []
    lines = text.split('\n')

    for raw in lines:
        line = raw.rstrip()

        # ── Blank line → spacer ──
        if not line.strip():
            flowables.append(Spacer(1, 6))
            continue

        # ── Headings ──
        if line.startswith('# ') and not line.startswith('## '):
            flowables.append(Paragraph(line[2:].strip(), styles['h1']))
        elif line.startswith('## ') and not line.startswith('### '):
            flowables.append(Paragraph(line[3:].strip(), styles['h2']))
        elif line.startswith('### '):
            flowables.append(Paragraph(line[4:].strip(), styles['h3']))

        # ── Horizontal rule ──
        elif line.strip() in ('---', '***', '___'):
            # simple visual divider
            flowables.append(Spacer(1, 6))
            flowables.append(Paragraph('─' * 60, styles['code']))
            flowables.append(Spacer(1, 6))

        # ── Blockquote ──
        elif line.startswith('> '):
            flowables.append(Paragraph(line[2:].strip(), styles['code']))

        # ── Regular paragraph ──
        else:
            flowables.append(Paragraph(line, styles['body']))

    return flowables


# ── Main ────────────────────────────────────────────────────────────
def create_pdf(input_md: str, output_pdf: str):
    """Read a markdown file and generate a wrapped PDF."""

    with open(input_md, 'r', encoding='utf-8') as f:
        text = f.read()

    styles = build_styles()
    flowables = md_to_flowables(text, styles)

    doc = SimpleDocTemplate(
        output_pdf,
        pagesize=A4,
        leftMargin=2 * cm,
        rightMargin=2 * cm,
        topMargin=1.5 * cm,
        bottomMargin=1.5 * cm,
        title=os.path.basename(input_md),
    )

    doc.build(flowables)
    print(f'✅ PDF已生成: {output_pdf}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python md2pdf.py <input.md> [output.pdf]")
        sys.exit(1)

    input_md = sys.argv[1]
    if not os.path.exists(input_md):
        print(f"Error: {input_md} does not exist.")
        sys.exit(1)

    output_pdf = sys.argv[2] if len(sys.argv) > 2 else input_md.rsplit('.', 1)[0] + '.pdf'
    create_pdf(input_md, output_pdf)
