// The part of the plugin API table (scriptapi.cpp) that asks a model:
// `omabook.ai`, and recognising handwriting and speech, `omabook.recognize`
// (#84). scriptassist.js builds them.

#include "scriptapi.h"

namespace {

// Recognising handwriting and speech, `omabook.recognize`.
QList<ScriptApi::Entry> recognizeEntries() {
    using ScriptApi::Entry;
    return {
        {"recognize.available", "recognize.available(kind, {hosted})", "",
         "Whether `\"ink\"` or `\"audio\"` can be recognised here, true or false: a server that may be asked is set "
         "up (a hosted one only with `hosted: true` and hosted servers allowed in the settings), or (for speech) "
         "whisper.cpp is installed. Never with the AI set to Off. `status` says why not."},
        {"recognize.status", "recognize.status(kind, {hosted})", "",
         "What `available` answers, with why not: `{available, why, message}`. `why` is a code: `\"ai-off\"` (the "
         "AI is set to Off: nothing is asked of any server), `\"no-server\"` (none is set up, nor whisper.cpp for "
         "speech), `\"hosted-not-allowed\"` (only someone else's computer, and hosted servers aren't allowed in "
         "the settings), `\"hosted-not-chosen\"` (allowed, but not asked for with `hosted: true`), `\"no-model\"` "
         "(the server was asked and lacks a model that reads handwriting, or transcribes; until the settings "
         "change or one is recognised), `\"unknown-kind\"` or `\"no-settings\"`; empty when available. "
         "`message` says it in the desktop's language."},
        {"recognize.ink", "recognize.ink(strokes, {language, hosted, again, onWaiting})", "",
         "Recognised text from handwriting, read by a vision model on the AI server, or on handwriting's own server "
         "(config.json's `recognition.ink_url`), with `recognition.ink_model`, else the server's main model. `strokes` are "
         "the items of a set of strokes as "
         "`omabook.log` keeps them (`{id: base64}`), or a list of strokes, each its base64 or `{points: [{x, y, "
         "pressure, ms}], width}`. A promise of `{kind, text, segments: [{text, start, end, words}], language, "
         "model, server, at, kept}`, which has `cancel()`. It runs in the background, one recognition at a time; "
         "while the server can't be reached it waits and tries again, telling `onWaiting(why)`. What was "
         "recognised is kept with the plugin's data in the change log, by the hash of the strokes and the model, "
         "so the same ink is read once, on whichever of your computers (`kept` says it came from there; `again: "
         "true` reads it anew). It goes only to your own servers (this computer, your tailnet, the addresses in "
         "`recognition.own`): a hosted one only with `hosted: true` *and* hosted servers allowed in the settings. "
         "A failure is said in plain words."},
        {"recognize.audio", "recognize.audio(file, {language, hosted, again, onWaiting})", "",
         "A transcript of a recording in the plugin's folder (`data/talk.opus`), each word with its moment in "
         "milliseconds (`segments[].words: [{text, start, end}]`): through the server's OpenAI-compatible "
         "`/audio/transcriptions` (`recognition.transcription_url` and `transcription_model`, else the AI server "
         "and `whisper-1`), else whisper.cpp on this computer (`whisper-cli`, a `ggml-*.bin` model). Answers, "
         "waits, is kept and keeps to your own servers as `ink` does."},
        {"recognize.transcript", "recognize.transcript(file, {language, hosted, again, onProgress, onWaiting})", "",
         "A recording in the plugin's folder written out whole, however long, through the speech-to-text capability "
         "(voxtype first, a hosted way only with `hosted: true` and hosted allowed for it, nothing with the AI Off), "
         "as `audio` answers it: `{text, segments, timed, language, model, server, at, kept}`, each word's moment "
         "from the start of the recording where the way gives them (`timed`). It is written out in the background "
         "five minutes at a time, one recording at a time for Omabook and every plugin, telling `onProgress(done, "
         "total)` of the pieces; each piece is kept on this computer as it is written out, so `stop()` on the "
         "promise (which is then rejected) loses at most the piece under way, and a later call goes on from there. "
         "Written out whole, it is answered from what was kept (`kept`); `again: true` writes it out anew."},
    };
}

} // namespace

namespace ScriptApi {

QList<Entry> assistEntries() {
    QList<Entry> entries{
        {"ai.available", "ai.available()", "", "Whether a model is set up to ask."},
        {"ai.status", "ai.status({capability, model, hosted})", "",
         "Whether a capability may be used, and where it goes (#138, ADR 0008): `{capability, available, hosted, provider, "
         "model, why, message}`. `capability` is `\"answers\"` (the default, or `\"quick\"` with the model `\"quick\"`), "
         "`\"quick\"`, `\"handwriting\"`, `\"speech-to-text\"`, `\"text-to-speech\"` or `\"embeddings\"`, each with the "
         "provider and model the person chose for it; a plugin never picks one. For answers and quick, "
         "`hosted` is true when it is someone else's computer: Claude's API, an agent (Claude Code, Codex, ...), a "
         "command of the person's own (where it goes can't be known), a server that is neither this computer, the "
         "tailnet nor an address in config.json's `recognition.own`, or a model that runs elsewhere (`...:cloud`); "
         "false for their own server with a model of its own. `hosted: false` asks for their own computers alone; "
         "true, or left out, takes the one they chose; for the other four only `hosted: true` asks for a hosted one, "
         "and only where the person allowed hosted for that capability. `why` is a code: `\"ai-off\"` (the AI is set "
         "to Off: nothing is asked of any model; a local voice still reads aloud), `\"no-server\"` (none is set up), "
         "`\"hosted-not-allowed\"` (only someone else's computer, and the person didn't allow hosted for it), "
         "`\"hosted-not-chosen\"` (someone else's, and the plugin kept to the person's own), `\"no-model\"`, "
         "`\"not-installed\"`, `\"no-key\"`, `\"unreachable\"` or `\"unknown-capability\"`; empty when available. "
         "`message` says it in the desktop's language. `model` is as for `ask`. Where the server lacks the model "
         "(`\"no-model\"`), `remedy` says what can be done, once the server has been looked at (an empty object "
         "until then, or when nothing can): `{action, model, size, server}`, `action` `\"use\"` (a model on the "
         "server that can do it, by what Ollama says it does or by its family) or `\"download\"` (a small, "
         "well-known one to download onto the Ollama server, of `size` bytes); `ai.remedy` does it (#172)."},
        {"ai.remedy", "ai.remedy({capability, onProgress})", "",
         "Does what `ai.status`'s `remedy` says, for the person who pressed the plugin's button for it: chooses the "
         "model on the server for the capability, or downloads the one suggested onto the Ollama server and then "
         "chooses it, telling `onProgress(fraction)` how far, 0 to 1. A promise of the model chosen; refused with why "
         "where there is none to find. Only model names go to the server."},
        {"ai.ask", "ai.ask(prompt, {system, onText, model, hosted})", "",
         "Asks the model the user chose in the settings; a promise of its whole answer as text, however long it takes "
         "to write. `onText(piece)` is called with each piece as it arrives. `model` is `\"main\"` (the default: the "
         "model for what a person reads), `\"quick\"` (the small, fast one, for reading text into structure) or a "
         "model's own name. `hosted: false` keeps it to the person's own computers: when the model they chose is someone "
         "else's, the promise is refused with `status`'s message and nothing is sent (`ai.status` says so first). "
         "(A string in place of the options is the system prompt.)"},
        {"ai.speak", "ai.speak(text, {language, hosted})", "",
         "Reads `text` aloud with the voice the person chose for text to speech (Piper or speech-dispatcher on this "
         "computer, or a server); a promise kept when it was read or stopped, which has `stop()`. A new one stops the "
         "one before. A server that is someone else's computer reads only with `hosted: true` and hosted allowed for "
         "reading aloud; with the AI set to Off only a voice on this computer reads."},
        {"ai.stopSpeaking", "ai.stopSpeaking()", "", "Stops what `ai.speak` is reading."},
        {"ai.embed", "ai.embed(texts, {use, hosted})", "",
         "The embeddings of a list of texts (or one), made by the provider the person chose for embeddings: a promise "
         "of a list of vectors, each a list of numbers of length 1, in the texts' order. `use` is `\"document\"` (the "
         "default) or `\"question\"`, which some models tell apart. Keep them in the plugin's own folder "
         "(`files.writeBytes`), never in its data in the change log (ADR 0007). A hosted provider only with `hosted: "
         "true` and hosted allowed for embeddings; never with the AI set to Off."},
    };
    entries.append(recognizeEntries());
    return entries;
}

} // namespace ScriptApi
