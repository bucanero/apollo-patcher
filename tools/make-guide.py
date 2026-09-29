#!/usr/bin/env python3
"""
Render docs/user-guide.md into a page for the static site.

Why a converter here rather than a markdown library or a client-side renderer:
the site has no npm and no bundler -- dist/ is exactly what gets published --
and adding a Python dependency would mean the Pages workflow could break on
something nobody in this repo controls. python3 is already required for
build-index.py, so this costs nothing new.

The same reasoning as core/png.c: the job is small and THE INPUT IS NARROW.
This does not implement markdown. It implements the subset docs/user-guide.md
actually uses, and it FAILS on anything else rather than quietly emitting the
wrong thing -- see the unsupported() calls. If the guide grows a construct this
does not handle, the build stops and says which line.

What is supported: ATX headings, paragraphs, unordered and ordered lists,
pipe tables with a header row, fenced code blocks, blockquotes, thematic
breaks, and inline links, bold, italic and code.

Heading ids match GitHub's own slug rules, so the guide's in-page Contents
links work identically on the site and in the repo, and a link somebody copied
from one lands in the same place on the other.
"""
import html
import os
import re
import sys

# ---------------------------------------------------------------- inline ----

# Code spans are lifted out FIRST and put back LAST, via a placeholder that
# cannot occur in the source (NUL is not valid in the input). Two reasons:
# a `*` or `<` inside a code span must never be read as markup, and bold that
# WRAPS a code span -- "**single `.PSV` file**" -- has to still be seen as one
# bold run. Substituting code inline instead of via a placeholder splits that
# run in half and leaves the asterisks on the page, which is exactly the bug
# this shape avoids.
_CODE = re.compile(r'`([^`]+)`')
# Images before links: ![alt](src) contains [alt](src), so the link pattern
# would match the inside of one and leave a stray '!' on the page.
_IMG  = re.compile(r'!\[([^\]]*)\]\(([^)\s]+)\)')
_LINK = re.compile(r'\[([^\]]*)\]\(([^)\s]+)\)')
_BOLD = re.compile(r'\*\*(.+?)\*\*', re.S)
_ITAL = re.compile(r'\*([^*\n]+)\*')
_ONLY_IMG  = re.compile(r'!\[[^\]]*\]\([^)\s]+\)')
_ONLY_ITAL = re.compile(r'\*[^*]+\*')

REPO_DOCS = 'https://github.com/bucanero/apollo-patcher/blob/main/docs/'


def inline(text):
    """Markdown inline -> HTML, escaping everything that is not markup."""
    codes = []

    def stash(m):
        codes.append(m.group(1))
        return f'\x00{len(codes) - 1}\x00'

    t = _CODE.sub(stash, text)

    # Escape before putting markup back, so anything the author did not write
    # as markdown -- <target>, A & B -- survives as itself.
    t = html.escape(t)

    def link(m):
        href = m.group(2)
        # A .md target is a sibling document in the repo; on the site the guide
        # stands alone, so those point back at the repo rather than 404ing.
        if href.endswith('.md') or '.md#' in href:
            href = REPO_DOCS + href.lstrip('./').replace('../', '')
        ext = ' target="_blank" rel="noopener"' if href.startswith('http') else ''
        return f'<a href="{html.escape(href, quote=True)}"{ext}>{m.group(1)}</a>'

    def image(m):
        # Paths are relative to docs/, and the Makefile copies docs/images/ to
        # dist/images/, so the same `images/...` path is correct in the repo
        # and on the site. Nothing to rewrite.
        return (f'<img src="{html.escape(m.group(2), quote=True)}" '
                f'alt="{m.group(1)}" loading="lazy">')

    t = _IMG.sub(image, t)
    t = _LINK.sub(link, t)
    # Bold before italic: what is left over as a single `*` pair once the `**`
    # pairs are gone is an italic run, including one nested inside a bold one.
    t = _BOLD.sub(r'<strong>\1</strong>', t)
    t = _ITAL.sub(r'<em>\1</em>', t)

    for k, code in enumerate(codes):
        t = t.replace(f'\x00{k}\x00', '<code>' + html.escape(code) + '</code>')
    return t




def slug(text):
    """GitHub's heading anchor: strip formatting and punctuation, space -> '-'.

    Consecutive spaces become consecutive hyphens, which is why an em dash in a
    heading yields a double hyphen. Matching that exactly is the whole point --
    the guide's Contents links are written against it.
    """
    s = re.sub(r'`([^`]*)`', r'\1', text)
    s = re.sub(r'\*\*?([^*]*)\*\*?', r'\1', s)
    s = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', s)
    s = s.strip().lower()
    s = re.sub(r'[^a-z0-9 _-]', '', s)
    return s.replace(' ', '-')


# ----------------------------------------------------------------- block ----

def unsupported(lineno, line):
    sys.exit(f"make-guide.py: line {lineno}: unsupported markdown\n"
             f"  {line.rstrip()}\n"
             f"  This renderer handles only what the guide uses. Either\n"
             f"  rewrite that line, or teach the renderer the construct.")


def render(md):
    lines = md.split('\n')
    out, i = [], 0
    n = len(lines)

    while i < n:
        raw = lines[i]
        line = raw.rstrip()
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        # Fenced code.
        if stripped.startswith('```'):
            i += 1
            body = []
            while i < n and not lines[i].strip().startswith('```'):
                body.append(lines[i])
                i += 1
            if i >= n:
                unsupported(i, '``` (unterminated fence)')
            i += 1
            out.append('<pre><code>' + html.escape('\n'.join(body)) + '</code></pre>')
            continue

        # Thematic break.
        if re.fullmatch(r'-{3,}|\*{3,}|_{3,}', stripped):
            out.append('<hr>')
            i += 1
            continue

        # Heading.
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            lvl, text = len(m.group(1)), m.group(2).strip()
            out.append(f'<h{lvl} id="{html.escape(slug(text), quote=True)}">'
                       f'{inline(text)}</h{lvl}>')
            i += 1
            continue

        # Table: a pipe row followed by an alignment row.
        if stripped.startswith('|') and i + 1 < n and \
                re.fullmatch(r'\|[\s:|-]+\|', lines[i + 1].strip()):
            head = _cells(lines[i])
            aligns = [_align(c) for c in _cells(lines[i + 1])]
            i += 2
            rows = []
            while i < n and lines[i].strip().startswith('|'):
                rows.append(_cells(lines[i]))
                i += 1
            out.append(_table(head, aligns, rows))
            continue

        # Blockquote.
        if stripped.startswith('>'):
            body = []
            while i < n and lines[i].strip().startswith('>'):
                body.append(re.sub(r'^\s*>\s?', '', lines[i]))
                i += 1
            out.append('<blockquote><p>' + inline(' '.join(
                s.strip() for s in body if s.strip())) + '</p></blockquote>')
            continue

        # Lists. A continuation line is indented; a blank line inside a list
        # keeps it open only if the next non-blank line is still part of it.
        m = re.match(r'^(\s*)([-*]|\d+\.)\s+(.*)$', line)
        if m:
            ordered = m.group(2)[0].isdigit()
            items, i = _list_items(lines, i, n)
            tag = 'ol' if ordered else 'ul'
            out.append(f'<{tag}>')
            for it in items:
                out.append('<li>' + inline(it) + '</li>')
            out.append(f'</{tag}>')
            continue

        # Anything starting with whitespace that is not a list continuation is
        # an indented code block in real markdown; the guide has none, and
        # treating it as a paragraph would silently lose the indent.
        if raw.startswith('    '):
            unsupported(i + 1, raw)

        # Paragraph: consume until a blank line or the start of another block.
        para = []
        while i < n and lines[i].strip() and not _starts_block(lines, i):
            para.append(lines[i].strip())
            i += 1
        text = ' '.join(para)

        # A paragraph that is nothing but an image is a figure, and an
        # all-italic paragraph straight after it is its caption. Emitting
        # <p><img></p> followed by <p><em></em></p> would leave the two
        # unrelated in the markup, so the caption could not be styled as one
        # without :has() -- and would not be a caption to a screen reader
        # either.
        if _ONLY_IMG.fullmatch(text):
            figure = ['<figure>', inline(text)]
            j = i
            while j < n and not lines[j].strip():
                j += 1
            if j < n and not _starts_block(lines, j):
                cap = []
                k = j
                while k < n and lines[k].strip() and not _starts_block(lines, k):
                    cap.append(lines[k].strip())
                    k += 1
                caption = ' '.join(cap)
                if _ONLY_ITAL.fullmatch(caption):
                    figure.append('<figcaption>'
                                  + inline(caption[1:-1]) + '</figcaption>')
                    i = k
            figure.append('</figure>')
            out.append('\n'.join(figure))
            continue

        out.append('<p>' + inline(text) + '</p>')

    return '\n'.join(out)


def _starts_block(lines, i):
    s = lines[i].strip()
    return (s.startswith(('```', '>', '|', '#'))
            or re.fullmatch(r'-{3,}|\*{3,}|_{3,}', s)
            or re.match(r'^(\s*)([-*]|\d+\.)\s+', lines[i]))


def _list_items(lines, i, n):
    """Collect one list's items, joining wrapped continuation lines."""
    items, cur = [], None
    while i < n:
        line = lines[i]
        s = line.strip()
        if not s:
            # A blank line ends the list unless the next line continues an item.
            if i + 1 < n and re.match(r'^(\s*)([-*]|\d+\.)\s+', lines[i + 1]):
                i += 1
                continue
            break
        m = re.match(r'^(\s*)([-*]|\d+\.)\s+(.*)$', line)
        if m:
            if cur is not None:
                items.append(' '.join(cur))
            cur = [m.group(3).strip()]
            i += 1
            continue
        if line.startswith(('  ', '\t')) and cur is not None:
            cur.append(s)          # wrapped continuation of the current item
            i += 1
            continue
        break
    if cur is not None:
        items.append(' '.join(cur))
    return items, i


def _cells(row):
    return [c.strip() for c in row.strip().strip('|').split('|')]


def _align(spec):
    left, right = spec.startswith(':'), spec.endswith(':')
    return 'center' if left and right else 'right' if right else ''


def _table(head, aligns, rows):
    def td(tag, cell, idx):
        a = aligns[idx] if idx < len(aligns) else ''
        style = f' style="text-align:{a}"' if a else ''
        return f'<{tag}{style}>{inline(cell)}</{tag}>'

    out = ['<div class="table-wrap"><table>', '<thead><tr>']
    out += [td('th', c, k) for k, c in enumerate(head)]
    out.append('</tr></thead><tbody>')
    for r in rows:
        out.append('<tr>' + ''.join(td('td', c, k) for k, c in enumerate(r)) + '</tr>')
    out.append('</tbody></table></div>')
    return '\n'.join(out)


# ------------------------------------------------------------------ page ----

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<meta name="description" content="{desc}">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='13' font-size='14'>&#127918;</text></svg>">
<link rel="stylesheet" href="./style.css">
<link rel="stylesheet" href="./guide.css">
</head>
<body>

<header class="topbar">
  <div class="topbar-row">
    <h1>{title}</h1>
    <div class="topbar-links">
      <a class="gh-button" href="./index.html">Web patcher</a>
      <a class="gh-button" href="https://github.com/bucanero/apollo-patcher"
         target="_blank" rel="noopener">GitHub</a>
    </div>
  </div>
</header>

<main class="guide">
{body}
</main>

</body>
</html>
"""


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: make-guide.py INPUT.md OUTPUT.html")
    src, dst = argv[1], argv[2]
    md = open(src, encoding='utf-8').read()

    # The document's own H1 becomes the page title and the topbar heading, so
    # it is not also emitted into the body.
    m = re.match(r'^#\s+(.*)$', md.split('\n')[0])
    title = m.group(1).strip() if m else 'Apollo Save Patcher'
    if m:
        md = '\n'.join(md.split('\n')[1:])

    body = render(md)

    # Anything left over means a construct was not recognised and the page
    # would show raw markdown. Cheap to check, and the whole reason this
    # renderer is allowed to be a subset: it is never silently wrong.
    for token, what in (('**', 'bold'), ('`', 'code span'), ('\x00', 'placeholder')):
        if token in body:
            where = body[max(0, body.index(token) - 90):body.index(token) + 90]
            sys.exit(f"make-guide.py: unconverted {what} left in the output:\n"
                     f"  ...{where}...")

    ids = set(re.findall(r'id="([^"]+)"', body))
    dangling = [a for a in re.findall(r'href="#([^"]+)"', body) if a not in ids]
    if dangling:
        sys.exit("make-guide.py: in-page links with no matching heading: "
                 + ', '.join(sorted(set(dangling))))

    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    with open(dst, 'w', encoding='utf-8') as fh:
        fh.write(PAGE.format(
            title=html.escape(title),
            desc="Using the Apollo Save Patcher desktop app: patching a save, "
                 "the settings, and what to do when something goes wrong.",
            body=body))
    print(f"wrote {dst} ({len(body.splitlines())} blocks from {src})")


if __name__ == '__main__':
    main(sys.argv)
