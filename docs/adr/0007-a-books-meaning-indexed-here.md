# 7. A book's meaning is indexed on each computer, beside the library, and measured whole

- Status: accepted (#72)
- Date: 2026-11-21
- Builds on: #32 (the AI server), #84 (own servers), #102 (the word index), ADR 0003 (tenants)

## Context

A search by what the words mean ("where do they cross the river?") needs an embedding of the book's
text: a vector for each stretch of it, made by a model, compared with the question's. Where those vectors
are kept, who makes them, what they may be sent to and how they are searched decide how much space,
time and privacy it costs.

## Decision

- **Kept in the library, never in the change log** (format 40): `meaning_books` (a book asked for, the
  hash of the file and the model its stretches came from), `meaning_stretches` (each stretch's place,
  words and vector, as 32-bit floats of length 1) and `meaning_words` (FTS5 over the stretches' words).
  Like the tenants' tables they are derived: each computer makes its own from its own file with its own
  model, so a log of other people's spaces never carries a book's text, and a stale index is one whose
  hash or model differs, made again in the background.
- **Only what is asked for**: a book is indexed when the reader asks (Find, `omabook meaning index`,
  `meaning_index`) and kept up to date from then on; every book only by the setting `meaning.all`.
- **Made by the person's own server**: Ollama's OpenAI-compatible `/embeddings` (or `meaning.url`), with
  `nomic-embed-text` unless another model is chosen; the privacy rule of #84 (this computer, the tailnet,
  an address marked as one's own), and a hosted server only with `meaning.hosted`, said before the text
  is sent.
- **Measured whole**: a search reads the vectors of the books asked about and takes the dot product of
  each with the question's. 20,000 stretches of 768 dimensions (some hundred books) are measured in 44 ms
  on the build machine, and stored in 0.75 s (meaning_tests); no vector index is kept.
- **Meaning and words together**: the stretches nearest in meaning and those holding every word typed
  (BM25 over `meaning_words`) are put together by reciprocal rank fusion; every word rather than any,
  since a question in plain words has words every stretch has.
- **Never past the reader's place**: a stretch is kept only when it ends no further than the furthest the
  book was read (`GraphContext::reachOf`), unless the whole book is asked for.

## Considered options

- **sqlite-vec**: an index of vectors in SQLite. Not in Qt's SQLite or Arch's, and a whole library's
  stretches are measured fast enough without it; to be weighed again for libraries indexed whole.
- **Vectors in the log**: one index for every computer, but megabytes per book in an append-only log,
  and only right for one model.
- **Any word (OR) in the word half**: lifts every stretch with "the", and drowns the meaning.

## Consequences

- An index costs about 3 KB per stretch at 768 dimensions: some 1.5 MB for a novel.
- A search over a tenant's books grows with the stretches it measures; when whole libraries are indexed,
  an index of vectors (or vectors of 8 bits) is the next step.
