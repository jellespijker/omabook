// A catalog writes the library, or the books matching a search, as one
// file: omabook catalog --format reading-list > books.md
omabook.catalog.register({
    format: "reading-list",
    description: "A reading list in Markdown, one heading per author.",

    // `books` is the list to write, each as omabook.library.get answers it.
    // Answer the text of the file.
    make(books) {
        const byAuthor = new Map();
        for (const book of books) {
            const author = book.authors[0] || "Unknown";
            byAuthor.set(author, (byAuthor.get(author) || []).concat([book]));
        }
        const lines = ["# Books", ""];
        for (const author of Array.from(byAuthor.keys()).sort()) {
            lines.push("## " + author, "");
            for (const book of byAuthor.get(author))
                lines.push("- [" + (book.status === "read" ? "x" : " ") + "] " + book.title
                           + (book.series ? " (" + book.series + " " + book.series_index + ")" : ""));
            lines.push("");
        }
        return lines.join("\n");
    }
});
