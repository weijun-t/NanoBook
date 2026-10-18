"""md_to_pdf.py: renders a NanoBook Markdown report as a PDF.

What it does: Reads a Markdown file and writes an A4 PDF with headings, tables, code blocks, lists,
  images (charts) and page numbers.
  Usage: .venv/bin/python scripts/md_to_pdf.py docs/phase1-report.md docs/phase1-report.pdf

How it works: A line-by-line parser turns the Markdown into reportlab "flowables" (paragraphs,
  tables, code boxes), and reportlab lays them out onto pages. Table columns get a width in
  proportion to their text, but never narrower than their longest word.

Trade-offs: It only supports the Markdown the reports use: # headings, paragraphs, - and 1. lists,
  | tables |, ``` code blocks ```, --- rules, ![caption](image.png) on a line of its own (scaled to
  the page width, path relative to the Markdown file), **bold** and `code`. It uses the DejaVu fonts,
  because reportlab's built-in fonts can't draw characters like "≤". Needs reportlab
  (requirements.txt) and the DejaVu fonts (Ubuntu: fonts-dejavu-core, installed by default).
"""

import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.pdfmetrics import stringWidth
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (HRFlowable, Image, KeepTogether, ListFlowable, ListItem, Paragraph,
                                Preformatted,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

# ---------------------------------------------------------------------------
# Fonts and colours
# ---------------------------------------------------------------------------

FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
pdfmetrics.registerFont(TTFont("Body", str(FONT_DIR / "DejaVuSans.ttf")))
pdfmetrics.registerFont(TTFont("Body-Bold", str(FONT_DIR / "DejaVuSans-Bold.ttf")))
pdfmetrics.registerFont(TTFont("Mono", str(FONT_DIR / "DejaVuSansMono.ttf")))
# Lets <b> inside a paragraph switch to the bold file.
pdfmetrics.registerFontFamily("Body", normal="Body", bold="Body-Bold", italic="Body",
                              boldItalic="Body-Bold")

NAVY = colors.HexColor("#0f2a4a")
BLUE = colors.HexColor("#24456e")
GREY_TEXT = colors.HexColor("#55607a")
RULE = colors.HexColor("#c9d1de")
CELL_LINE = colors.HexColor("#d5dce8")
ZEBRA = colors.HexColor("#f6f8fb")
CODE_BG = colors.HexColor("#f4f6fa")
INLINE_CODE_BG = "#eef2f7"

MARGIN = 16 * mm
PAGE_W, PAGE_H = A4
TEXT_W = PAGE_W - 2 * MARGIN

# ---------------------------------------------------------------------------
# Styles
# ---------------------------------------------------------------------------

BODY = ParagraphStyle("body", fontName="Body", fontSize=9.5, leading=13.5,
                      textColor=colors.HexColor("#1d2330"), spaceBefore=2, spaceAfter=4,
                      alignment=TA_LEFT)
SUBTITLE = ParagraphStyle("subtitle", parent=BODY, textColor=GREY_TEXT, spaceAfter=8)
H1 = ParagraphStyle("h1", parent=BODY, fontName="Body-Bold", fontSize=21, leading=26,
                    textColor=NAVY, spaceAfter=2)
H2 = ParagraphStyle("h2", parent=BODY, fontName="Body-Bold", fontSize=13.5, leading=17,
                    textColor=NAVY, spaceBefore=12, spaceAfter=0, keepWithNext=1)
H3 = ParagraphStyle("h3", parent=BODY, fontName="Body-Bold", fontSize=11, leading=14,
                    textColor=BLUE, spaceBefore=9, spaceAfter=4, keepWithNext=1)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=8.4, leading=11, spaceBefore=0, spaceAfter=0)
CELL_HEAD = ParagraphStyle("cellhead", parent=CELL, fontName="Body-Bold", textColor=colors.white)
CELL_KEY = ParagraphStyle("cellkey", parent=CELL, fontName="Body-Bold", textColor=BLUE)
CODE = ParagraphStyle("code", fontName="Mono", fontSize=7.8, leading=10.4,
                      textColor=colors.HexColor("#1d2330"))
CAPTION = ParagraphStyle("caption", fontName="Body", fontSize=8, leading=10.5,
                         textColor=GREY_TEXT, spaceBefore=2, spaceAfter=0, alignment=1)
LIST_ITEM = ParagraphStyle("li", parent=BODY, spaceBefore=1, spaceAfter=1)

# ---------------------------------------------------------------------------
# Inline markup: `code` and **bold** -> reportlab's mini-HTML
# ---------------------------------------------------------------------------


def inline(text: str) -> str:
    """Escape &, <, > first (reportlab parses the result as markup), then add tags.
    Code spans are set aside as numbered placeholders first, so ** inside code stays literal,
    while **bold** can still contain code spans."""
    codes: list[str] = []

    def set_aside(m: re.Match) -> str:
        codes.append(m.group(1))
        return f"\x00{len(codes) - 1}\x00"

    text = escape(re.sub(r"`([^`]+)`", set_aside, text))
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    return re.sub(r"\x00(\d+)\x00",
                  lambda m: f'<font name="Mono" size="8" backColor="{INLINE_CODE_BG}">'
                            f"{escape(codes[int(m.group(1))])}</font>", text)


# ---------------------------------------------------------------------------
# Block builders
# ---------------------------------------------------------------------------


def heading_two(text: str) -> list:
    rule = HRFlowable(width="100%", thickness=1.4, color=NAVY, spaceBefore=2, spaceAfter=6)
    rule.keepWithNext = 1  # never leave a heading and its rule alone at the bottom of a page
    return [Paragraph(inline(text), H2), rule]


def code_block(lines: list[str]) -> Table:
    # A one-cell table gives the code its background box and border.
    box = Table([[Preformatted("\n".join(lines), CODE)]], colWidths=[TEXT_W])
    box.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
        ("BOX", (0, 0), (-1, -1), 0.6, CELL_LINE),
        ("LEFTPADDING", (0, 0), (-1, -1), 8), ("RIGHTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 6), ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
    ]))
    return box


CELL_PADDING = 10  # left + right padding of a table cell, in points


def longest_word_width(cells: list[str]) -> float:
    """Width of the longest single word in a column, so no word is ever broken mid-word."""
    words = [w for c in cells for w in re.sub(r"[*`]", "", c).split()]
    widest = max((stringWidth(w, "Body-Bold", CELL.fontSize) for w in words), default=0)
    return widest + CELL_PADDING + 2


def column_widths(rows: list[list[str]], key_value: bool) -> list[float]:
    """Split the page width in proportion to each column's longest cell (capped, so one long
    cell doesn't squeeze every other column). Then make sure every column is at least as wide as
    its longest word, taking the extra space from the columns that have room to spare."""
    if key_value:
        return [TEXT_W * 0.22, TEXT_W * 0.78]
    ncols = max(len(r) for r in rows)
    columns = [[r[c] for r in rows if c < len(r)] for c in range(ncols)]
    weights = [min(max(max((len(x) for x in col), default=1), 6), 70) for col in columns]
    widths = [TEXT_W * w / sum(weights) for w in weights]
    floors = [longest_word_width(col) for col in columns]

    for _ in range(ncols):  # each pass fixes at least one column, so ncols passes are enough
        short = [c for c in range(ncols) if widths[c] < floors[c]]
        if not short:
            break
        needed = sum(floors[c] - widths[c] for c in short)
        for c in short:
            widths[c] = floors[c]
        donors = [c for c in range(ncols) if c not in short]
        spare = sum(widths[c] - floors[c] for c in donors)
        for c in donors:  # take space in proportion to how much each donor can give
            widths[c] -= needed * (widths[c] - floors[c]) / spare
    return widths


def table(header: list[str], rows: list[list[str]]) -> Table:
    key_value = all(h == "" for h in header)
    body_rows = [[Paragraph(inline(c), CELL_KEY if key_value and i == 0 else CELL)
                  for i, c in enumerate(r)] for r in rows]
    data = body_rows if key_value else [[Paragraph(inline(h), CELL_HEAD) for h in header]] + body_rows
    t = Table(data, colWidths=column_widths([header] + rows, key_value),
              repeatRows=0 if key_value else 1)  # repeat the header on every page it spans
    first = 0 if key_value else 1
    style = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 3.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 3.5),
        ("LINEBELOW", (0, first), (-1, -1), 0.5, CELL_LINE),
    ]
    if not key_value:
        style.append(("BACKGROUND", (0, 0), (-1, 0), NAVY))
    for r in range(first, len(data)):
        if (r - first) % 2 == 1:
            style.append(("BACKGROUND", (0, r), (-1, r), ZEBRA))
    t.setStyle(TableStyle(style))
    t.spaceBefore, t.spaceAfter = 4, 8
    return t


def bullet_list(items: list[str], ordered: bool) -> ListFlowable:
    return ListFlowable(
        [ListItem(Paragraph(inline(t), LIST_ITEM), leftIndent=16) for t in items],
        bulletType="1" if ordered else "bullet", start="1" if ordered else "\u2022",
        bulletFormat="%s." if ordered else None, bulletFontName="Body", bulletFontSize=9,
        leftIndent=16, bulletColor=NAVY, spaceBefore=2, spaceAfter=4)


# ---------------------------------------------------------------------------
# Markdown -> flowables
# ---------------------------------------------------------------------------

LIST_START = re.compile(r"(- |\d+\. )")
BLOCK_START = re.compile(r"(#|```|\||- |\d+\. |---|!\[)")
IMAGE = re.compile(r"!\[(.*)\]\((.+)\)\s*$")


def image(path: Path, caption: str) -> KeepTogether:
    """An image scaled to the text width (never enlarged), with its caption underneath."""
    picture = Image(str(path))
    scale = min(1.0, TEXT_W / picture.imageWidth)
    picture.drawWidth = picture.imageWidth * scale
    picture.drawHeight = picture.imageHeight * scale
    return KeepTogether([Spacer(1, 4), picture, Paragraph(inline(caption), CAPTION), Spacer(1, 6)])


def split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def parse(md: str, base: Path) -> list:
    """`base`: the folder the Markdown file is in, which image paths are relative to."""
    lines, story, i = md.split("\n"), [], 0
    previous_was_h1 = False
    while i < len(lines):
        line = lines[i]
        is_h1 = False
        if line.startswith("```"):
            i += 1
            block = []
            while i < len(lines) and not lines[i].startswith("```"):
                block.append(lines[i])
                i += 1
            story.append(code_block(block))
            story.append(Spacer(1, 4))
            i += 1
        elif m := IMAGE.match(line):
            story.append(image(base / m.group(2), m.group(1)))
            i += 1
        elif line.startswith("### "):
            story.append(Paragraph(inline(line[4:]), H3))
            i += 1
        elif line.startswith("## "):
            story.extend(heading_two(line[3:]))
            i += 1
        elif line.startswith("# "):
            story.append(Paragraph(inline(line[2:]), H1))
            is_h1 = True
            i += 1
        elif line.strip() == "---":
            story.append(HRFlowable(width="100%", thickness=0.6, color=RULE, spaceBefore=6,
                                    spaceAfter=4))
            i += 1
        elif line.startswith("|"):
            header = split_row(line)
            i += 2  # skip the |---| separator row
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append(split_row(lines[i]))
                i += 1
            story.append(table(header, rows))
        elif LIST_START.match(line):
            ordered = bool(re.match(r"\d+\. ", line))
            items = []
            while i < len(lines) and (LIST_START.match(lines[i]) or
                                      (items and lines[i].startswith("  "))):
                if LIST_START.match(lines[i]):
                    items.append(LIST_START.sub("", lines[i], count=1))
                else:
                    items[-1] += " " + lines[i].strip()  # a wrapped continuation line
                i += 1
            story.append(bullet_list(items, ordered))
        elif line.strip() == "":
            i += 1
            continue
        else:
            para = [line]
            i += 1
            while i < len(lines) and lines[i].strip() and not BLOCK_START.match(lines[i]):
                para.append(lines[i])
                i += 1
            style = SUBTITLE if previous_was_h1 else BODY  # the line right under the title
            story.append(Paragraph(inline(" ".join(para)), style))
        previous_was_h1 = is_h1
    return story


def footer(canvas, doc) -> None:
    canvas.saveState()
    canvas.setFont("Body", 7.5)
    canvas.setFillColor(GREY_TEXT)
    canvas.drawString(MARGIN, 9 * mm, doc.title)
    canvas.drawRightString(PAGE_W - MARGIN, 9 * mm, f"Page {doc.page}")
    canvas.restoreState()


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    md = src.read_text(encoding="utf-8")
    title_match = re.search(r"^# (.+)$", md, flags=re.MULTILINE)
    title = title_match.group(1) if title_match else src.stem

    doc = SimpleDocTemplate(str(dst), pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN,
                            topMargin=16 * mm, bottomMargin=18 * mm, title=title,
                            author="NanoBook", subject=title)
    doc.build(parse(md, src.parent), onFirstPage=footer, onLaterPages=footer)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
