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
            ask: function (prompt, options) {
                need(prompt, "string", "The prompt");
                // The system prompt alone, as a string, is still understood.
                options = typeof options === "string" ? {system: options} : (options || {});
                if (options.onText !== undefined && options.onText !== null)
                    need(options.onText, "function", "onText");
                return new Promise(function (resolve, reject) {
                    assist.ask(prompt, String(options.system || ""), String(options.model || ""), options.onText || null,
                               resolve, reject);
                });
            }
        },

        recognize: {
            available: function (kind) { return assist.recognizes(String(kind || "")); },
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
            }
        }
    };
})
