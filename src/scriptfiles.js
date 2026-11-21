// `omabook.files`, what of `omabook.ui` reaches the person's files and
// clipboard (#131), and `omabook.pdf` (#138), as a plugin's script sees them,
// built by ScriptFiles (scriptfiles.cpp) from its native side, which is an
// argument here and never reachable from the script. scriptapireach.cpp and
// scriptapiui.cpp describe it.
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
    function kindsOf(options) {
        var kinds = options.kinds === undefined || options.kinds === null ? [] : options.kinds;
        if (typeof kinds === "string")
            kinds = [kinds];
        if (!Array.isArray(kinds))
            throw new TypeError("kinds should be a list, such as [\"image\", \".pdf\"].");
        return JSON.stringify(kinds.map(String));
    }
    function number(value, otherwise) {
        var made = Number(value);
        return value === undefined || value === null || !isFinite(made) ? otherwise : made;
    }

    // A path's points, given as [x, y, x, y, ...], [[x, y], ...] or [{x, y}, ...], as the first.
    function flat(points) {
        if (!Array.isArray(points))
            throw new TypeError("A path's points should be a list: [x, y, x, y, ...], [[x, y], ...] or [{x, y}, ...].");
        var out = [];
        points.forEach(function (point) {
            if (Array.isArray(point))
                out.push(number(point[0], 0), number(point[1], 0));
            else if (point !== null && typeof point === "object")
                out.push(number(point.x, 0), number(point.y, 0));
            else
                out.push(number(point, 0));
        });
        if (out.length % 2 !== 0)
            throw new TypeError("A path's points come in twos, x and y.");
        return out;
    }
    function style(item, options) {
        ["color", "fill", "font"].forEach(function (key) {
            if (options[key] !== undefined && options[key] !== null)
                item[key] = String(options[key]);
        });
        ["alpha", "width", "height", "size", "x", "y"].forEach(function (key) {
            if (options[key] !== undefined && options[key] !== null)
                item[key] = number(options[key], 0);
        });
        return item;
    }

    // A PDF drawn page by page, written by Omabook when its bytes are asked for.
    function document(options) {
        options = options || {};
        var pages = [];
        var pictures = [];
        var size = {width: number(options.width, 595), height: number(options.height, 842)};
        function page(given) {
            given = given || {};
            var items = [];
            pages.push({width: number(given.width, size.width), height: number(given.height, size.height), items: items});
            return Object.freeze({
                path: function (points, look) {
                    look = look || {};
                    items.push(style({type: "path", points: flat(points), closed: look.closed === true}, look));
                },
                text: function (words, look) {
                    look = look || {};
                    items.push(style({type: "text", text: String(words), bold: look.bold === true, italic: look.italic === true},
                                     look));
                },
                image: function (picture, look) {
                    // Bytes, or something picked or pasted that has them.
                    var bytes = picture && typeof picture.bytes === "function" ? picture.bytes() : picture;
                    pictures.push(buffer(bytes, "The picture"));
                    items.push(style({type: "image", image: pictures.length - 1}, look || {}));
                }
            });
        }
        return Object.freeze({
            page: page,
            pages: function () { return pages.length; },
            bytes: function () {
                return settled(native.pdf(JSON.stringify({title: String(options.title || ""), pages: pages}), pictures));
            }
        });
    }

    var pdf = Object.freeze({create: document});

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

    var made = {files: undefined, ui: {}, pdf: pdf};
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
            usage: function () { return parse(native.usage()); },
            // Large files that travel to one's other computers only when opened there (#135).
            onDemand: function (path, onDemand) { return native.setOnDemand(aPath(path, "What travels on demand"), onDemand !== false); },
            status: function (path) { return parse(native.status(aPath(path, "The file"))); },
            fetch: function (path) {
                aPath(path, "The file");
                return new Promise(function (resolve, reject) {
                    native.fetch(path, function (here, why) {
                        if (here)
                            resolve(path);
                        else
                            reject(new Error(why));
                    });
                });
            }
        });
        made.ui.pickFile = function (options) {
            options = options || {};
            var kinds = kindsOf(options);
            var multiple = options.multiple === true;
            var answer;
            var promise = new Promise(function (resolve) { answer = resolve; });
            // Throws at once when the person did nothing of the plugin's just now.
            native.pick(kinds, multiple, function (picked) {
                var files = parse(picked).map(held);
                answer(multiple ? files : files[0] || null);
            });
            return promise;
        };
        made.ui.saveFile = function (options) {
            if (options === null || typeof options !== "object")
                throw new TypeError("saveFile takes {name, kinds, bytes} or {name, kinds, text}.");
            var kinds = kindsOf(options);
            var what = typeof options.text === "string" ? options.text : null;
            if (what === null) {
                if (options.bytes === undefined || options.bytes === null)
                    throw new TypeError("saveFile needs what to save: bytes (an ArrayBuffer or a typed array) or text.");
                what = buffer(options.bytes, "What to save");
            }
            var settle = {};
            var promise = new Promise(function (resolve, reject) { settle.resolve = resolve; settle.reject = reject; });
            // Throws at once when the person did nothing of the plugin's just now.
            native.save(String(options.name || ""), kinds, what, function (saved) { settle.resolve(parse(saved)); },
                        function (why) { settle.reject(new Error(why)); });
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
