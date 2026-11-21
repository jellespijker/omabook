// `omabook.files`, and what of `omabook.ui` reaches the person's files and
// clipboard (#131), as a plugin's script sees them, built by ScriptFiles
// (scriptfiles.cpp) from its native side, which is an argument here and never
// reachable from the script. scriptapireach.cpp and scriptapiui.cpp describe it.
(function (native, permissions) {
    "use strict";

    function may(permission) { return permissions.indexOf(permission) >= 0; }
    function parse(json) { return JSON.parse(json); }
    function aPath(path, what) {
        if (typeof path !== "string" || path === "")
            throw new TypeError(what + " should be a path in the plugin's folder, such as \"data/pictures/one.png\".");
        return path;
    }
    // What a script hands over as bytes, as one ArrayBuffer of its own.
    function buffer(data, what) {
        if (data instanceof ArrayBuffer)
            return data;
        if (ArrayBuffer.isView(data))
            return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
        throw new TypeError(what + " should be an ArrayBuffer or a typed array (such as a Uint8Array), or base64 text.");
    }
    function settled(value) { return value === undefined ? null : value; }

    // A file the person picked, or a picture from the clipboard: what it is,
    // and its contents through this alone.
    function held(info) {
        var id = info.id;
        return Object.freeze({
            name: info.name,
            size: info.size,
            type: info.type,
            width: info.width,
            height: info.height,
            bytes: function () { return settled(native.heldBytes(id)); },
            base64: function () { return settled(native.heldBase64(id)); },
            text: function () { return settled(native.heldText(id)); },
            copyTo: function (path) { return native.heldCopy(id, aPath(path, "Where to copy it")); }
        });
    }

    var made = {files: undefined, ui: {}};
    if (may("files")) {
        made.files = Object.freeze({
            read: function (path) { return native.read(String(path)); },
            readBytes: function (path) { return native.readBytes(String(path)); },
            readBase64: function (path) { return native.readBase64(String(path)); },
            write: function (path, words) {
                // Text as text; bytes as they are.
                if (words instanceof ArrayBuffer || ArrayBuffer.isView(words))
                    return native.writeBytes(String(path), buffer(words, "What to write"));
                return native.write(String(path), String(words));
            },
            writeBytes: function (path, data) {
                aPath(path, "The file");
                return typeof data === "string" ? native.writeBase64(path, data) : native.writeBytes(path, buffer(data, "What to write"));
            },
            list: function (folder) { return parse(native.list(String(folder || "."))); },
            exists: function (path) { return native.exists(String(path)); },
            remove: function (path) { return native.remove(String(path)); },
            usage: function () { return parse(native.usage()); }
        });
        made.ui.pickFile = function (options) {
            options = options || {};
            var kinds = options.kinds === undefined || options.kinds === null ? [] : options.kinds;
            if (typeof kinds === "string")
                kinds = [kinds];
            if (!Array.isArray(kinds))
                throw new TypeError("kinds should be a list, such as [\"image\", \".pdf\"].");
            var multiple = options.multiple === true;
            var answer;
            var promise = new Promise(function (resolve) { answer = resolve; });
            // Throws at once when the person did nothing of the plugin's just now.
            native.pick(JSON.stringify(kinds.map(String)), multiple, function (picked) {
                var files = parse(picked).map(held);
                answer(multiple ? files : files[0] || null);
            });
            return promise;
        };
    }
    if (may("clipboard")) {
        made.ui.clipboard = Object.freeze({
            readText: function () { return native.clipboardText(); },
            writeText: function (words) { return native.setClipboardText(String(words)); },
            readImage: function () {
                var info = parse(native.clipboardImage());
                return info ? held(info) : null;
            },
            // Bytes of a PNG or JPEG, or a picture of the plugin's folder by its path.
            writeImage: function (picture) {
                return typeof picture === "string" ? native.setClipboardImage(picture, new ArrayBuffer(0))
                                                   : native.setClipboardImage("", buffer(picture, "The picture"));
            }
        });
    }
    return made;
})
