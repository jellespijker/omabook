// A source of book details. Omabook asks it whenever a book is looked up
// (M on the shelf, or `omabook lookup <id>`), beside its own catalogs.
// It needs the "http" permission in plugin.json to reach the network.
omabook.metadata.register({
    name: "Gutenberg",
    fields: ["title", "authors", "language", "tags", "cover"],

    // `book` is what is known so far: {title, authors, isbn, ...}. Answer a
    // list of candidates, or a promise of one. Each is shown beside the
    // others, saying what it would add and change, and the user picks.
    search(book) {
        const words = [book.title].concat(book.authors.slice(0, 1)).join(" ");
        return omabook.http.get("https://gutendex.com/books?search=" + encodeURIComponent(words))
            .then((response) => {
                if (!response.ok)
                    throw new Error("Gutendex answered " + response.status);
                return response.json().results.slice(0, 5).map(candidate);
            });
    }
});

// One of Gutendex's results in the shape Omabook reads. A function at the
// top level can also be called from outside, which is how to try it:
//   omabook plugin run {{name}} candidate '[{"title": "Moby Dick", "authors": [{"name": "Melville, Herman"}]}]'
function candidate(found) {
    return {
        title: found.title,
        // "Melville, Herman" is "Herman Melville" on a shelf.
        authors: (found.authors || []).map((author) => author.name.split(", ").reverse().join(" ")),
        language: (found.languages || [])[0] || "",
        tags: (found.subjects || []).slice(0, 5),
        cover_url: (found.formats || {})["image/jpeg"] || ""
    };
}
