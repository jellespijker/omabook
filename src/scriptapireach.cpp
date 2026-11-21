// The part of the plugin API table (scriptapi.cpp) that reaches past the
// library: the reader, the plugin's settings, the network, its
// files, programs and sound (`omabook.audio`, #79), and the permissions that
// guard what of it a plugin may only do when it asks.

#include "scriptapi.h"

namespace ScriptApi {

QList<Entry> laterEntries() {
    return {
        {"reader.current", "reader.current()", "",
         "Where the reader is: `{id, generated, identifiers, section, sections, fraction, percent}`, or `null` when no "
         "book is open. `section` counts from 0 and `sections` is how many there are; `fraction` is how far into the "
         "section (0 to 1), `percent` how far into the book."},
        {"reader.open", "reader.open(id, {section, fraction})", "",
         "Opens a book in the reader: where it was last read to, or at the place given (`section` from 0, `fraction` 0 "
         "to 1 into it)."},
        {"reader.reload", "reader.reload()", "",
         "Has the reader read the open book's file anew and stay at the same section and fraction: for a book that "
         "changed on disk. Nothing happens when no book is open. `library.replaceFile` does this by itself."},

        {"settings.get", "settings.get(key, fallback)", "",
         "A value the plugin kept, or `fallback`. It is the plugin's own "
         "storage (data/settings.json) and survives restarts."},
        {"settings.set", "settings.set(key, value)", "", "Keeps a value: anything JSON can hold."},
        {"settings.all", "settings.all()", "", "Everything the plugin kept, as one object."},
        {"settings.option", "settings.option(key)", "",
         "One of Omabook's own look settings (`cover_size`, `font_size`, "
         "`measure`, `line_height`, `paragraph_spacing`, `indent`, "
         "`margins`, `align`, `font_family`, `pages`, `sort`). These are "
         "the defaults; a book can have its own (`omabook format`)."},
        {"settings.setOption", "settings.setOption(key, value)", "", "Changes one of those; false when the value doesn't fit."},

        {"http.get", "http.get(url, {headers})", "http",
         "Fetches an http or https address; a promise of `{status, ok, text, headers, json()}`."},
        {"http.post", "http.post(url, body, {headers})", "http",
         "Posts `body` (text, or an object sent as JSON); a promise of the same answer as `get`."},

        {"files.read", "files.read(path)", "files",
         "The text of a file in the plugin's folder, or `null`. Paths are relative to that folder."},
        {"files.readBytes", "files.readBytes(path)", "files", "The bytes of a file in the plugin's folder, as an ArrayBuffer, or `null`."},
        {"files.readBase64", "files.readBase64(path)", "files", "The same as base64 text, for a picture's `data:` address."},
        {"files.write", "files.write(path, text)", "files",
         "Writes a file under the plugin's `data/` folder (`data/notes.txt`), and nowhere else: text, or bytes (an "
         "ArrayBuffer or a typed array) as they are."},
        {"files.writeBytes", "files.writeBytes(path, bytes)", "files",
         "Writes bytes under the plugin's `data/` folder: an ArrayBuffer, a typed array (`Uint8Array`), or base64 "
         "text. A file holds at most 64 MB and the folder 1 GB; a write over either throws, as does one anywhere else."},
        {"files.list", "files.list(folder)", "files", "The names in a folder of the plugin's (its own when none is given)."},
        {"files.exists", "files.exists(path)", "files", "Whether there is such a file in the plugin's folder."},
        {"files.remove", "files.remove(path)", "files",
         "Removes a file under the plugin's `data/` folder, and on the person's other computers at their next sync."},
        {"files.usage", "files.usage()", "files",
         "`{used, file, plugin}`: the bytes the `data/` folder holds now, and the most a file and the folder may hold."},

        {"run", "run(program, args, {input, timeout})", "run",
         "Starts a program in the plugin's folder; a promise of `{code, stdout, stderr}`. `timeout` is in seconds (30 "
         "when not given)."},
        {"audio.record", "audio.record({file, onLevel(level, elapsed), onStopped(summary)})", "microphone",
         "Records from the microphone into `file`, an Opus file under the plugin's `data/` folder "
         "(`data/recordings/monday.opus`), about 4 KB a second of speech, for hours. The first time it records "
         "while loaded, the person is asked in the window, and while it records a mark at the top of the window says "
         "so, with a button to stop it; where nobody can be asked (a command) it can't record. A promise of the "
         "recording: `{file, started, elapsed(), level(), paused(), recording(), pause(), resume(), stop()}`, "
         "`started` the moment it began (ISO, UTC) and `elapsed()` its length in milliseconds. `onLevel` hears how "
         "loud it is (0 to 1) ten times a second, for a meter. `stop()` answers the summary, `{file, started, "
         "duration, bytes, parts: [{at, started}]}`: each part is a stretch between pauses, `at` milliseconds into "
         "the recording, begun at `started`, so a moment in it is a moment of the day. When it ends otherwise (the "
         "person stopped it, the microphone went), `onStopped` is given the summary, with a `problem` when it failed. "
         "One recording at a time."},
        {"audio.play", "audio.play(file, {at, paused, onPosition(milliseconds), onEnded()})", "",
         "Plays an Opus file of the plugin's folder, from `at` milliseconds in, or holds it there with `paused: "
         "true`. Answers the player, `{file, started, duration, position(), playing(), play(), pause(), "
         "seek(milliseconds), stop()}`; `onPosition` hears where it is ten times a second while it plays, so a view "
         "can follow along (hand it to the panel with `ui.set`), and `onEnded` when it played to its end. `stop()` "
         "lets it go. At most eight at once."},
        {"audio.info", "audio.info(file)", "",
         "What an Opus file of the plugin's folder is, without playing it: `{file, started, duration, bytes}`."},
    };
}

const QList<Permission> &permissions() {
    static const QList<Permission> permissions{
        {"http", "Reach the network: `omabook.http`."},
        {"files", "Read files in its own folder and write under its `data/` folder: `omabook.files`."},
        {"run", "Start programs on this computer: `omabook.run`."},
        {"microphone", "Record from the microphone, once the person said yes in the window: `omabook.audio.record`."},
        {"clipboard", "Read and write the clipboard, right after the person did something of the plugin's: `omabook.ui.clipboard`."},
    };
    return permissions;
}

} // namespace ScriptApi
