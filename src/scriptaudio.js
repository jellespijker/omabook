// `omabook.audio` (#79), as a plugin's script sees it, built by ScriptAudio
// (scriptaudio.cpp) from its native side, which is an argument here and
// never reachable from the script. scriptapireach.cpp describes it.
(function (native, mayRecord) {
    "use strict";

    function parse(json) { return JSON.parse(json); }
    function optional(value, what) {
        if (value !== undefined && value !== null && typeof value !== "function")
            throw new TypeError(what + " should be a function.");
        return value || null;
    }
    function aFile(file, what) {
        if (typeof file !== "string" || file === "")
            throw new TypeError(what + " should be a path in the plugin's folder, such as \"data/recordings/monday.opus\".");
        return file;
    }

    // A recording under way. What changes is asked for each time.
    function recording(id) {
        function now() { return parse(native.recordingState(id)); }
        var first = now();
        return Object.freeze({
            file: first.file,
            started: first.started,
            elapsed: function () { return now().elapsed || 0; },
            level: function () { return now().level || 0; },
            paused: function () { return now().paused === true; },
            recording: function () { return now().recording === true; },
            pause: function () { native.pause(id); },
            resume: function () { native.resume(id); },
            stop: function () { return parse(native.stop(id)); }
        });
    }

    function player(id) {
        function now() { return parse(native.playerState(id)); }
        var first = now();
        return Object.freeze({
            file: first.file,
            started: first.started,
            duration: first.duration,
            position: function () { return now().position || 0; },
            playing: function () { return now().playing === true; },
            play: function () { native.play(id); },
            pause: function () { native.pausePlaying(id); },
            seek: function (milliseconds) { native.seek(id, Number(milliseconds) || 0); },
            stop: function () { native.stopPlaying(id); }
        });
    }

    var audio = {
        play: function (file, options) {
            options = options || {};
            aFile(file, "The file to play");
            return player(native.open(file, Number(options.at) || 0, optional(options.onPosition, "onPosition"),
                                      optional(options.onEnded, "onEnded"), options.paused === true));
        },
        info: function (file) { return parse(native.info(aFile(file, "The file"))); }
    };
    if (mayRecord) {
        audio.record = function (options) {
            if (options === null || typeof options !== "object")
                throw new TypeError("record takes {file, onLevel, onStopped}.");
            var file = aFile(options.file, "The file to record into");
            var onLevel = optional(options.onLevel, "onLevel"), onStopped = optional(options.onStopped, "onStopped");
            native.check(file);
            return new Promise(function (resolve, reject) {
                native.ask(function (answer) {
                    if (answer === null) {
                        reject(new Error(native.refusal()));
                        return;
                    }
                    try {
                        resolve(recording(native.record(file, onLevel, onStopped)));
                    } catch (error) {
                        reject(error);
                    }
                });
            });
        };
    }
    return Object.freeze(audio);
})
