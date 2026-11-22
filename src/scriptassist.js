// The parts of the `omabook` object that ask a model (#84): `omabook.ai` and
// `omabook.recognize`, built here from ScriptAi in scriptai.cpp, which is
// handed over as an argument and is never a global. scriptapiassist.cpp lists
// what is here, and a test holds the two to each other.
(function (assist) {
    "use strict";

    function need(value, type, what) {
        if (typeof value !== type)
            throw new TypeError(what + " should be a " + type + ".");
    }

    function hostedOf(options) {
        if (options === undefined || options === null)
            return false;
        need(options, "object", "The options");
        return options.hosted === true;
    }

    // What `ai.status` and `ai.ask` are asked with: the capability, the
    // model, and `hosted` only when it is said, true or false (left out, the
    // person's choice stands for answers and quick; the others keep to their own).
    function asking(options) {
        if (options === undefined || options === null)
            return {model: ""};
        need(options, "object", "The options");
        var chosen = {model: String(options.model || "")};
        if (options.capability !== undefined && options.capability !== null)
            chosen.capability = String(options.capability);
        if (options.hosted !== undefined && options.hosted !== null)
            chosen.hosted = options.hosted === true;
        return chosen;
    }

    // A promise of what was recognised, which cancel() stops. The options are
    // {language, hosted, again, onWaiting}.
    function recognition(start, options) {
        options = options === undefined || options === null ? {} : options;
        need(options, "object", "The options");
        if (options.onWaiting !== undefined && options.onWaiting !== null)
            need(options.onWaiting, "function", "onWaiting");
        var chosen = JSON.stringify({language: String(options.language || ""), hosted: options.hosted === true,
                                     again: options.again === true});
        var job = 0;
        var promise = new Promise(function (resolve, reject) {
            job = start(chosen, options.onWaiting || null, function (json) { resolve(JSON.parse(json)); }, reject);
        });
        promise.cancel = function () { return assist.cancel(job); };
        return promise;
    }

    return {
        ai: {
            available: function () { return assist.available(); },
            status: function (options) { return assist.status(JSON.stringify(asking(options))); },
            // A promise of the model chosen for a capability whose model the
            // server lacks (`status`'s `remedy`, #172): one there, or one
            // downloaded onto Ollama, telling onProgress(0 to 1).
            remedy: function (options) {
                options = options === undefined || options === null ? {} : options;
                need(options, "object", "The options");
                if (options.onProgress !== undefined && options.onProgress !== null)
                    need(options.onProgress, "function", "onProgress");
                var chosen = JSON.stringify(asking(options));
                return new Promise(function (resolve, reject) {
                    assist.remedy(chosen, options.onProgress || null, resolve, reject);
                });
            },
            ask: function (prompt, options) {
                need(prompt, "string", "The prompt");
                // The system prompt alone, as a string, is still understood.
                options = typeof options === "string" ? {system: options} : (options || {});
                if (options.onText !== undefined && options.onText !== null)
                    need(options.onText, "function", "onText");
                var chosen = asking(options);
                chosen.system = String(options.system || "");
                return new Promise(function (resolve, reject) {
                    assist.ask(prompt, JSON.stringify(chosen), options.onText || null, resolve, reject);
                });
            },
            // A promise kept when the text was read aloud, or stopped; stop() stops it.
            speak: function (text, options) {
                need(text, "string", "The text");
                options = options === undefined || options === null ? {} : options;
                need(options, "object", "The options");
                var chosen = JSON.stringify({language: String(options.language || ""), hosted: options.hosted === true});
                var promise = new Promise(function (resolve, reject) {
                    assist.speak(text, chosen, function () { resolve(); }, reject);
                });
                promise.stop = function () { assist.stopSpeaking(); };
                return promise;
            },
            stopSpeaking: function () { assist.stopSpeaking(); },
            // A promise of each text's embedding, a list of numbers of length 1.
            embed: function (texts, options) {
                if (typeof texts === "string")
                    texts = [texts];
                if (!Array.isArray(texts))
                    throw new TypeError("The texts should be a list of strings.");
                options = options === undefined || options === null ? {} : options;
                need(options, "object", "The options");
                var given = JSON.stringify(texts.map(String));
                var chosen = JSON.stringify({use: String(options.use || "document"), hosted: options.hosted === true});
                return new Promise(function (resolve, reject) {
                    assist.embed(given, chosen, function (json) { resolve(JSON.parse(json).vectors); }, reject);
                });
            }
        },

        recognize: {
            // A boolean, as it always was; status() says why not (#133).
            available: function (kind, options) { return assist.recognizes(String(kind || ""), hostedOf(options)); },
            status: function (kind, options) { return assist.recognition(String(kind || ""), hostedOf(options)); },
            ink: function (strokes, options) {
                if (strokes === null || typeof strokes !== "object")
                    throw new TypeError("The strokes should be a list of strokes, or the items of a set of them.");
                var given = JSON.stringify(strokes);
                return recognition(function (chosen, waiting, resolve, reject) {
                    return assist.ink(given, chosen, waiting, resolve, reject);
                }, options);
            },
            audio: function (file, options) {
                need(file, "string", "The recording");
                return recognition(function (chosen, waiting, resolve, reject) {
                    return assist.audio(file, chosen, waiting, resolve, reject);
                }, options);
            },
            // A recording written out whole, a piece at a time, which stop()
            // stops and a later call goes on with (#151). The options are
            // {language, hosted, again, onProgress(done, total), onWaiting}.
            transcript: function (file, options) {
                need(file, "string", "The recording");
                options = options === undefined || options === null ? {} : options;
                need(options, "object", "The options");
                if (options.onProgress !== undefined && options.onProgress !== null)
                    need(options.onProgress, "function", "onProgress");
                var job = 0;
                var promise = recognition(function (chosen, waiting, resolve, reject) {
                    job = assist.transcript(file, chosen, options.onProgress || null, waiting, resolve, reject);
                    return job;
                }, options);
                promise.stop = function () { return assist.stopTranscript(job); };
                return promise;
            }
        }
    };
})
