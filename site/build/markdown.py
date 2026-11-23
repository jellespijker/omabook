"""A page fragment as Markdown, for agents that would rather not read styled HTML (standard library only)."""

from html.parser import HTMLParser

HEADINGS = {"h1": "# ", "h2": "## ", "h3": "### "}
SKIPPED = {"script", "style", "nav", "button"}


class Converter(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []
        self.lists = []
        self.links = []
        self.skip = 0
        self.pre = False
        self.row = []
        self.cell = None
        self.header_row = False
        self.rows = 0

    def emit(self, text):
        if self.cell is not None:
            self.cell.append(text)
        else:
            self.out.append(text)

    def start_block(self, tag, attrs):
        if tag in HEADINGS:
            self.emit("\n\n" + HEADINGS[tag])
        elif tag in ("p", "dl", "table", "details", "figure"):
            self.emit("\n\n")
        elif tag in ("ul", "ol"):
            self.lists.append([tag, 0])
            self.emit("\n")
        elif tag == "li":
            kind = self.lists[-1] if self.lists else ["ul", 0]
            kind[1] += 1
            mark = f"{kind[1]}. " if kind[0] == "ol" else "- "
            self.emit("\n" + "  " * max(len(self.lists) - 1, 0) + mark)
        elif tag == "dt":
            self.emit("\n\n**")
        elif tag == "dd":
            self.emit("\n")
        elif tag == "summary":
            self.emit("\n\n**")

    def start_inline(self, tag, attrs):
        values = dict(attrs)
        if tag == "pre":
            self.pre = True
            self.emit("\n\n```\n")
        elif (tag == "code" and not self.pre) or tag == "kbd":
            self.emit("`")
        elif tag in ("strong", "b"):
            self.emit("**")
        elif tag in ("em", "i"):
            self.emit("*")
        elif tag == "a":
            self.links.append(values.get("href", ""))
            self.emit("[")
        elif tag == "img":
            self.emit(f"\n\n[Picture: {values.get('alt', '')}]\n\n")
        elif tag == "br":
            self.emit("\n")

    def start_table(self, tag, attrs):
        if tag == "tr":
            self.row = []
            self.header_row = False
        elif tag in ("th", "td"):
            self.cell = []
            self.header_row = self.header_row or tag == "th"

    def handle_starttag(self, tag, attrs):
        if tag in SKIPPED:
            self.skip += 1
        if self.skip:
            return
        self.start_block(tag, attrs)
        self.start_inline(tag, attrs)
        self.start_table(tag, attrs)

    def end_table(self, tag):
        if tag in ("th", "td") and self.cell is not None:
            self.row.append(" ".join("".join(self.cell).split()).replace("|", "\\|"))
            self.cell = None
        elif tag == "tr":
            self.out.append("\n| " + " | ".join(self.row) + " |")
            if self.rows == 0 and self.header_row:
                self.out.append("\n|" + " --- |" * len(self.row))
            self.rows += 1
        elif tag == "table":
            self.rows = 0

    def handle_endtag(self, tag):
        if tag in SKIPPED:
            self.skip -= 1
            return
        if self.skip:
            return
        self.end_table(tag)
        if tag == "pre":
            self.pre = False
            self.emit("\n```\n")
        elif tag in ("code", "kbd") and not self.pre:
            self.emit("`")
        elif tag in ("strong", "b", "dt", "summary"):
            self.emit("**")
        elif tag in ("em", "i"):
            self.emit("*")
        elif tag == "a":
            self.emit(f"]({self.links.pop()})")
        elif tag in ("ul", "ol") and self.lists:
            self.lists.pop()

    def handle_data(self, data):
        if self.skip:
            return
        if self.pre:
            self.emit(data)
            return
        words = " ".join(data.split())
        lead = " " if data[:1].isspace() and words else ""
        tail = " " if data[-1:].isspace() and words else ""
        self.emit(lead + words + tail)


def to_markdown(fragment):
    parser = Converter()
    parser.feed(fragment)
    text = "".join(parser.out)
    lines = [line.rstrip() for line in text.splitlines()]
    cleaned = []
    for line in lines:
        if line or (cleaned and cleaned[-1]):
            cleaned.append(line)
    return "\n".join(cleaned).strip() + "\n"
