# 8. Each AI capability has its own provider, local unless the person allows a hosted one

- Status: accepted (#142)
- Date: 2026-11-21
- Builds on: #32 (the AI server), #72 (embeddings), #73 (reading aloud), #84 (recognition), #133 (AI Off),
  #138 (`ai.status`), [ADR 0005](0005-failures-are-codes-worded-where-shown.md) (codes),
  [ADR 0007](0007-a-books-meaning-indexed-here.md) (the meaning index)
- Served by: #143 (the registry), #144 (the Settings page), #145 (Gemini), #146 (voxtype), #147 (voices),
  #148 to #151 (dictation, voice, commands, transcripts)

## Context

Omabook asks models for six different things, and each grew its own way of being chosen:

- **Answers** and the **quick model** are one provider (`ai_provider`: `auto`, a harness, `anthropic`,
  `openai` with `ai_endpoint`, `command` or `off`) with two models (`ai_models`, `AiRequest::Tier`). `auto`
  follows Omarchy's default agent, so a new install sends book text to a hosted service before asking.
- **Handwriting** goes to a vision model on the AI server (`recognition.ink_model`), **speech** to a
  transcription server and then whisper.cpp (`recognition.transcription_url`, `whisper_program`), by the
  privacy rule of #84: own servers only, unless `recognition.hosted` *and* the one asking chose it.
- **Embeddings** go to the AI server or `meaning.url`, by the same rule with `meaning.hosted` (ADR 0007).
- **Reading aloud** is Piper, speech-dispatcher or an `/audio/speech` server (`read_aloud`), with
  `read_aloud.hosted`.

Three copies of the privacy rule (`RecognitionRoute::whyNot`, `AiReach::hosted`, `Voices`' server
check), four places a server is named, and one provider for answers whatever else is chosen. 0.3.0 adds
speech both ways (#146 to #151) and Gemini (#145): a person may want Claude to answer, Ollama to read
their handwriting, voxtype to hear them and Piper to read, and none of their recordings to leave the
computer.

## Decision

- **Six capabilities, each with its own provider and model**: `answers` (what a person reads: the
  companion, catch-ups, `omabook ask`, what a plugin writes), `quick` (reading and parsing into
  structure: the graph, look-ups, series), `handwriting` (a vision model reading ink, #84), `speech-to-text`
  (#84, #146), `text-to-speech` (#73, #147) and `embeddings` (#72). A request names its capability, as it
  names its tier today; `AiRequest::Tier` becomes the answers and quick capabilities.
- **Providers, and what each can serve**:

  | Provider | answers, quick | handwriting | speech to text | text to speech | embeddings | where |
  |---|---|---|---|---|---|---|
  | Claude API (`anthropic`) | yes | yes | | | | hosted |
  | Claude Code, Codex, Grok, Pi (harnesses) | yes | | | | | hosted |
  | Gemini API, with a key (#145) | yes | yes | yes | yes | yes | hosted |
  | Gemini through Antigravity's subscription (`agy`) | yes | | | | | hosted |
  | OpenAI | yes | yes | yes | yes | yes | hosted |
  | Ollama | yes | yes | | | yes | by its address |
  | Omarchy's local models | yes | where the model reads pictures | | | | this computer |
  | Another OpenAI-compatible server | yes | yes | yes | yes | yes | by its address |
  | voxtype (#146) | | | yes | | | this computer |
  | whisper.cpp | | | yes | | | this computer |
  | Piper, speech-dispatcher | | | | yes | | this computer |
  | A command (stdin to stdout) | yes | | | | | hosted |

  A server offers only what it is found to serve (its model list, its endpoints); a capability it lacks
  says so (`no-model`) rather than being guessed. Each provider is set up once (its address, program or
  key, keys in the keyring as today) and chosen by any capability it can serve.
- **One privacy rule, applied per capability**: a provider is the person's own (this computer, the
  tailnet, an address they marked as theirs) or hosted; a model whose name says it runs elsewhere
  (`...:cloud`) is hosted, and so is a command, since what it reaches can't be known. A hosted provider is
  used only when the person allowed hosted for **that capability**, said before anything is sent. Allowing
  Claude to answer allows nothing for handwriting or recordings.
- **Local by default**: a new install chooses, for each capability, the first own provider that is there:
  answers and quick Omarchy's local models, then Ollama; handwriting the same server's vision model;
  speech to text voxtype (Omarchy ships `voxtype-bin` in omarchy-pkgs; `voxtype transcribe` takes a WAV of
  16 kHz mono, converted to it first), then whisper.cpp; text to speech Piper with a voice, then
  speech-dispatcher; embeddings Ollama with `nomic-embed-text`. Where none is there the capability is
  unavailable and says what to install or choose; Omarchy's default agent is the first hosted choice
  offered for answers, never taken by itself.
- **Fallbacks stay on the same side**: a capability may list several providers, tried in order (voxtype,
  whisper.cpp, then a chosen server, #146), but never falls from an own provider to a hosted one unless
  hosted is allowed for it. Today's retry of a refused model (`modelToRetry`) stays within one provider.
- **AI Off stops every model, every sending and every analysis** (`ai_provider: "off"`, kept where it is
  so an older Omabook on the same file is off too): no capability has a provider, nothing is asked of any
  model on any server or program (voxtype and whisper.cpp included), nothing is sent, and nothing is read
  for meaning, handwriting or speech. A local voice that only reads the text is not AI: Piper and
  speech-dispatcher on this computer keep reading aloud, as they do today; a hosted voice stops.
- **One registry in the core** (#143) resolves a capability to a provider and model, and every use goes
  through it: the companion, the graph, recognition, the meaning index, reading aloud, the command line,
  the MCP server and plugins. A harness rule flags a provider made anywhere else. Its **status** per
  capability is `{available, hosted, provider, model, why}`, `why` a code worded where it is shown
  (ADR 0005): today's `ai-off`, `no-server`, `hosted-not-allowed`, `hosted-not-chosen`, `no-model`, and
  `not-installed`, `no-key`, `unreachable`.
- **A plugin may use all six capabilities** through `omabook.ai`, each through the registry with the
  provider and model the person chose: `ai.ask` for answers and quick (a model by name too), `recognize`
  for handwriting and speech to text (and dictation, #148; `omabook.recognize` stays as it is), `ai.speak`
  for text to speech, played by Omabook where the person can see and stop it, and `ai.embed` for
  embeddings, whose vectors the plugin keeps in its own folder, never in its data in the change log (ADR
  0007's reason). `ai.status({capability, model, hosted})` answers the registry's status for that
  capability; `recognize.status` stays, answering the same. A plugin never picks a provider.
- **What a plugin may send where**: to the person's own providers, whatever the capability serves; to a
  hosted one only when the person allowed hosted for that capability, and never more than they allowed
  (`hosted: false` keeps to their own). For answers and quick, where the plugin sends a prompt it wrote,
  leaving `hosted` out takes what the person chose, as #138 has it. For the other four, where it sends the
  person's own material whole (ink, recordings, the text of notes, what is read aloud), it must also ask
  for hosted (`hosted: true`), as #84 has it for recognition; without that a hosted provider is
  `hosted-not-chosen`. A private book's content (#127) goes hosted only by that same double choice.
- **AI Off for a plugin is AI Off**: every capability's status says `ai-off`, `ai.ask`, `recognize` and
  `ai.embed` refuse with it, and `ai.speak` reads only with a local voice, refusing a hosted one.
- **Today's settings carry over**: on the first start of a version with the registry, when config.json has
  no `capabilities`, it is written from the old keys, which are left as they are:
  - `ai_provider`, `ai_models`, `ai_endpoint`, `ai_command` become answers and quick, each with its model;
    `auto` becomes the provider "Omarchy's default agent", following it as today. A provider that is
    hosted gets hosted allowed for answers and quick, since that is where the text already went.
  - `recognition.ink_model` and `.hosted` become handwriting; `.transcription_url`, `.transcription_model`,
    `.whisper_program`, `.whisper_model` and `.hosted` become speech to text (voxtype is put first only
    where nothing was chosen); `recognition.own` becomes the list of own addresses for every capability.
  - `meaning.url`, `.model` and `.hosted` become embeddings (`meaning.all` stays the meaning index's).
  - `read_aloud`'s engine, voice, rate, program, server and `.hosted` become text to speech.

  Only a config.json written new starts local. Each person keeps the provider and models they use, and a
  test of #143 holds an old file to it.

## Considered options

- **One provider for everything** (today's answers): one hosted choice would take handwriting and
  recordings along, and only Gemini and OpenAI serve all six.
- **One "allow hosted" for all capabilities**: can't say "Claude may answer, my voice stays here", which is
  the choice people make.
- **Omarchy's default agent as the default for new installs** (today's `auto`): book text goes to a hosted
  service before the person was asked. It stays one choice away.
- **Local by default on upgrade too**: the companion would stop for everyone on Claude Code or the API the
  day they update, with nothing they changed.
- **A plugin choosing its provider**: the plugin would decide where the person's notes go.
- **Only answers and recognition for plugins** (0.2's API): a plugin that reads aloud or searches by
  meaning would run its own engine or server, outside the privacy rule and AI Off.
- **One hosted rule for every capability, answers included**: either every `ai.ask` of today's plugins
  would need `hosted: true`, or recordings and ink would go hosted on the person's choice alone.
- **One gateway (a LiteLLM-style proxy) in front of every provider**: another service to install and
  run, and voxtype, whisper.cpp, Piper and the harnesses aren't servers.
- **Hosted fallbacks by themselves** (try Gemini when Ollama is away): where text goes would change
  without asking.
- **AI Off also silencing Piper and speech-dispatcher**: a person who reads by ear would lose it, though a
  local voice that only reads the text is not AI.

## Consequences

- Settings has one AI page with six rows (#144), each with its provider, model, whether it may be hosted,
  and why it isn't available; the old dialog's AI part goes.
- The three copies of the privacy rule become the registry's one; `RecognitionRoute`, `AiReach`,
  `Embeddings::route` and `Voices::choose` ask it.
- A new provider is a row of what it serves and a class per capability it serves, tested against a fake
  (a server, a binary); a new capability (spoken answers, 0.4.0, uses text to speech) is a name and its
  providers.
- New installs answer only once a local model runs or the person picks a hosted one; the empty state has
  to say how, in one step.
- config.json keeps the old keys beside `capabilities` until a later version drops them; an older Omabook
  reads the old ones and ignores the new.
- Not every speech-to-text provider gives each word its moment (whether voxtype's `transcribe` does is
  #146's to find); a transcript without them says so (#146, #151), and whisper.cpp or a server is the
  choice where they are needed.

## As built in #143

Where the code asked for it, #143 differs from the decision above, as follows:

- **`capabilities` keeps what is new, and the providers stay set up where they are.** Each capability
  has `hosted` there, and answers and quick their `provider` (`ai_provider`'s words, with
  `"omarchy-default"` for `auto` and `"local"` for Omarchy's local models, then Ollama). The models stay
  in their keys (`ai_models`, `recognition.ink_model`, `.transcription_model`, `meaning.model`,
  `read_aloud.model`), and so do the other capabilities' providers (`recognition`, `meaning`,
  `read_aloud`): each provider is set up once, today's Settings dialog writes those keys, and an older
  Omabook reads them. The carry-over is accordingly only `hosted` (and answers' and quick's provider).
  #144 gives the other capabilities a `provider` of their own when the Settings page offers a choice.
- **The carry-over runs at start, not in Settings' upgrade.** It needs the privacy rule (is the server
  one's own?), which is above `foundation`; `AiPrivacy::carryOver` writes it when Omabook starts (the
  window and every command). A config.json that isn't there yet at that start is a new one, and starts
  local. Until a file is carried over (an older Omabook wrote it, or nothing has started since), the
  registry reads its capabilities from the older keys, with answers and quick going where they always did.
- **Quick work shares answers' provider** for now: one `AiProvider` serves both tiers, as today. Its own
  hosted choice holds all the same: where hosted is allowed for one and not the other, the registry's
  provider refuses the other's requests before anything is sent. A provider of quick's own is a change to
  `AiRegistry::answers` alone.
- **Today's Settings dialog** has no row per capability until #144: choosing an AI there chooses it for
  answers and quick and allows hosted for them, as choosing it always sent the text there; recognition's,
  the meaning index's and reading aloud's "hosted" switch their capability's.
- **Text to speech with the AI Off** is available with a local voice (its status says no `ai-off`), and
  `ai-off` where its voice is a server: a local voice only reads the text.
- **`ai.speak`** is read by Omabook, one at a time, and the plugin can stop it (`stop()`); showing it in
  the window, where the person can stop it too, is still to come.
- The registry is `AiRegistry` (`airegistry.h`), the privacy rule `AiPrivacy` (`aiprivacy.h`), and the
  names and codes `Capability` and `CapabilityLack` (`aicapability.h`). The harness rule is
  `rules/cpp/ai-only-through-the-registry.yml`.

## As built in #146

- **voxtype gives no word's moment.** `voxtype transcribe <file>` prints what it is doing, then the text
  after an empty line; it reads any WAV (resampling to 16 kHz mono itself), and a recording in another
  format is made one with ffmpeg first. A result says whether its words have their moments (`timed`), and
  the settings say it of the way in use.
- **Chosen before stays first.** Where config.json names a transcription server (`transcription_url`)
  or whisper.cpp (`whisper_program`, `whisper_model`), speech goes there first, as it did, and to
  voxtype last; otherwise voxtype, whisper.cpp, then the AI server. `voxtype_program` and
  `voxtype_model` sit beside them in `recognition`.
- **A voxtype that fails** (no model yet, say) hands the recording on to the next way, as a server
  without transcription does.
- **Installing it on Omarchy** is `omarchy-pkg-add voxtype-bin` and `voxtype setup --download` in a
  floating terminal (#156's), not Omarchy's dictation install, which would also make it the dictation.

## As built in #144

- **The AI page** (`AiSettingsDialog.qml`, a row per capability, its model `AiCapabilityRows`) shows each
  capability's status from the registry, its `why` worded in the window with what to do, and its choices.
- **Hosted is chosen in each row, never with a provider.** Choosing an AI on the page sets `ai_provider`
  and answers' and quick's `provider` and allows nothing (`Settings::chooseAiProvider`); `omabook ai use`
  still allows it for answers and quick, as typing it always sent the text there. The older keys'
  `hosted` (`recognition`, `meaning`, `read_aloud`) are written only as far as every capability they
  stand for allows it, and are followed in `capabilities` only when they are what changed.
- **Models and providers' set-up stay in their keys**, as #143 left them: each provider is set up once and
  an older Omabook on the same file reads them, and answers' and quick's models are each provider's own
  (`ai_models`), kept for when it is chosen again. `capabilities` gains a `provider` where the page offers a
  choice: speech to text (`"server"`, a transcription server then whisper.cpp, or `"whisper.cpp"` alone,
  which the registry follows) and text to speech (the `read_aloud` engine, followed there). Handwriting has
  the AI server only, and embeddings a server (the AI server unless an address is given), until Gemini,
  voxtype and more voices come (#145 to #147): each is an id more in the row's `offered`.
- **AI Off** is a switch at the top of the page; on again, answers come back to the provider
  `capabilities` kept for them.
