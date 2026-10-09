#!/usr/bin/env python3
"""
Build the static pages of the apollo-patches GitHub Pages site.

    build-site.py <apollo-patches-dir> <output-dir> [--base=URL] [--patcher=URL]

One plain HTML page per patch, one listing per platform, a landing page, a
sitemap and a 404. They exist for search engines and for anyone without
JavaScript: the interactive view of a patch is the web patcher, and every page
links straight into it (?patch=PS3/BLUS30490). Nothing here is committed to
apollo-patches -- its Pages workflow runs this on every push, so the site can
never fall behind the patches.

Old links keep working with no redirect. The Jekyll site this replaces served
PS3/BCES00001.md as PS3/BCES00001.html, which is the same path written here,
and GitHub Pages answers /PS3/BCES00001 with it as well.

The patch reading is shared with build-index.py (the game name, its
encodings). The code list follows source/loader.c in apollo-lib, closely
enough to show the same codes, groups and types the engine would -- but it is
a listing, not a second engine: what a code DOES is only ever decided there.
"""
import html
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "build_index", os.path.join(HERE, "build-index.py"))
build_index = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build_index)

PLATFORMS = build_index.PLATFORMS
PLATFORM_NAMES = {"PS3": "PlayStation 3", "PS4": "PlayStation 4",
                  "PSV": "PlayStation Vita", "PSP": "PlayStation Portable",
                  "PS2": "PlayStation 2", "PS1": "PlayStation"}
# What each console app downloads (ONLINE_PATCH_URL in its settings.c). The
# workflow builds these next to the pages; PS2 patches ride in PS3's and PS4's.
UPDATE_PACKS = {"PS3": "apollo-ps3-update.zip", "PS4": "apollo-ps4-update.zip",
                "PSV": "apollo-vita-update.zip", "PSP": "apollo-psp-update.zip"}

REPO = "https://github.com/bucanero/apollo-patches"
RAW = "https://cdn.jsdelivr.net/gh/bucanero/apollo-patches@main"

# The loader's headers and body terminators (source/loader.c). Lines are
# right-trimmed only, as the loader does: an indented "[x]" is not a header.
RE_HEADER = re.compile(r"^(\[.*\]|; --- .* ---|(?i:group:).*)$", re.S)
RE_OPTION = re.compile(r"^\{.*\}.*\{/.*\}$", re.S)
RE_PATH = re.compile(r"^path:", re.I)
RE_SW_LINE = re.compile(r"^.{8} .{8}$", re.S)


def decode(raw):
    for encoding in ("utf-8", "cp1252"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1")


def parse_patch(text):
    """(notes, items). items are dicts: kind 'group' with 'codes', or 'code'.

    notes is the comment block under the game name -- credits and usage notes,
    which the engine ignores and readers want."""
    lines = [l.rstrip() for l in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]

    notes = []
    for line in lines[2:]:
        if not line.startswith(";"):
            break
        note = line.lstrip(";").strip()
        if note:
            notes.append(note)

    items, group, code, target = [], None, None, ""

    def close_code():
        nonlocal code
        if code is None:
            return
        body = code.pop("lines")
        if code["type"] is None:
            # Save Wizard only when EVERY line has the SW shape; else BSD.
            sw = body and all(RE_SW_LINE.match(l) for l in body)
            code["type"] = "sw" if sw else "bsd"
        code["body"] = "\n".join(body)
        (group["codes"] if group else items).append(code)
        code = None

    for line in lines:
        if not line:
            continue
        is_header = RE_HEADER.match(line)
        if code is not None and not (is_header or line.startswith(":")
                                     or RE_OPTION.match(line) or RE_PATH.match(line)):
            if not line.startswith(";"):
                code["lines"].append(line.lstrip())
            continue
        close_code()

        if line.startswith(":"):
            target = line[1:].strip()
            continue
        if not is_header:
            continue

        lower = line.lower()
        if re.match(r"^.*group:\\", lower):
            group = None                   # [GROUP:\] closes the open group
            name = line[line.rindex("\\") + 1:].rstrip("]").strip()
            if not name:
                continue
        elif lower.startswith("[group:") or lower.startswith("group:"):
            name = line.split(":", 1)[1].rstrip("]").strip()
            group = {"kind": "group", "name": name, "codes": []}
            items.append(group)
            continue
        elif line.startswith("; --- "):
            # The loader cuts every title at its last ']', group or not.
            name = line[6:-4]
            group = {"kind": "group", "name": name[:name.rindex("]")].strip()
                     if "]" in name else name.strip(), "codes": []}
            items.append(group)
            continue
        else:
            name = line[1:]
            if "]" in name:
                name = name[:name.rindex("]")]

        flags, ctype = set(), None
        while True:
            m = re.match(r"^(default|info|python|sw|bsd|le|be)\s*:", name, re.I)
            if not m:
                break
            tag = m.group(1).lower()
            name = name[m.end():]
            if tag in ("python", "sw", "bsd"):
                ctype = tag
            elif tag in ("le", "be"):
                flags.discard("le"); flags.discard("be"); flags.add(tag)
            else:
                flags.add(tag)
        name = name.strip()
        if "(required)" in name.lower():
            flags.add("required")
        code = {"kind": "code", "name": name or "(untitled)", "type": ctype,
                "flags": flags, "file": target, "lines": []}
    close_code()
    return notes, items


def count_codes(items):
    return sum(len(i["codes"]) if i["kind"] == "group" else 1 for i in items)


# ---------------------------------------------------------------------------
# HTML

E = html.escape

CSS = """\
:root{--bg:#fafafa;--fg:#1d1d1f;--muted:#6e6e73;--line:#d9d9de;--card:#fff;
--accent:#2357d9;--code:#f2f2f5;--req:#b4410a;color-scheme:light}
@media (prefers-color-scheme:dark){:root{--bg:#141416;--fg:#ececf0;--muted:#9a9aa3;
--line:#2c2c31;--card:#1c1c20;--accent:#7aa2ff;--code:#232328;--req:#ff9a5c;color-scheme:dark}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--accent)}
.wrap{max-width:960px;margin:0 auto;padding:0 16px}
header.top{border-bottom:1px solid var(--line);padding:12px 0;margin-bottom:24px}
header.top .wrap{display:flex;gap:16px;align-items:center;flex-wrap:wrap}
header.top a.home{font-weight:600;text-decoration:none;color:var(--fg)}
nav.crumbs{color:var(--muted);font-size:14px}
h1{font-size:28px;line-height:1.2;margin:0 0 4px}
h2{font-size:19px;margin:28px 0 8px}
.meta{color:var(--muted);margin:0 0 16px}
.notes{color:var(--muted);font-size:14px;margin:0 0 16px;padding-left:18px}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 24px}
.btn{display:inline-block;padding:8px 14px;border:1px solid var(--line);border-radius:8px;
background:var(--card);text-decoration:none;color:var(--fg)}
.btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}
details{background:var(--card);border:1px solid var(--line);border-radius:8px;margin:6px 0}
summary{padding:8px 12px;cursor:pointer;display:flex;gap:8px;align-items:baseline;flex-wrap:wrap}
summary .name{flex:1 1 auto;min-width:0;overflow-wrap:anywhere}
.badge{font-size:12px;padding:1px 6px;border-radius:4px;border:1px solid var(--line);
color:var(--muted);white-space:nowrap}
.badge.req{color:var(--req);border-color:var(--req)}
.target{margin:0 12px 6px;color:var(--muted);font-size:13px;overflow-wrap:anywhere}
pre{margin:0 12px 12px;padding:10px;background:var(--code);border-radius:6px;overflow:auto;
font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
section.group{border-left:3px solid var(--line);padding-left:12px;margin:16px 0}
section.group h2{margin-top:8px}
table{width:100%;border-collapse:collapse}
td{padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}
td.id{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:14px;white-space:nowrap}
input[type=search]{width:100%;padding:10px 12px;font:inherit;border:1px solid var(--line);
border-radius:8px;background:var(--card);color:var(--fg);margin:0 0 12px}
.plats{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;margin:0 0 24px}
.plat{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.plat h3{margin:0 0 4px;font-size:17px}
.plat p{margin:0 0 6px;color:var(--muted);font-size:14px}
footer{color:var(--muted);font-size:13px;border-top:1px solid var(--line);margin-top:40px;padding:16px 0}
"""

FILTER_JS = """\
<script>
document.getElementById('q').addEventListener('input',function(e){
  var q=e.target.value.trim().toLowerCase();
  document.querySelectorAll('#games tr').forEach(function(r){
    r.hidden=q&&r.textContent.toLowerCase().indexOf(q)<0;});
});
</script>"""


def page(title, body, description, canonical, root):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{E(title)}</title>
<meta name="description" content="{E(description)}">
<link rel="canonical" href="{E(canonical)}">
<link rel="stylesheet" href="{root}style.css">
</head>
<body>
<header class="top"><div class="wrap">
<a class="home" href="{root}">Apollo Save Database</a>
</div></header>
<main class="wrap">
{body}
</main>
<footer><div class="wrap">
Part of <a href="https://github.com/bucanero/apollo-ps3">Apollo Save Tool</a>.
Patches are <a href="{REPO}">on GitHub</a> under the
<a href="{REPO}/blob/main/LICENSE">GPL-3.0 license</a>.
</div></footer>
</body>
</html>
"""


def render_code(code):
    badges = []
    if "required" in code["flags"]:
        badges.append('<span class="badge req">Required</span>')
    badges.append(f'<span class="badge">{ {"sw": "Save Wizard", "bsd": "BSD", "python": "Python"}[code["type"]] }</span>')
    name = re.sub(r"\s*\(required\)", "", code["name"], flags=re.I) or code["name"]
    out = [f'<details><summary><span class="name">{E(name)}</span>{"".join(badges)}</summary>']
    if code["file"]:
        out.append(f'<p class="target">File: <code>{E(code["file"])}</code></p>')
    if code["body"]:
        out.append(f'<pre>{E(code["body"])}</pre>')
    out.append("</details>")
    return "".join(out)


def render_patch(platform, title_id, name, alt, notes, items, cfg):
    total = count_codes(items)
    open_url = f'{cfg["patcher"]}?patch={platform}/{title_id}'
    parts = [
        f'<nav class="crumbs"><a href="../">Home</a> / '
        f'<a href="./">{E(PLATFORM_NAMES[platform])}</a></nav>',
        f"<h1>{E(name)}</h1>",
        f'<p class="meta">{E(title_id)} · {platform} · {total} code{"s" if total != 1 else ""}</p>',
    ]
    if alt:
        parts.append(f'<p class="meta">Also covers: {E(", ".join(alt))}</p>')
    shown = [n for n in notes if n != name and n != title_id][:8]
    if shown:
        parts.append('<ul class="notes">' + "".join(f"<li>{E(n)}</li>" for n in shown) + "</ul>")
    parts.append(
        '<p class="actions">'
        f'<a class="btn primary" href="{E(open_url)}">Open in Apollo Patcher</a>'
        f'<a class="btn" href="{RAW}/{platform}/{title_id}.savepatch">Download .savepatch</a>'
        f'<a class="btn" href="{REPO}/blob/main/{platform}/{title_id}.savepatch">View on GitHub</a>'
        "</p>")

    for item in items:
        if item["kind"] == "group":
            parts.append(f'<section class="group"><h2>{E(item["name"])}</h2>')
            parts.extend(render_code(c) for c in item["codes"])
            parts.append("</section>")
        else:
            parts.append(render_code(item))

    names = [c["name"] for i in items for c in (i["codes"] if i["kind"] == "group" else [i])]
    desc = f"{total} save game patches for {name} ({title_id}, {PLATFORM_NAMES[platform]})"
    if names:
        desc += ": " + ", ".join(names[:6])
    return page(f"{name} ({title_id}) save patches — Apollo",
                "\n".join(parts), desc[:300],
                f'{cfg["base"]}/{platform}/{title_id}.html', "../")


def render_platform(platform, rows, cfg):
    trs = "\n".join(
        f'<tr><td><a href="{tid}.html">{E(name)}</a></td><td class="id">{tid}</td></tr>'
        for tid, name in rows)
    pack = UPDATE_PACKS.get(platform)
    pack_line = (f' The Apollo app downloads all of them at once as '
                 f'<a href="{pack}">{pack}</a>.' if pack else "")
    body = f"""<nav class="crumbs"><a href="../">Home</a></nav>
<h1>{E(PLATFORM_NAMES[platform])} save patches</h1>
<p class="meta">{len(rows)} games.{pack_line}</p>
<input type="search" id="q" placeholder="Filter by game or title ID" aria-label="Filter">
<table><tbody id="games">
{trs}
</tbody></table>
{FILTER_JS}"""
    return page(f"{PLATFORM_NAMES[platform]} save patches — Apollo", body,
                f"{len(rows)} {PLATFORM_NAMES[platform]} save game patches for Apollo Save Tool",
                f'{cfg["base"]}/{platform}/', "../")


def render_home(counts, cfg):
    cards = []
    for p in PLATFORMS:
        if p not in counts:
            continue
        pack = UPDATE_PACKS.get(p)
        cards.append(
            f'<div class="plat"><h3><a href="{p}/">{E(PLATFORM_NAMES[p])}</a></h3>'
            f'<p>{counts[p]} games</p>'
            + (f'<p><a href="{p}/{pack}">{pack}</a></p>' if pack else "")
            + "</div>")
    total = sum(counts.values())
    body = f"""<h1>Apollo Save Database</h1>
<p class="meta">{total} save game patches for
<a href="https://github.com/bucanero/apollo-ps3">Apollo Save Tool</a> on
PS1, PS2, PS3, PS4, PSP and PS Vita.</p>
<p class="actions">
<a class="btn primary" href="{E(cfg["patcher"])}?browse">Search and apply patches in the browser</a>
<a class="btn" href="{REPO}">Contribute on GitHub</a>
</p>
<div class="plats">
{"".join(cards)}
</div>"""
    return page("Apollo Save Database", body,
                f"{total} save game patches for Apollo Save Tool on PS1, PS2, PS3, PS4, PSP and PS Vita",
                f'{cfg["base"]}/', "./")


def render_404(cfg):
    body = f"""<h1>Page not found</h1>
<p>The patch may have been renamed. <a href="{E(cfg["patcher"])}?browse">Search the
database</a> or start from the <a href="{cfg["base"]}/">home page</a>.</p>"""
    # Absolute stylesheet: a 404 is served at whatever path was asked for.
    return page("Not found — Apollo Save Database", body, "Page not found",
                f'{cfg["base"]}/', cfg["base"] + "/")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    cfg = {"base": "https://bucanero.github.io/apollo-patches",
           "patcher": "https://bucanero.github.io/apollo-patcher/"}
    for flag in (a for a in argv[1:] if a.startswith("--")):
        key, _, value = flag[2:].partition("=")
        if key not in cfg or not value:
            sys.exit(f"unknown option: {flag}")
        cfg[key] = value
    cfg["base"] = cfg["base"].rstrip("/")
    if len(args) != 2:
        sys.exit(f"usage: {os.path.basename(argv[0])} <apollo-patches-dir> <output-dir> "
                 "[--base=URL] [--patcher=URL]")
    root, out = args

    counts, urls, ncodes = {}, [f'{cfg["base"]}/'], 0
    for platform in PLATFORMS:
        directory = os.path.join(root, platform)
        if not os.path.isdir(directory):
            continue
        rows = []
        for entry in sorted(os.listdir(directory)):
            if not entry.endswith(".savepatch"):
                continue
            title_id = entry[: -len(".savepatch")]
            path = os.path.join(directory, entry)
            named = build_index.read_game_name(path)
            name, alt = named if named else (title_id, [])
            with open(path, "rb") as fh:
                notes, items = parse_patch(decode(fh.read()))
            ncodes += count_codes(items)
            write(os.path.join(out, platform, f"{title_id}.html"),
                  render_patch(platform, title_id, name, alt, notes, items, cfg))
            rows.append((title_id, name))
            urls.append(f'{cfg["base"]}/{platform}/{title_id}.html')
        if not rows:
            continue
        rows.sort(key=lambda r: (r[1].lower(), r[0]))
        counts[platform] = len(rows)
        write(os.path.join(out, platform, "index.html"), render_platform(platform, rows, cfg))
        urls.append(f'{cfg["base"]}/{platform}/')

    write(os.path.join(out, "index.html"), render_home(counts, cfg))
    write(os.path.join(out, "404.html"), render_404(cfg))
    write(os.path.join(out, "style.css"), CSS)
    write(os.path.join(out, "sitemap.xml"),
          '<?xml version="1.0" encoding="UTF-8"?>\n'
          '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
          + "".join(f"<url><loc>{E(u)}</loc></url>\n" for u in urls)
          + "</urlset>\n")

    total = sum(counts.values())
    detail = " ".join(f"{k}:{v}" for k, v in counts.items())
    print(f"{out}: {total} patch pages ({detail}), {ncodes} codes")


if __name__ == "__main__":
    main(sys.argv)
