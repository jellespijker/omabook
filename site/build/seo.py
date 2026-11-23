"""What search engines and agents read besides the pages: sitemap, robots, llms.txt, structured data, the releases feed."""

import html
import json
import os
import re

E = html.escape
SITE_URL = "https://jellespijker.github.io/omabook/"
REPO_URL = "https://github.com/jellespijker/omabook"
SUMMARY = (
    "Omabook is a free ebook reader and library for Linux (Arch and Omarchy). It reads EPUB, PDF, FictionBook, "
    "comics and more, imports a Calibre library, and keeps a knowledge graph of the people, places, ideas and "
    "events of the books you read, never past the page you are on. AI is optional and per capability, local first. "
    "Every library action is a command with JSON output and a tool of its MCP server."
)
AI_CRAWLERS = ["GPTBot", "ChatGPT-User", "OAI-SearchBot", "ClaudeBot", "Claude-User", "anthropic-ai", "PerplexityBot", "Google-Extended"]


def project_version(root):
    with open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8") as f:
        found = re.search(r"project\(omabook VERSION ([0-9.]+)", f.read())
    return found.group(1) if found else ""


def releases(root, limit=8):
    """The newest releases of CHANGELOG.md: version, date and the sentence under the heading."""
    with open(os.path.join(root, "CHANGELOG.md"), encoding="utf-8") as f:
        text = f.read()
    found = []
    for match in re.finditer(r"^## \[([0-9][^\]]*)\] - (\d{4}-\d\d-\d\d)\n\n(.*?)(?=\n\n|\Z)", text, re.M | re.S):
        found.append({"version": match.group(1), "date": match.group(2), "summary": " ".join(match.group(3).split())})
    return found[:limit]


def whats_new_fragment(entries):
    items = "".join(
        f'<article class="rel"><h2>{E(e["version"])} <small>{e["date"]}</small></h2><p>{E(e["summary"])}</p></article>' for e in entries
    )
    link = f'<p><a href="{REPO_URL}/blob/main/CHANGELOG.md">The full changelog, with every change and its issue</a></p>'
    intro = (
        '<h1>What’s new</h1><p>What each release of Omabook brought, newest first. The same list is a <a href="releases.xml">feed</a>.</p>'
    )
    return intro + items + link


def releases_feed(entries):
    updated = entries[0]["date"] if entries else "2026-01-01"
    items = "".join(
        f"<entry><title>Omabook {E(e['version'])}</title><id>{REPO_URL}/releases/tag/v{E(e['version'])}</id>"
        f'<link href="{REPO_URL}/releases/tag/v{E(e["version"])}"/><updated>{e["date"]}T00:00:00Z</updated>'
        f"<summary>{E(e['summary'])}</summary></entry>"
        for e in entries
    )
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n<feed xmlns="http://www.w3.org/2005/Atom"><title>Omabook releases</title>'
        f'<id>{SITE_URL}</id><link href="{SITE_URL}releases.xml" rel="self"/><link href="{SITE_URL}"/>'
        f"<updated>{updated}T00:00:00Z</updated>{items}</feed>\n"
    )


def sitemap(paths, date):
    urls = "".join(f"<url><loc>{SITE_URL}{E(p)}</loc><lastmod>{date}</lastmod></url>" for p in paths)
    return f'<?xml version="1.0" encoding="utf-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">{urls}</urlset>\n'


def robots():
    rules = "".join(f"User-agent: {name}\nAllow: /\n\n" for name in AI_CRAWLERS)
    return f"User-agent: *\nAllow: /\n\n{rules}Sitemap: {SITE_URL}sitemap.xml\n"


def llms_txt(pages):
    """pages: (title, markdown path, description) of the pages that have a Markdown copy."""
    lines = ["# Omabook", "", f"> {SUMMARY}", "", f"Source: {REPO_URL}", f"Site: {SITE_URL}", "", "## Docs", ""]
    lines += [f"- [{title}]({SITE_URL}{path}): {desc}" for title, path, desc in pages]
    lines += ["", "## Optional", "", f"- [Releases feed]({SITE_URL}releases.xml)", f"- [Source and issues]({REPO_URL})", ""]
    return "\n".join(lines)


def app_data(version, screenshots):
    return {
        "@context": "https://schema.org",
        "@type": "SoftwareApplication",
        "name": "Omabook",
        "description": SUMMARY,
        "applicationCategory": "MultimediaApplication",
        "operatingSystem": "Linux (Arch Linux, Omarchy)",
        "softwareVersion": version,
        "license": "https://opensource.org/license/mit",
        "isAccessibleForFree": True,
        "offers": {"@type": "Offer", "price": "0", "priceCurrency": "USD"},
        "url": SITE_URL,
        "downloadUrl": f"{REPO_URL}/releases",
        "codeRepository": REPO_URL,
        "screenshot": [f"{SITE_URL}{s}" for s in screenshots],
        "featureList": [
            "Knowledge graph and timelines across books, never past your page",
            "Follows the Omarchy theme and language live",
            "AI per capability, local first, hosted only where allowed",
            "Calibre import with metadata plugins",
            "OPDS and KOReader sync for phones and e-readers",
            "Command line with JSON output and an MCP server for agents",
        ],
    }


def faq_data(fragment):
    pairs = re.findall(r"<summary>(.*?)</summary><p>(.*?)</p>", fragment, re.S)
    entities = [
        {"@type": "Question", "name": html.unescape(q), "acceptedAnswer": {"@type": "Answer", "text": html.unescape(a)}} for q, a in pairs
    ]
    return {"@context": "https://schema.org", "@type": "FAQPage", "mainEntity": entities}


def ld_script(data):
    return '<script type="application/ld+json">' + json.dumps(data, ensure_ascii=False).replace("</", "<\\/") + "</script>"
