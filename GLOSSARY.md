# Omabook

An ebook library and reader whose books, reading and knowledge stay one's own.

## Annotations

**Annotation**:
Anything a reader adds to a book at a place in it: a highlight, a margin note, book ink or free ink.
_Avoid_: Note (alone), markup, comment

**Highlight**:
A passage of a book marked in a colour.
_Avoid_: Underline, mark, selection

**Highlight colour**:
One of the five colours a highlight is in, known by its own name (yellow, blue, green, orange, purple) and called what the reader chooses ("Important"); each has a line of its own under the words, so it is told apart without its colour (`HighlightColors`).
_Avoid_: Category, tag, label

**Pen gesture**:
A stroke of the pen over a book's text that means something rather than staying as ink: a line under words, a loop round them or a bar beside lines highlights them; a scribble takes highlights away (`Ink::gestureOf`).
_Avoid_: Ink (for this), drawing, shortcut

**Margin note**:
Words about a passage or place in a book, typed or written by hand, shown beside it.
_Avoid_: Note (alone), comment, annotation (for this kind only)

**Handwritten margin note**:
A margin note written with the pen: its ink is the note, kept as strokes on it; its words are what was read of the ink, searched and shown on request (`NoteRecognition`).
_Avoid_: Ink note, scribble, drawing

**Margin column**:
The strip beside the text where margin notes stand, each at the height of its passage; part of the page's layout, there only when the window has room for it beside a comfortable page (`PageLayout::columns`).
_Avoid_: Sidebar, notes pane, gutter (the space between pages)

**Book ink**:
Handwriting or drawing directly on a page of a book whose pages don't reflow, such as a PDF or a comic.
_Avoid_: Drawing, scribble, ink (alone, which is the notebook plugin's)

**Free ink**:
Handwriting or drawing in the side margins and between the lines of a flowing book, each stroke anchored to the line beside it and drawn beside that line, at its height, however the text is laid out: *margin ink* on the whole line, *line ink* on the words under or over it (`LineInk`, `InkLines`, ADR 0010).
_Avoid_: Book ink (for this, which is on fixed pages), margin note (which is on words chosen first), drawing

**Page region**:
A part of a page of a book whose pages don't reflow, measured across and down in the page's width, so it is the same part at any zoom: where book ink lies on its page, and what a highlight or a margin note on a comic's panel is on, which has no words to find it by (`PageRegion`, `Annotation::area`).
_Avoid_: Area, box, rectangle, selection

**Ink surface**:
Where a pen writes and draws, in Omabook or a plugin's panel: pressure makes the line wider, the pen's other end erases, a palm is ignored while the pen is near (`InkSurface`).
_Avoid_: Canvas, drawing area

**Stroke**:
One line of ink, from the pen touching down to lifting: its points, each with its pressure, tilt and moment, kept as data rather than a picture (`Ink::Stroke`).
_Avoid_: Path, line (alone), scribble

**Highlighter stroke**:
A stroke laid on as one shape, its colour see-through and no darker where the line crosses itself, as a highlighter pen's; ink, not a highlight of the book's words (`Ink::Stroke::highlighter`).
_Avoid_: Highlight (for ink), marker

**Ink batch**:
What an ink surface did between two pauses of the pen, strokes added and removed, kept as one change wherever its strokes are kept; erasing part of a stroke is its removal and its pieces' addition (`Ink::Change`).
_Avoid_: Diff, delta, update

**Recognised text**:
The words of handwriting as a vision model read them, kept with the strokes they came from by the hash of the ink and the model, so it is read once and found by search on every computer (`RecognitionStore`).
_Avoid_: OCR (alone), transcription (for ink)

**Transcript**:
The words of a recording, written out by voxtype or whisper.cpp on this computer or by a transcription server (`Recognition::Result`); each word with its moment where the way gives them (whisper.cpp and most servers do, voxtype doesn't: `timed`).
_Avoid_: Captions, subtitles

**Writing out**:
Making a recording's transcript through the speech-to-text capability, a piece of five minutes at a time, each kept as it is written out, so a long recording stops and goes on where it was (`Transcriber`, `TranscriptStore`). A recording in the library written out reads as a book (`RecordingBook`).
_Avoid_: Transcoding, dictation (that is #148's, as it is said)

**Own server**:
A server what is written and said may be sent to without asking: on this computer, on the tailnet, or at an address the person marked as theirs. Any other is hosted, and is sent it only where chosen twice over: allowed in the settings and asked for (`RecognitionRoute::whyNot`).
_Avoid_: Local server (for the tailnet), trusted server

**Lost annotation**:
An annotation whose passage can no longer be found in its book, kept until the reader places or removes it.
_Avoid_: Orphan, broken annotation

**Annotation layer**:
Someone else's annotations on a book, shown beside one's own, under the name they chose (`MemberNames`) and in their colour, and cut at one's own place: carried by a space whose annotations part is on, switched off one person at a time or all (`AnnotationLayers`).
_Avoid_: Shared highlights, overlay

**Part of a space**:
What a space holds beyond its collection and its books, switched on by name: its graph, questions, ratings, reading, files and annotations. What is one's own is off until one switches it on (`SpaceParts`).
_Avoid_: Section, feature, scope (for this meaning)

**Private book**:
A book of a kind its plugin keeps the person's own (a notebook, `"private"` in its plugin.json): in their own collections and on their own computers, and in no shared or published space unless they let that one book go (`PrivateBooks`).
_Avoid_: Hidden book, secret book, personal book

**Place**:
A point in a book in the book's own terms: its section (a chapter, or a page of a PDF or comic) and how far into it, so it is the same text in any window. Where a book was read to, where the graph met a thing, and where a bookmark is are all places (`BookPlace`).
_Avoid_: Position (alone), location, offset

**Passage**:
A stretch of a book's text that an annotation is attached to: two places and the words between, with a few before and after, found again by its words when the book's layout changes (`Passage`).
_Avoid_: Selection, range, quote, excerpt (the text the graph is built from)

**Stretch**:
A run of about a hundred and sixty words of a book's text, sharing its first and last forty with the stretches beside it, within one section: what the meaning index keeps and a search by meaning finds (`TextStretches`).
_Avoid_: Chunk, passage (an annotation's), excerpt (the graph's)

**Meaning index**:
A book's stretches, each with the embedding a model on the person's own server gave it, kept in the library beside the book and never in the change log; made only for the books asked for (or every book, by a setting) and made again when the book's file changes (`MeaningIndex`).
_Avoid_: Vector store, semantic index, embeddings (for the whole)

## Collections and the graph

**Tenant**:
A top-level collection's own part of the knowledge graph, kept entirely apart from every other: nothing in it is shared with, merged into or read from another tenant.
_Avoid_: Side, book side, partition, scope (for this meaning)

**Tenant membership**:
Which tenants see a thing, a fact or a relation of the graph, each stored once: those of the books it was learned from.
_Avoid_: Copy, partition, side

**Unfiled**:
The tenant of the books that are in no top-level collection.
_Avoid_: None, default, uncategorised

**Timeline**:
One order of story time that events are on in the chronology: a book's own, a series', a world's, real history (one for the whole library), one the reader names, or a branch. Two books share a timeline when their events are on the same one (`Chronology::Whose`).
_Avoid_: Reveal history, "When it was revealed" (the order a reader learned things in), track

**Branch**:
A timeline that is the same as another, its parent, up to an event and its own after it: the events before the split are shared with the parent, those after are the branch's alone. A branch can branch again (`Chronology::Split`, ADR 0015).
_Avoid_: Fork, copy, version (for the timeline; a Storyteller story's branch is a book, whose timeline is a branch)

**Alternate history**:
A branch of real history: a book's world that is ours up to an event and goes otherwise after it. Its events are never on real history.
_Avoid_: Fiction on real history, counterfactual

**Split**:
The event at which a branch departs from its parent, with the place in a book where that was revealed; neither it nor what comes after shows before it is read.
_Avoid_: Divergence point, fork point, branch point

**Proposed branch**:
A branch the AI recognised while building a book's graph, waiting for the reader to take it or not; until taken it is no timeline, and its events stay in their book's own order (`Chronology::Standing`).
_Avoid_: Suggestion, guess, pending timeline

## AI

**Capability**:
One kind of thing Omabook asks a model to do, chosen on its own: answers, the quick model, handwriting, speech to text, text to speech or embeddings. Each has its own provider and model, and its own say over whether a hosted one may be used (`Capability`, resolved by `AiRegistry`, ADR 0008).
_Avoid_: Feature, tier (for all six), task, skill

**Provider**:
What does a capability's work: a service (Claude's API, Gemini's, OpenAI's), a subscription through its agent (Claude Code, Antigravity), a server (Ollama, Omarchy's local models, another of the OpenAI-compatible API), a program on this computer (voxtype, whisper.cpp, Piper, speech-dispatcher) or a command; the person's own or hosted, as an own server is (`AiProvider`, ADR 0008).
_Avoid_: Backend, engine (alone), vendor, model (what a provider runs)

**Fallback**:
The second provider a capability uses while its first server can't be reached at all (off, or the laptop away from home), never when it answers with an error: this computer, or another server chosen in its row, by the same privacy rule, with a hosted one needing its own allowance. Omabook goes back to the first by itself once it answers; quick work has none and waits for its server (`AiFallback`, `ServerReach`, #174).
_Avoid_: Failover, backup server, retry (that is another model on the same provider)

**Dictation**:
Saying words into a text field instead of typing them: heard while the person holds the field's microphone or its key (or between two presses), written out by speech to text, and put where the cursor is; or voxtype's own dictation, which types them itself (`Dictation`, #148). A recording is something kept; dictation keeps nothing but the words.
_Avoid_: Recording (for this), voice input, speech recognition (for the whole of it)

**Spoken answer**:
An AI's answer read aloud, to a question asked by dictation: the companion's or one about the library, read by the voice chosen for text to speech and stopped by asking again (`SpokenAnswer`, #149).
_Avoid_: Voice mode, voice assistant, reading aloud (that is a book's)

**Voice command**:
A command said while reading ("next chapter", "go to chapter 5"), heard by dictation and understood by a grammar of the command table in the window's five languages, or by the model for quick work where the grammar doesn't know it (`VoiceGrammar`, `VoiceCommands`, #150). It runs what the command's key runs, or one of the three only said: a chapter by its number, highlighting the selection, and what a word means.
_Avoid_: Voice control, wake word, speech command

## Phones and e-readers

**Serving**:
Offering the library to phones and e-readers over the network: the OPDS catalog that their apps browse and download from, and KOReader's progress sync, each when switched on, only where the person allowed. Off until chosen.
_Avoid_: Sharing (that is publishing a collection), hosting, exposing

**Reach**:
Where Omabook serves: this computer only, the home network (every network the computer is on, unencrypted), or the tailnet. Set in Phones and e-readers or with `omabook serve --reach` (`DeviceReach`).
_Avoid_: Scope, interface, bind address

**Catalog token**:
The password an app gives the catalog, made once and kept in the keyring, also put in the catalog's address (`/t/<token>/opds`) for an app that takes no password. A new one locks out every app with the old one.
_Avoid_: API key, PIN

**Background service**:
The person's own systemd user service, `omabook-serve.service`, that serves with the window closed, from login on; while it is chosen the window leaves serving to it. Which Omabook serves (the window, the background service, or `omabook serve` in a terminal) each says when asked from this computer (`DeviceServing`).
_Avoid_: Daemon (the hub's), server, system service

## Hub and backups

**Hub**:
An Omabook without a window that is always on and belongs to one person: it keeps their libraries for their computers to sync with, serves them to phones and e-readers, does their background work (the knowledge graph, the meaning index, writing out recordings) and backs them up. Managed only from that person's own computers.
_Avoid_: Server (an own server is an AI's), headless Omabook, NAS, cloud

**Snapshot**:
A library as it stood at one moment, kept so it can be gone back to: encrypted, read-only, and one of a series. Only snapshots count as copies of a library; a synced library does not, since a sync carries a mistake to every computer.
_Avoid_: Backup (for a single copy), sync, mirror, replica

**Moment snapshot**:
A snapshot taken just before something that changes much at once (a sync, an import, a tidy, a restore, an upgrade, a bulk change by an agent) or when the person asks, kept a short while and then thinned like the others.
_Avoid_: Checkpoint, restore point, undo

**Library diff**:
What changed in a library between two snapshots, or a snapshot and now, told as the person sees the library: books added or removed, details before and after, notes, highlights and ink, graph facts, notebook pages. Not a list of changed files.
_Avoid_: Changelog, file diff, delta

**Destination**:
Where a hub keeps its snapshots: a second disk of the hub, another computer over SFTP, a storage service, or another of the person's hubs. Three copies on two kinds of medium with one away from home count as backed up.
_Avoid_: Target, remote, repository (restic's word for what is kept there)
