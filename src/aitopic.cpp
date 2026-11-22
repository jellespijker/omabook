#include "manual.h"

// The help's page on the AI: the companion, asking and dictating by voice, and
// the AI page's rows, each tested, a model found for it and a fallback. Out of
// helptopics.cpp, which has the other pages, as the page grew with each release
// of the AI page; the same rules hold ({commands} as keys, `backticks` typed).

// The companion, asking and dictating by voice (#148, #149), and the AI page's rows (#170, #172), a model found (#175)
// and a fallback (#174).
Manual::Topic Manual::aiTopic() {
    return {"ai",
            Manual::tr("The AI companion"),
            Manual::tr("It reads along with you, and looks after the library when asked."),
            {
                {Manual::tr("While you read"),
                 Manual::tr("{reader.ask} opens the companion beside the page. Ask about the chapter in front of you, or select a "
                            "passage first and ask about that. It is sent the book only as far as you have read, so it can't spoil "
                            "the ending.")},
                {Manual::tr("Coming back to a book"),
                 Manual::tr("{reader.catch_up} tells you where things stand, in a few lines. {reader.cast} lists the people and "
                            "places that matter so far. Both keep notes with the book, so a later book in a series builds on the "
                            "earlier ones.")},
                {Manual::tr("For the whole library"),
                 Manual::tr("{library.ask} asks about your library: “which of these are about the sea?”. {library.suggest} suggests "
                            "what to read next. {library.tidy} looks for duplicates, authors spelled several ways and missing "
                            "series, and changes nothing until you say so. {library.lookup} looks a book's details up and has the "
                            "AI weigh what the catalogs found.")},
                {Manual::tr("Saying it instead of typing"),
                 Manual::tr("A note's or a question's microphone, or Ctrl+M in its field, listens while you hold it, or from one press "
                            "to the next, and writes out what you say at the cursor; Escape stops it. A question said is asked when "
                            "you let go, and its answer is read aloud. It goes through speech to text as chosen on the AI page.")},
                {Manual::tr("Each row of the AI page"),
                 Manual::tr("Under each row of the AI page ({global.settings}) a line says what Omabook uses it for. Its Test, or T on "
                            "the row, sends one small real request through that row and shows what came back, or why not; where the "
                            "model isn't there, it offers one the server has, or downloads one onto Ollama. Handwriting can have a "
                            "server of its own.")},
                {Manual::tr("A model beyond what the server has"),
                 Manual::tr("Find a Model… in a row searches Ollama's library and Hugging Face for models that can do its work, "
                            "with the download's size, the memory it needs and whether that fits this computer, its licence, and "
                            "whether it runs on someone else's computer. Install puts it on the row's Ollama server, or downloads "
                            "a speech model, chooses it and tests it. Only the words you search for go out.")},
                {Manual::tr("When a server can't be reached"),
                 Manual::tr("A row can fall back to this computer or another server when its own can't be reached at all, never "
                            "when it answers with an error. The row and Find say so while it does, and Omabook goes back to the "
                            "first by itself once it answers. The fallback has a Test of its own.")},
                {Manual::tr("Changing Omabook itself"),
                 Manual::tr("{global.change} takes a wish in words, such as “make it warmer” or “bigger covers”, and rewrites the "
                            "interface's files for you. One click puts back what the last change replaced.")},
                {Manual::tr("Which model, and what is sent"),
                 Manual::tr("The settings ({global.settings}) pick the model: Omarchy's default agent, Claude with a key, a server such "
                            "as Ollama or Omarchy's local models, any command, or none. Nothing is sent until you ask; then it is your "
                            "question, the book's title and authors, and the part of the text you asked about, to that model only.")},
            }};
}
