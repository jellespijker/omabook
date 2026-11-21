// The part of the plugin API table (scriptapi.cpp) that asks a model:
// `omabook.ai`, and recognising handwriting and speech, `omabook.recognize`
// (#84). scriptassist.js builds them.

#include "scriptapi.h"

namespace ScriptApi {

QList<Entry> assistEntries() {
    return {
        {"ai.available", "ai.available()", "", "Whether a model is set up to ask."},
        {"ai.ask", "ai.ask(prompt, {system, onText, model})", "",
         "Asks the model the user chose in the settings; a promise of its whole answer as text, however long it takes "
         "to write. `onText(piece)` is called with each piece as it arrives. `model` is `\"main\"` (the default: the "
         "model for what a person reads), `\"quick\"` (the small, fast one, for reading text into structure) or a "
         "model's own name. (A string in place of the options is the system prompt.)"},

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
         "Recognised text from handwriting, read by a vision model on the AI server (config.json's "
         "`recognition.ink_model`, else the server's main model). `strokes` are the items of a set of strokes as "
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
    };
}

} // namespace ScriptApi
