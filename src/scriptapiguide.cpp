// The parts of the plugin guide (scriptapi.cpp) on what a plugin makes and
// keeps: books of its own, and files, the person's among them (#131);
// and what it writes and records beside a book being read (#137, #135).

#include "scriptapi.h"

namespace ScriptApi {

QString booksGuide() {
    return QStringLiteral(R"guide(
### A plugin that makes books

A plugin can write books of its own and go on writing them while they are
read: a story told chapter by chapter, a digest that grows every morning.
It needs the `files` permission, since a book it makes starts as a file it
wrote under its `data/` folder.

1. Write the book's file: `omabook.files.write("data/story.txt", text)`, in
   a format Omabook reads (plain text, Markdown, HTML) or one the plugin
   registers with `omabook.format.register`.
2. `const book = omabook.library.create({title, authors, tags, ...}, "data/story.txt")`
   copies it into the library and marks the book as the plugin's:
   `book.generated === omabook.plugin.id`. Keep `book.id`
   (`omabook.settings.set`), or find the plugin's books again with
   `omabook.library.list().filter((book) => book.generated === omabook.plugin.id)`.
3. To add to it, write the whole file again and call
   `omabook.library.replaceFile(book.id, "data/story.txt")`. If the reader
   has the book open it is read anew and they stay where they were, with
   the new chapters in the contents. Only a book the plugin made can be
   replaced or removed (`omabook.library.remove`); a book a person added,
   or another plugin's, throws.
4. Know where the reader is: `page.turned`, `book.opened` and `book.closed`
   all carry `{id, generated, section, sections, fraction, percent}`, and
   `omabook.reader.current()` answers the same, so
   `event.generated === omabook.plugin.id && event.section >= event.sections - 2`
   is "they are near the end of one of mine: write the next chapter".
5. Write with `omabook.ai.ask(prompt, {system, onText})`. The promise waits
   for the whole answer however long it takes; nothing times it out. Only
   the script's own code is timed (five seconds a call), so do the work in
   `.then`, not in a loop that waits.
6. Ask the reader where the story should go with
   `omabook.ui.ask({title: "What happens next?"})`, from a command with a
   key: a promise of their words, or `null` when they would rather not say.
   For more than one answer, `omabook.ui.form({title, fields: [...]})`
   puts a whole form: choices, numbers, switches, and fields shown only
   when another field says so.
7. Let a book be started from the shelf:
   `omabook.ui.launcher({id: "new", title: "Write a new story", open: start})`
   puts an entry among the books that calls `start` when it is opened,
   and is gone again when the plugin is switched off.
)guide");
}

QString filesGuide() {
    return QStringLiteral(R"guide(
### Files, pictures and the clipboard

With the `files` permission a plugin keeps what it makes under its `data/`
folder: text with `omabook.files.write`, and bytes, a picture or a sound, as
an ArrayBuffer or a typed array (`files.write` and `files.writeBytes`), or as
base64 (`files.writeBytes`). `files.readBytes` and `files.readBase64` read
them back, and a view shows one as
`"data:image/png;base64," + omabook.files.readBase64("data/one.png")`, or by
its path in the folder (`omabook.plugin.dataFolder`). A file holds at most
64 MB and the folder 1 GB (`files.usage()`); a path that leads out of the
folder throws.

```js
omabook.commands.register({id: "paste", title: "Paste a picture", place: "reader", key: "Ctrl+Shift+V", run: function () {
    const picture = omabook.ui.clipboard.readImage();    // the "clipboard" permission
    if (picture)
        picture.copyTo("data/pictures/" + Date.now() + ".png");
}});
omabook.commands.register({id: "add", title: "Add a picture", place: "reader", run: function () {
    return omabook.ui.pickFile({kinds: ["image"]}).then(function (file) {
        if (file)
            file.copyTo("data/pictures/" + file.name);
    });
}});
```

`omabook.ui.pickFile` asks the person with the desktop's own dialog, and
`omabook.ui.clipboard` reads and writes text and pictures; both only right
after the person did something of the plugin's (ran one of its commands,
used its view or panel), and otherwise throw. What they picked or copied is
read through what is answered (`bytes()`, `base64()`, `text()`,
`copyTo(path)`), never by its path. Where there is no window (a command)
nothing is picked and the clipboard is empty.

**Everything under `data/` goes to the person's other computers** when they
sync, as their books do: a file once, by its content, where the plugin is
installed too, and a file removed with `files.remove` is removed there.
`settings.json` stays on its computer, and so does a file over the limits.
None of it goes into a space shared with others.

### Exports: saving, PDFs and links

What a plugin exports for the person goes where they choose, not into
`data/` (which goes to their other computers): `omabook.ui.saveFile` asks
with the desktop's own save dialog, on their action as `pickFile` does, and
writes the bytes or text there. `omabook.pdf` draws a PDF to save, in the
app's fonts:

```js
omabook.commands.register({id: "export", title: "Export as PDF", place: "view", run: function () {
    const pdf = omabook.pdf.create({width: 595, height: 842, title: "Notebook"});
    const page = pdf.page();
    page.text("Monday", {x: 48, y: 40, size: 20, bold: true});
    page.path([48, 80, 200, 96, 320, 80], {width: 2, color: "#3366cc", alpha: 0.8});
    page.image(omabook.files.readBytes("data/pictures/one.png"), {x: 48, y: 120, width: 240});
    return omabook.ui.saveFile({name: "Notebook.pdf", kinds: [".pdf"], bytes: pdf.bytes()});
}});
```

A link to a place in a book opens Omabook there, from Markdown, a note or
a browser: `"omabook://book/" + book.gid + "/section/" + section` (a book's
`gid` from `library.get`, the same on each of the person's computers;
`section` from 0, or left out for where they left it). A window already
open takes it.

Before asking the AI about something private, `omabook.ai.status({hosted:
false})` says whether the model the person chose is their own (this
computer, their tailnet, an address they marked as theirs); `ai.ask(prompt,
{hosted: false})` then never sends it to anyone else's.

To hand `omabook plugin run` more than a command line holds, give `-` for
the JSON and pipe it in: `omabook plugin run notes import - < pages.json`.
)guide");
}

QString besideGuide() {
    return QStringLiteral(R"guide(
### Writing beside a book, and recording beside it

A plugin that keeps notes beside what is read (a notebook) refers to the
book by a **passage**, as Omabook keeps a highlight: its words, a few just
before and after them, and where they are. The words find it again when the
book's file changes (a new edition, a typo fixed); the place finds it fast.

```js
const selected = omabook.reader.selection();          // {book, global_id, passage, confidence}, or null
const here = omabook.passages.make(book.id, {section: 3, fraction: 0.4});      // the words that start there
const chosen = omabook.passages.make(book.id, {section: 3, fraction: 0.4, quote: "the far pier"});
const again = omabook.passages.find(book.id, chosen);  // {passage, confidence} in the book as it is now, or null
omabook.on("reader.selected", function (event) { /* event.passage, null once nothing is selected */ });
```

A book's number (`id`) is this computer's; what a plugin keeps that travels
names a book by its `global_id`, which every book it is told of carries
(`library.list`, the events, `reader.current()`), the same on every computer,
and `omabook.library.byGlobalId(id)` finds it here. `library.sharedIn(id)`
says the shared and published spaces a book goes into: beside a book in one,
write as for others to read, and say nothing past where they read to.

On `InkSurface`, `hiddenStrokes` and `emphasizedStrokes` take stroke ids:
those strokes are not drawn, or drawn in `emphasisColor`, which is how a
page shows what was written at a moment of a recording. Only how they are
drawn changes. Each `inked` batch is one write of the change log
(`omabook.log.add`): a change too large for one (the lasso moving a whole
page) comes as several, those that add strokes before those that remove.

`omabook.ui.notify("Recording kept", {action: "Play", run: play})` says
something briefly at the bottom of the window, with a button.

A recording is large: `omabook.files.onDemand("data/recordings")` keeps the
recordings on the computer they were made on until another one opens them;
`omabook.files.status(path)` says whether one is here, and
`omabook.files.fetch(path)` brings it, a promise of its path:

```js
omabook.files.fetch(file).then(function () { return omabook.audio.play(file); });
```

Try recording without a window or a sound card in a **scratch library**, a
new folder that is none of your own libraries:
`omabook plugin run notes record --library /tmp/scratch --test-microphone tone --allow-microphone`.
The microphone hears a tone (or `silence`, or a WAV file of mono 16-bit
samples at 48 kHz) as fast as a real one would, what plays goes nowhere at
the speed it would play, and the yes to recording is given beforehand.
)guide");
}

} // namespace ScriptApi
