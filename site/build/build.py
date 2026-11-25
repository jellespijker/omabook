#!/usr/bin/env python3
"""Builds the static pages of site/ (standard library only).

    python3 site/build/build.py            # registry, then GitHub repositories with the topic omabook-plugin
    python3 site/build/build.py --offline  # registry only

The words of the product page, the documentation and the help are fragments in site/build/pages/.
The plugins page is made from share/omabook/plugins.json (the registry Omabook is built with) and,
unless --offline, from GitHub. Nothing runs in the visitor's browser but theme.js, which remembers
the theme preview in that browser and sends nothing anywhere.
"""

import datetime
import html
import json
import os
import sys
import urllib.error
import urllib.request

import seo
from markdown import to_markdown

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.dirname(HERE)
ROOT = os.path.dirname(SITE)
VERSION = seo.project_version(ROOT)
E = html.escape
PAGES = []

NAVS = [("", "Product"), ("docs/", "Docs"), ("help/", "Help"), ("plugins.html", "Plugins")]
DOCS = [
    ("getting-started", "Getting started"),
    ("library", "The library"),
    ("knowledge-graph", "The knowledge graph"),
    ("ai", "AI setup, per capability"),
    ("plugins", "Plugins"),
    ("calibre", "Calibre"),
    ("phones-and-e-readers", "Phones and e-readers"),
    ("sync", "Sync and sharing"),
    ("agents", "For agents"),
    ("omabook-vs-calibre", "Omabook and Calibre"),
]
HELPS = [("shortcuts", "Shortcuts"), ("troubleshooting", "Troubleshooting"), ("faq", "Questions")]
THEMES = [
    ("auto", "System"),
    ("tokyo", "Tokyo Night"),
    ("gruvbox", "Gruvbox"),
    ("latte", "Catppuccin Latte"),
    ("forest", "Everforest"),
]
DESCRIPTION = "Omabook is a free ebook reader and library for Linux and Omarchy, with a knowledge graph of everything you read."
DESCS = {
    "docs/getting-started.html": "Install Omabook on Arch or Omarchy, add your books, and read them. "
    "Free ebook reader and library for Linux.",
    "docs/library.html": "Omabook's library: five views, filters, search, nested collections, and a reader that sets text your way.",
    "docs/knowledge-graph.html": "How Omabook builds a knowledge graph and timelines of the books you read, never past your page.",
    "docs/ai.html": "Set up AI in Omabook per capability: local Ollama first, hosted only "
    "where you allow it, and a Test button on every row.",
    "docs/plugins.html": "Omabook plugins: how to find, install and write them. Storyteller writes books; Notebooks keeps pages and ink.",
    "docs/calibre.html": "Import a Calibre library into Omabook with your stars, reading places, highlights and metadata plugins.",
    "docs/phones-and-e-readers.html": "Serve your Omabook library to phones and e-readers over OPDS, and "
    "keep KOReader's reading place in step.",
    "docs/sync.html": "Sync Omabook between your own computers over ssh, share collections, and spaces for a household or book club.",
    "docs/agents.html": "Use Omabook from AI agents: a command line with JSON output, an MCP server, and a skill for coding agents.",
    "docs/omabook-vs-calibre.html": "Omabook and Calibre compared: what each does well, what Omabook takes "
    "over, and what it leaves to Calibre.",
    "help/shortcuts.html": "Every Omabook keyboard shortcut for the library, the reader, the graph and the timelines.",
    "help/troubleshooting.html": "Fixes for common Omabook problems: a missing AI model, slow first opens, dictation, and an empty graph.",
    "help/faq.html": "Omabook questions: does it change my files, does my reading leave the computer, does the site track me.",
    "plugins.html": "Plugins for Omabook, found from its registry and from GitHub, with what each asks to be allowed.",
    "whats-new.html": "What each Omabook release brought, newest first.",
}
OG_IMAGE = "assets/og.png"
GOOGLE_SITE_VERIFICATION = os.environ.get("GOOGLE_SITE_VERIFICATION", "").strip()
THEME_JS = """(function () {
  // Remembers the theme preview in this browser only. Nothing is sent anywhere.
  var key = "omabook-theme", form = document.querySelector(".themes");
  if (!form) return;
  try {
    var saved = localStorage.getItem(key);
    if (saved) { var r = form.querySelector('input[value="' + saved + '"]'); if (r) r.checked = true; }
  } catch (e) {}
  form.addEventListener("change", function (e) {
    try { localStorage.setItem(key, e.target.value); } catch (x) {}
  });
})();
"""


def fragment(name):
    with open(os.path.join(HERE, "pages", name), encoding="utf-8") as f:
        return f.read().replace("{{version}}", VERSION)


def write(path, text):
    full = os.path.join(SITE, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8") as f:
        f.write(text)


def nav_html(depth, here):
    links = []
    for href, label in NAVS:
        current = here == href if href in ("", "plugins.html") else here.startswith(href)
        links.append(f'<a href="{depth}{href}"{" aria-current=page" if current else ""}>{label}</a>')
    return "".join(links)


def themes_html():
    radios = "".join(
        f'<label><input type="radio" name="t" value="{value}"{" checked" if i == 0 else ""}><span>{name}</span></label>'
        for i, (value, name) in enumerate(THEMES)
    )
    return f'<form class="themes" aria-label="Preview a theme"><fieldset><legend>Theme</legend>{radios}</fieldset></form>'


def head_meta(depth, title, meta):
    path = meta["path"]
    desc = meta.get("desc") or DESCS.get(path) or DESCRIPTION
    url = seo.SITE_URL + path
    PAGES.append({"path": path, "title": title, "desc": desc, "md": meta.get("md")})
    tags = [
        f'<meta name="description" content="{E(desc)}">',
        f'<link rel="canonical" href="{url}">',
        '<meta property="og:type" content="website"><meta property="og:site_name" content="Omabook">',
        f'<meta property="og:title" content="{E(title)}"><meta property="og:description" content="{E(desc)}">',
        f'<meta property="og:url" content="{url}"><meta property="og:image" content="{seo.SITE_URL}{OG_IMAGE}">',
        '<meta name="twitter:card" content="summary_large_image">',
        f'<link rel="alternate" type="application/atom+xml" title="Omabook releases" href="{depth}releases.xml">',
    ]
    if GOOGLE_SITE_VERIFICATION:
        tags.insert(1, f'<meta name="google-site-verification" content="{E(GOOGLE_SITE_VERIFICATION)}">')
    if meta.get("md"):
        tags.append(f'<link rel="alternate" type="text/markdown" href="{depth}{meta["md"]}">')
    tags += meta.get("ld", [])
    return "\n".join(tags)


def frame(depth, here, title, body, meta):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{E(title)}</title>
{head_meta(depth, title, meta)}
<meta name="color-scheme" content="light dark">
<link rel="stylesheet" href="{depth}style.css">
<link rel="icon" href="{depth}assets/omabook.svg" type="image/svg+xml">
<script src="{depth}theme.js" defer></script>
</head>
<body class="dir-b">
<a class="skip" href="#main">Skip to content</a>
<header class="top"><a class="brand" href="{depth}index.html"><span class="mark" aria-hidden="true"></span>Omabook</a>
<nav aria-label="Main">{nav_html(depth, here)}<a class="gh" href="https://github.com/jellespijker/omabook">GitHub</a></nav>
{themes_html()}</header>
<main id="main">{body}</main>
<footer class="foot"><p>Omabook is free software. This site is static: no cookies, no analytics, no scripts from elsewhere.</p>
<p><a href="https://github.com/jellespijker/omabook">Source</a> · <a href="{depth}docs/">Docs</a> ·
<a href="{depth}help/">Help</a> · <a href="{depth}whats-new.html">What's new</a> ·
<a href="https://github.com/jellespijker/omabook/blob/main/CHANGELOG.md">Changelog</a></p>
<p>Pictures marked demo are Omabook's own library of made-up books. The others are a copy of a real library
with nothing private shown.</p></footer>
</body></html>"""


def side(items, current, label):
    links = "".join(f'<li><a href="{key}.html"{" aria-current=page" if key == current else ""}>{E(title)}</a></li>' for key, title in items)
    return f'<nav class="toc" aria-label="{label}"><p class="toc-h">{label}</p><ol>{links}</ol></nav>'


def pager(items, current):
    i = [key for key, _ in items].index(current)
    before = f'<a href="{items[i - 1][0]}.html">← {E(items[i - 1][1])}</a>' if i else "<span></span>"
    after = f'<a href="{items[i + 1][0]}.html">{E(items[i + 1][1])} →</a>' if i + 1 < len(items) else "<span></span>"
    return f'<div class="pager">{before}{after}</div>'


def write_markdown(path, title, fragment_html):
    """A page's words as Markdown, next to the page, for agents."""
    url = seo.SITE_URL + path.replace(".md", ".html")
    write(path, f"<!-- {title}: {url} -->\n\n" + to_markdown(fragment_html))


def section_pages(kind, items, label):
    for key, title in items:
        crumb = f'<p class="crumb"><a href="./">{label}</a> / {E(title)}</p>' if kind == "docs" else ""
        words = fragment(f"{kind}-{key}.html")
        article = f'<article class="prose">{crumb}{words}{pager(items, key)}</article>'
        meta = {"path": f"{kind}/{key}.html", "md": f"{kind}/{key}.md"}
        if key == "faq":
            meta["ld"] = [seo.ld_script(seo.faq_data(words))]
        body = f'<div class="docs">{side(items, key, label)}{article}</div>'
        write(f"{kind}/{key}.html", frame("../", f"{kind}/", f"{title}: Omabook {kind}", body, meta))
        write_markdown(f"{kind}/{key}.md", title, words)
    index = "".join(f'<li><a href="{key}.html">{E(title)}</a></li>' for key, title in items)
    article = f'<article class="prose">{fragment(f"{kind}-index.html")}<ol class="idx">{index}</ol></article>'
    body = f'<div class="docs">{side(items, "", label)}{article}</div>'
    write(f"{kind}/index.html", frame("../", f"{kind}/", f"{label}: Omabook", body, {"path": f"{kind}/", "desc": f"{label} for Omabook."}))


def registry():
    with open(os.path.join(ROOT, "share", "omabook", "plugins.json"), encoding="utf-8") as f:
        data = json.load(f)
    return [
        {
            "name": p["name"],
            "title": p["title"],
            "repo": p["repository"],
            "author": p.get("author", ""),
            "lic": p.get("license", ""),
            "perms": p.get("permissions", []),
            "needs": p.get("omabook", ""),
            "desc": p.get("description", ""),
            "src": "registry",
            "reviewed": bool(p.get("reviewed")),
            "stars": None,
            "updated": "",
        }
        for p in data["plugins"]
    ]


def get_json(url):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "omabook-site-build"})
    if os.environ.get("GITHUB_TOKEN"):
        request.add_header("Authorization", "Bearer " + os.environ["GITHUB_TOKEN"])
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def manifest_of(repo):
    url = f"https://raw.githubusercontent.com/{repo['full_name']}/{repo['default_branch']}/plugin.json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "omabook-site-build"}), timeout=20) as r:
            manifest = json.load(r)
    except (urllib.error.URLError, OSError, ValueError):
        return None
    return manifest if isinstance(manifest, dict) and manifest.get("name") else None


def entry_from(repo, manifest):
    return {
        "name": manifest["name"],
        "title": manifest.get("title") or manifest["name"],
        "repo": repo["full_name"],
        "author": (repo.get("owner") or {}).get("login", ""),
        "lic": (repo.get("license") or {}).get("spdx_id") or "",
        "perms": manifest.get("permissions", []),
        "needs": manifest.get("omabook", ""),
        "desc": manifest.get("description") or repo.get("description") or "",
        "src": "github topic",
        "reviewed": False,
        "stars": repo.get("stargazers_count"),
        "updated": (repo.get("pushed_at") or "")[:10],
    }


def discovered(known):
    """Repositories with the topic omabook-plugin and a valid plugin.json, as Omabook finds them itself."""
    query = "https://api.github.com/search/repositories?q=topic:omabook-plugin+archived:false&sort=stars&per_page=30"
    try:
        hits = get_json(query)["items"]
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        print("plugin discovery skipped:", error, file=sys.stderr)
        return []
    found = []
    for repo in hits:
        manifest = None if repo["full_name"] in known else manifest_of(repo)
        if manifest:
            found.append(entry_from(repo, manifest))
    return found


def card(p):
    perms = ", ".join(p["perms"]) or "nothing"
    extra = ""
    if p["stars"] is not None:
        extra = f"<dt>Stars</dt><dd>{p['stars']}</dd><dt>Updated</dt><dd>{E(p['updated'])}</dd>"
    badge = ("ok", "Reviewed") if p["reviewed"] else ("warn", "Not reviewed")
    target = p["name"] if p["src"] == "registry" else p["repo"]
    return (
        f'<article class="plug"><header><h3>{E(p["title"])}</h3><span class="badge {badge[0]}">{badge[1]}</span></header>'
        f'<p>{E(p["desc"])}</p><dl class="meta"><dt>Found in</dt><dd>{E(p["src"])}</dd><dt>Asks for</dt><dd>{E(perms)}</dd>'
        f"<dt>Needs</dt><dd>Omabook {E(p['needs'] or 'any')}</dd><dt>Licence</dt><dd>{E(p['lic'] or 'not stated')}</dd>{extra}"
        f'<dt>Repository</dt><dd><a href="https://github.com/{E(p["repo"])}">{E(p["repo"])}</a></dd></dl>'
        f'<p class="cmd"><code>omabook plugin add {E(target)}</code></p></article>'
    )


def plugins_page(offline):
    reg = registry()
    extra = [] if offline else discovered({p["repo"] for p in reg})
    cards = "".join(card(p) for p in reg + extra)
    count = f"{len(reg)} in the registry · {len(extra)} more from GitHub at the last build"
    body = (
        f'<div class="plugins-page">{fragment("plugins-top.html")}'
        f'<section class="p-list"><h2>Everything found</h2><p class="count">{count}</p><div class="plug-grid">{cards}</div>'
        '<p class="note">Write your own: press <kbd>N</kbd> in the plugin manager, then add the topic <code>omabook-plugin</code> '
        "to your repository to be listed here.</p></section></div>"
    )
    write("plugins.html", frame("", "plugins.html", "Plugins: Omabook", body, {"path": "plugins.html"}))


def product_page():
    words = fragment("product.html")
    meta = {"path": "", "md": "index.md", "desc": DESCRIPTION}
    shots = ["assets/shots/covers.webp", "assets/shots/spines.webp", "assets/demo/graph.webp", "assets/shots/notebook.webp"]
    meta["ld"] = [seo.ld_script(seo.app_data(VERSION, shots))]
    page = frame("", "", "Omabook: a free ebook reader for Linux, with a knowledge graph", words, meta)
    write("index.html", page.replace('<body class="dir-b">', '<body class="dir-b home">'))
    write_markdown("index.md", "Omabook", words)


def whats_new_page():
    entries = seo.releases(ROOT)
    body = f'<div class="docs"><article class="prose">{seo.whats_new_fragment(entries)}</article></div>'
    write("whats-new.html", frame("", "whats-new.html", "What's new: Omabook", body, {"path": "whats-new.html"}))
    write("releases.xml", seo.releases_feed(entries))


def machine_files():
    """Sitemap, robots, llms.txt: written last, from the pages that were made."""
    today = datetime.date.today().isoformat()
    paths = sorted({p["path"] for p in PAGES} | {p["md"] for p in PAGES if p["md"]})
    write("sitemap.xml", seo.sitemap([p for p in paths if not p.endswith(".md")], today))
    write("robots.txt", seo.robots())
    docs = [(p["title"], p["md"], p["desc"]) for p in PAGES if p["md"] and p["path"].startswith(("docs/", "help/"))]
    write("llms.txt", seo.llms_txt([("Omabook", "index.md", DESCRIPTION)] + docs))


def main():
    product_page()
    section_pages("docs", DOCS, "Documentation")
    section_pages("help", HELPS, "Help")
    plugins_page("--offline" in sys.argv)
    whats_new_page()
    machine_files()
    write("theme.js", THEME_JS)
    print("built", len(PAGES), "pages")


if __name__ == "__main__":
    main()
