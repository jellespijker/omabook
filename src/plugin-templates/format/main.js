// A book format of its own. This one is a "text book": a folder, or the same
// folder zipped and called .txtbook, of plain text chapters with an
// index.txt that says what the book is and in what order its chapters come:
//
//     title: The Long Way
//     author: Ann Author
//
//     01-start.txt   Setting Out
//     02-road.txt    On the Road
//
// That is the whole of a format: say which files are yours, and describe the
// book in them as chapters. Omabook does the rest (the reader, the contents,
// search, the command line, the AI), and needs no permission for it, since
// a format sees nothing but the one book it is asked about.
omabook.format.register({
    name: "Text book",

    // A file with one of these extensions is this format. The first is also
    // what a folder or a plain .zip that `claims` says yes to is packed as
    // when it joins a library, which keeps one file per book.
    extensions: ["txtbook"],

    // Whether a folder or a .zip is a book of this kind. `source` is that
    // one book's files: source.name, source.entries(), source.contains(file),
    // source.text(file), source.bytes(file) (base64) and
    // source.resolve(fromFile, relativePath). Leave claims out for a format
    // that is only ever a file with its own extension.
    claims(source) {
        const entries = source.entries();
        return entries.indexOf("index.txt") >= 0 && entries.every((name) => /\.(txt|png|jpe?g|gif)$/i.test(name));
    },

    // The book: its details, and its chapters in reading order. A chapter's
    // words are `text` (plain), `markdown` or `html`, whichever is easiest;
    // `file` says where it came from, so pictures and links in it are found
    // relative to it. Answer the book itself, not a promise. Throw to say
    // why a book can't be read: the message is what the reader is shown.
    open(source) {
        const index = source.text("index.txt");
        if (index === null)
            throw new Error("There is no index.txt in " + source.name + ".");
        const book = {title: source.name, authors: [], chapters: []};
        for (const line of index.split("\n").map((line) => line.trim())) {
            const detail = /^(title|author|language|description|cover)\s*:\s*(.+)$/i.exec(line);
            const chapter = /^(\S+\.txt)\s*(.*)$/i.exec(line);
            if (detail && detail[1].toLowerCase() === "author")
                book.authors.push(detail[2]);
            else if (detail)
                book[detail[1].toLowerCase()] = detail[2];
            else if (chapter && source.contains(chapter[1]))
                book.chapters.push({title: chapter[2] || chapter[1], text: source.text(chapter[1]), file: chapter[1]});
        }
        if (book.chapters.length === 0)
            throw new Error("index.txt lists no chapters that are in " + source.name + ".");
        return book;
    }
});

// To try it without the window, on a folder, a .zip or a .txtbook:
//   omabook plugin run {{name}} format:"Text book" '"/path/to/book"'
//   omabook toc /path/to/book.txtbook
