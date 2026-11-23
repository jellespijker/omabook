<!-- AI setup, per capability: https://jellespijker.github.io/omabook/docs/ai.html -->

# AI setup, per capability

Omabook asks a model to do six kinds of work. Each has its own provider and model, and its own say on whether a hosted one may be used. Open Settings with `,` and go to the AI page.

| Capability | What it does |
| --- | --- |
| Answers | The reading companion, catch-ups, who is who, asking about your library. |
| Quick work | Builds the knowledge graph, tidies the library, judges look-ups, understands voice commands the grammar doesn’t know. |
| Handwriting | Reads ink in margins and notebooks. A server of its own if you want one. |
| Speech to text | Dictation and recordings written out. voxtype first, then whisper.cpp, then a server. |
| Text to speech | Spoken answers and read-aloud, by the voice you choose. |
| Embeddings | Search by meaning across the library. |

## Start local

For a capability that takes a server, pick **Ollama** and leave the address as `http://localhost:11434/v1`. Ollama on another computer is the same row with another address. Omarchy’s local models, any server that speaks the OpenAI-compatible API, a hosted API (Claude, OpenAI, Gemini), a subscription through its agent (Claude Code, Antigravity) or any command are the other choices. *Automatic* follows the agent you picked with `omarchy default agent`.

```
Answers          Ollama   llama3.2           localhost:11434
Quick work       Ollama   qwen2.5:7b         localhost:11434
Handwriting      Ollama   qwen2.5vl:3b       localhost:11434
Speech to text   voxtype
Text to speech   Piper
Embeddings       Ollama   nomic-embed-text   localhost:11434
```

Example values; your rows name your own.

## Two models for text

*Answers* is for what you read: the companion, catch-ups, who is who, asking about your library. *Quick work* is for what Omabook reads into structure: growing the graph, tidying, judging look-ups, finding series, understanding voice commands the grammar does not know. That second kind is most of the calls, so a small model is a good fit.

## Speech and handwriting

Speech to text tries voxtype first, then whisper.cpp, then a server. Dictation listens only after you press the microphone (or hold `Ctrl+M`) and keeps nothing but the words. Voice commands work in English, German, French, Dutch and Spanish. Handwriting is read by a vision model on your AI server, or on a server of its own set in the Handwriting row. Recordings (MP3, M4B, FLAC and others) become books once written out, and are searchable by what is said.

## Test each row

Every row has a Test button, or `T` on the row. It sends one small real request through exactly that row’s provider, server, key and model: a two-word reply, a tiny drawn word read back, a short clip heard. If the model is missing, Omabook finds one that can do the job on the server, or offers a small known one to download with one press. Both are new in 0.4.0.

## Hosted only where you allow it

Each capability has its own switch for hosted providers. Off, nothing for that capability leaves your computer. A key is kept in the desktop’s keyring, never in `config.json`. With AI turned off nothing is asked and nothing is listened to.

What is sent: the question, the book’s title and authors, and the text of the part you ask about, to the AI chosen and nowhere else.
