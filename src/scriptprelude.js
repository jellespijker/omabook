// What a plugin's script sees: the `omabook` object, built here from the
// host, which is the C++ side of one plugin (ScriptHost in scriptplugin.cpp,
// and ScriptSharing in scriptsharing.cpp, which it hands over, for what goes
// into spaces, and what scriptassist.js builds from ScriptAi). The hosts are
// only ever reachable through these functions: the host is an argument of
// this one, never a global, so a script can call what is listed in
// scriptapi.cpp and nothing else. A test holds this object and that table to
// each other.
(function (host, info) {
    "use strict";

    var shared = host.sharing();
    var assisted = host.assistant();

    function may(permission) { return info.permissions.indexOf(permission) >= 0; }

    // What newer JavaScript has that this engine lacks and that costs a
    // few lines to give it: each only where it is missing, and none of them
    // showing up in a `for ... in`. (What can't be given this way, syntax
    // and regular expressions mostly, is listed in the guide.)
    function give(object, name, value) {
        if (!(name in object))
            Object.defineProperty(object, name, {value: value, writable: true, configurable: true, enumerable: false});
    }
    // Given once, here, apart from the object below.
    function giveWhatIsMissing() {
        give(Array.prototype, "flat", function (depth) {
            depth = depth === undefined ? 1 : Math.floor(depth);
            var out = [];
            (function flatten(list, left) {
                for (var i = 0; i < list.length; ++i) {
                    if (!(i in list))
                        continue;
                    if (Array.isArray(list[i]) && left > 0)
                        flatten(list[i], left - 1);
                    else
                        out.push(list[i]);
                }
            })(this, depth);
            return out;
        });
        give(Array.prototype, "flatMap", function (fn, self) { return Array.prototype.map.call(this, fn, self).flat(1); });
        give(Array.prototype, "at", function (index) { index = Math.trunc(index) || 0; return this[index < 0 ? this.length + index : index]; });
        give(Array.prototype, "findLast", function (fn, self) {
            for (var i = this.length - 1; i >= 0; --i)
                if (fn.call(self, this[i], i, this))
                    return this[i];
            return undefined;
        });
        give(String.prototype, "replaceAll", function (search, replacement) {
            if (search instanceof RegExp) {
                if (!search.global)
                    throw new TypeError("replaceAll must be called with a global RegExp");
                return this.replace(search, replacement);
            }
            // Split and join, so nothing in the search is taken for a pattern;
            // a function is called once for each place, as replace would.
            var parts = String(this).split(String(search));
            if (typeof replacement !== "function")
                return parts.join(String(replacement).replace(/\$\$/g, "$"));
            var out = parts[0], at = parts[0].length;
            for (var i = 1; i < parts.length; ++i) {
                out += String(replacement(String(search), at, String(this))) + parts[i];
                at += String(search).length + parts[i].length;
            }
            return out;
        });
        give(String.prototype, "at", function (index) { index = Math.trunc(index) || 0; return this[index < 0 ? this.length + index : index]; });
        give(String.prototype, "trimStart", function () { return this.replace(/^\s+/, ""); });
        give(String.prototype, "trimEnd", function () { return this.replace(/\s+$/, ""); });
        give(Object, "fromEntries", function (entries) {
            var out = {};
            Array.from(entries).forEach(function (entry) { out[entry[0]] = entry[1]; });
            return out;
        });
        give(Promise, "allSettled", function (promises) {
            return Promise.all(Array.from(promises).map(function (one) {
                return Promise.resolve(one).then(function (value) { return {status: "fulfilled", value: value}; },
                                                 function (reason) { return {status: "rejected", reason: reason}; });
            }));
        });
    }
    giveWhatIsMissing();

    function need(value, type, what) {
        if (typeof value !== type)
            throw new TypeError(what + " should be a " + type + ".");
    }
    function oneOf(value, values, what) {
        if (values.indexOf(value) < 0)
            throw new RangeError(what + " is \"" + value + "\"; it should be one of: " + values.join(", ") + ".");
    }
    function text(values) {
        return Array.prototype.map.call(values, function (value) {
            if (typeof value === "string")
                return value;
            if (value instanceof Error)
                return String(value);
            try { return JSON.stringify(value); } catch (error) { return String(value); }
        }).join(" ");
    }

    // A function the script hands to a promise or a timer runs later, from
    // the event loop, where nothing of Omabook's is on the stack to time it.
    // So each is wrapped: the host starts its watchdog on the way in.
    function guarded(fn) {
        if (typeof fn !== "function")
            return fn;
        return function () {
            host.enter();
            try {
                return fn.apply(this, arguments);
            } catch (error) {
                host.caught(error);
                throw error;
            } finally {
                host.leave();
            }
        };
    }
    var then = Promise.prototype.then;
    Promise.prototype.then = function (fulfilled, rejected) { return then.call(this, guarded(fulfilled), guarded(rejected)); };
    Promise.prototype["catch"] = function (rejected) { return this.then(undefined, rejected); };
    give(Promise.prototype, "finally", function (after) {
        return this.then(function (value) { return Promise.resolve(after()).then(function () { return value; }); },
                         function (reason) { return Promise.resolve(after()).then(function () { throw reason; }); });
    });

    // The plugin's own data in the change log (#76), on the function that
    // writes its log, as `omabook.log.set` and the rest.
    function changeLog(changes) {
        return {
            set: function (entity, id, fields) {
                need(fields, "object", "The fields");
                changes.set(String(entity), String(id), JSON.stringify(fields));
            },
            add: function (entity, id, set, items, removed) {
                need(items, "object", "The items");
                changes.add(String(entity), String(id), String(set), JSON.stringify(items), JSON.stringify(removed || []));
            },
            remove: function (entity, id, set, keys) {
                changes.remove(String(entity), String(id), set === undefined ? "" : String(set), JSON.stringify(keys || []));
            },
            read: function (entity, id) { return parse(changes.read(String(entity), String(id))); },
            list: function (entity) { return parse(changes.list(String(entity))); }
        };
    }

    function parse(json) { return JSON.parse(json); }

    var omabook = {
        plugin: Object.freeze(info.plugin),

        say: function (words) { host.say(String(words)); },
        log: Object.assign(function () { host.log("log", text(arguments)); }, changeLog(shared.changeLog())),
        audio: shared.audio(),
        on: function (event, handler) {
            need(event, "string", "The event");
            need(handler, "function", "The handler");
            oneOf(event, info.events, "The event");
            host.on(event, handler);
        },
        off: function (event, handler) { host.off(String(event), handler); },
        after: function (milliseconds, fn) {
            need(milliseconds, "number", "The time to wait");
            need(fn, "function", "What to call");
            host.after(milliseconds, fn);
        },
        include: function (file) { return host.include(String(file)); },
        wait: function (work) {
            var done = function () { host.hold(-1); };
            host.hold(1);
            then.call(Promise.resolve(work), done, done);
            return work;
        },

        metadata: {
            register: function (source) {
                need(source, "object", "A source");
                need(source.name, "string", "The source's name");
                need(source.search, "function", "The source's search");
                host.registerMetadata(source.name, source.fields || [], function (book) { return source.search(book); });
            }
        },
        catalog: {
            register: function (catalog) {
                need(catalog, "object", "A catalog");
                need(catalog.format, "string", "The catalog's format");
                need(catalog.make, "function", "The catalog's make");
                host.registerCatalog(catalog.format, String(catalog.description || ""), function (books) { return catalog.make(books); });
            }
        },
        sharing: {
            register: function (sharing) {
                need(sharing, "object", "What a plugin shares");
                need(sharing.worlds, "function", "Its worlds");
                shared.registerWorlds(function (context) { return sharing.worlds(context); });
            }
        },
        spaces: {
            add: function (space, id) {
                need(space, "string", "The space");
                shared.addToSpace(space, Number(id) || 0);
            }
        },
        importer: {
            register: function (importer) {
                need(importer, "object", "An importer");
                need(importer.name, "string", "The importer's name");
                need(importer.added, "function", "The importer's added");
                host.registerImporter(importer.name, function (book) { return importer.added(book); });
            }
        },
        commands: {
            register: function (command) {
                need(command, "object", "A command");
                need(command.id, "string", "The command's id");
                need(command.title, "string", "The command's title");
                need(command.run, "function", "The command's run");
                if (!/^[a-z0-9_-]+$/.test(command.id))
                    throw new RangeError("A command's id is lower-case letters, digits, - and _; this one is \"" + command.id + "\".");
                oneOf(command.place || "library", info.commandPlaces, "The command's place");
                oneOf(command.when || "always", ["always", "generated"], "The command's when");
                host.registerCommand(command.id, command.title, command.place || "library", String(command.key || ""),
                                     command.when || "always", function (context) { return command.run(context); });
            },
            setEnabled: function (id, enabled) { host.setCommandEnabled(String(id), !!enabled); }
        },
        format: {
            register: function (format) {
                need(format, "object", "A format");
                need(format.name, "string", "The format's name");
                need(format.open, "function", "The format's open");
                if (!Array.isArray(format.extensions) || format.extensions.length === 0)
                    throw new TypeError("The format's extensions should be a list with at least one in it, such as [\"txtbook\"].");
                var extensions = format.extensions.map(function (extension) { return String(extension).toLowerCase().replace(/^\./, ""); });
                extensions.forEach(function (extension) {
                    if (!/^[a-z0-9][a-z0-9.]*$/.test(extension))
                        throw new RangeError("An extension is letters and digits, as in \"txtbook\"; this one is \"" + extension + "\".");
                });
                if (format.claims !== undefined && format.claims !== null)
                    need(format.claims, "function", "The format's claims");
                host.registerFormat(format.name, extensions,
                                    format.claims ? function (source) { return !!format.claims(source); } : null,
                                    function (source) { return format.open(source); });
            }
        },
        markdown: {
            toHtml: function (words) { return host.markdownToHtml(String(words)); }
        },
        ui: {
            panel: function (panel) {
                need(panel, "object", "A panel");
                need(panel.source, "string", "The panel's source");
                oneOf(panel.place || "library.side", info.panelPlaces, "The panel's place");
                oneOf(panel.when || "always", ["always", "generated"], "The panel's when");
                host.registerPanel(panel.place || "library.side", String(panel.title || info.plugin.name),
                                   Number(panel.width) || 280, panel.source, panel.when || "always");
            },
            setVisible: function (source, visible) { host.setPanelVisible(String(source), !!visible); },
            showGraph: function (what) {
                what = what || {};
                host.showGraph(String(what.scope || ""), Number(what.book) || 0);
            },
            set: function (key, value) { host.uiSet(String(key), JSON.stringify(value === undefined ? null : value)); },
            ask: function (question) {
                question = typeof question === "string" ? {title: question} : question;
                need(question, "object", "The question");
                need(question.title, "string", "The question's title");
                return new Promise(function (resolve) {
                    host.uiAsk(question.title, String(question.placeholder || ""), String(question.text || ""), resolve);
                });
            },
            form: function (form) {
                need(form, "object", "The form");
                // Functions and the like don't cross; what does is checked
                // now, so a form that can't be drawn is a mistake said at once.
                var json = JSON.stringify(form);
                var problem = host.formProblem(json);
                if (problem)
                    throw new TypeError(problem);
                return new Promise(function (resolve) {
                    host.uiForm(json, function (answer) { resolve(answer === null ? null : parse(answer)); });
                });
            },
            launcher: function (launcher) {
                need(launcher, "object", "A launcher");
                need(launcher.id, "string", "The launcher's id");
                need(launcher.title, "string", "The launcher's title");
                need(launcher.open, "function", "The launcher's open");
                if (!/^[a-z0-9_-]+$/.test(launcher.id))
                    throw new RangeError("A launcher's id is lower-case letters, digits, - and _; this one is \"" + launcher.id + "\".");
                if (launcher.removed !== undefined && launcher.removed !== null)
                    need(launcher.removed, "function", "The launcher's removed");
                host.registerLauncher(launcher.id, launcher.title, String(launcher.author || info.plugin.name),
                                      String(launcher.description || ""), String(launcher.cover || ""),
                                      function (context) { return launcher.open(context); },
                                      launcher.removed ? function (context) { return launcher.removed(context); } : null);
            },
            removeLauncher: function (id) { host.removeLauncher(String(id)); }
        },

        library: {
            list: function () { return parse(host.libraryBooks("")); },
            search: function (query) { return parse(host.libraryBooks(String(query || ""))); },
            get: function (id) { return parse(host.libraryBook(Number(id) || 0)); },
            setMeta: function (id, details) {
                need(details, "object", "The details");
                return parse(host.librarySetMeta(Number(id) || 0, JSON.stringify(details)));
            },
            setReading: function (id, reading) {
                need(reading, "object", "The reading");
                host.librarySetReading(Number(id) || 0, JSON.stringify(reading));
            },
            setIdentifier: function (id, scheme, value) { host.librarySetIdentifier(Number(id) || 0, String(scheme), String(value || "")); },
            addEntry: function (details, paper) {
                need(details, "object", "The details");
                return host.libraryAddEntry(JSON.stringify(details), !!paper);
            },
            notes: function (id, kind) { return parse(host.libraryNotes(Number(id) || 0, String(kind))); },
            setNotes: function (id, kind, words) { host.librarySetNotes(Number(id) || 0, String(kind), String(words)); },
            progress: function (id) { return parse(host.libraryProgress(Number(id) || 0)); },
            annotations: function (id) { return parse(host.libraryAnnotations(Number(id) || 0)); },
            toc: function (id) { return parse(host.libraryToc(Number(id) || 0)); },
            text: function (id, section) { return host.libraryText(Number(id) || 0, section === undefined ? -1 : Number(section)); },
            remove: function (id) { return host.libraryRemove(Number(id) || 0); },
            setProgress: function (id, place) {
                need(place, "object", "The place");
                return parse(host.librarySetProgress(Number(id) || 0, JSON.stringify(place)));
            }
        },

        graph: {
            find: function (words, options) {
                options = options || {};
                return parse(host.graphFind(String(words || ""), String(options.kind || ""), String(options.scope || ""),
                                            !!options.everywhere, Number(options.limit) || 50));
            },
            node: function (id) { return parse(host.graphNode(Number(id) || 0)); },
            around: function (id, depth) { return parse(host.graphAround(Number(id) || 0, depth === undefined ? 1 : Number(depth))); },
            ofBook: function (id, upTo) { return parse(host.graphOfBook(Number(id) || 0, JSON.stringify(upTo || {}))); },
            beyond: function (id, upTo) { return parse(host.graphBeyond(Number(id) || 0, JSON.stringify(upTo || {}))); },
            connect: function (id) {
                return new Promise(function (resolve, reject) { host.graphConnect(Number(id) || 0, resolve, reject); });
            },
            whole: function (scope, limit) { return parse(host.graphWhole(String(scope || ""), Number(limit) || 2000)); },
            upsert: function (thing) {
                need(thing, "object", "The node");
                return host.graphUpsert(JSON.stringify(thing));
            },
            link: function (source, target, relation, details) {
                return host.graphLink(Number(source) || 0, Number(target) || 0, String(relation || ""), JSON.stringify(details || {}));
            },
            unlink: function (edge) { return host.graphUnlink(Number(edge) || 0); },
            merge: function (keep, other) { return host.graphMerge(Number(keep) || 0, Number(other) || 0); },
            remove: function (id) { return host.graphRemove(Number(id) || 0); },
            scopes: function () { return parse(host.graphScopes()); },
            layers: function () { return parse(shared.layers()); },
            copyScope: function (from, to) { return host.graphCopyScope(String(from || ""), String(to || "")); },
            removeScope: function (scope) { return host.graphRemoveScope(String(scope || "")); },
            exported: function (format, what) { return host.graphExported(String(format || "json"), JSON.stringify(what || {})); }
        },

        reader: {
            current: function () { return parse(host.reading()); },
            open: function (id, place) {
                var at = place && place.section !== undefined;
                host.openBook(Number(id) || 0, at ? Number(place.section) || 0 : -1, at ? Number(place.fraction) || 0 : 0);
            },
            reload: function () { host.reloadBook(); }
        },

        settings: {
            get: function (key, fallback) {
                var kept = parse(host.settings());
                return Object.prototype.hasOwnProperty.call(kept, key) ? kept[key] : fallback;
            },
            set: function (key, value) { host.keep(String(key), JSON.stringify(value === undefined ? null : value)); },
            all: function () { return parse(host.settings()); },
            option: function (key) { return parse(host.options())[key]; },
            setOption: function (key, value) { return host.setOption(String(key), JSON.stringify(value)); }
        },

        // Asking the model, and recognising ink and speech (scriptassist.js).
        ai: assisted.ai,
        recognize: assisted.recognize
    };

    if (may("http")) {
        var request = function (method, url, body, options) {
            need(url, "string", "The address");
            var headers = (options && options.headers) || {};
            if (body !== null && typeof body === "object") {
                body = JSON.stringify(body);
                if (!headers["Content-Type"])
                    headers = Object.assign({"Content-Type": "application/json"}, headers);
            }
            return new Promise(function (resolve, reject) {
                host.http(method, url, body === null || body === undefined ? "" : String(body), JSON.stringify(headers),
                          function (answer) {
                              var response = parse(answer);
                              response.json = function () { return JSON.parse(response.text); };
                              resolve(response);
                          }, reject);
            });
        };
        omabook.http = {
            get: function (url, options) { return request("GET", url, null, options); },
            post: function (url, body, options) { return request("POST", url, body, options); }
        };
    }

    if (may("files")) {
        // Books the plugin makes come from files it wrote, so these go
        // with the permission to write them.
        omabook.library.create = function (details, file) {
            need(details, "object", "The details");
            need(file, "string", "The file");
            return parse(host.libraryCreate(JSON.stringify(details), file));
        };
        omabook.library.replaceFile = function (id, file) {
            need(file, "string", "The file");
            return parse(host.libraryReplaceFile(Number(id) || 0, file));
        };
        omabook.files = {
            read: function (path) { return host.fileRead(String(path)); },
            write: function (path, words) { return host.fileWrite(String(path), String(words)); },
            list: function (folder) { return parse(host.fileList(String(folder || "."))); },
            exists: function (path) { return host.fileExists(String(path)); },
            remove: function (path) { return host.fileRemove(String(path)); }
        };
    }

    if (may("run"))
        omabook.run = function (program, args, options) {
            need(program, "string", "The program");
            return new Promise(function (resolve, reject) {
                host.run(program, JSON.stringify(args || []), JSON.stringify(options || {}),
                         function (answer) { resolve(parse(answer)); }, reject);
            });
        };

    // The script can't take the object apart, by accident or otherwise.
    Object.keys(omabook).forEach(function (key) {
        if (omabook[key] !== null && (typeof omabook[key] === "object" || typeof omabook[key] === "function"))
            Object.freeze(omabook[key]);
    });
    Object.freeze(omabook);

    var console = Object.freeze({
        log: Object.assign(function () { host.log("log", text(arguments)); }, changeLog(shared.changeLog())),
        info: function () { host.log("log", text(arguments)); },
        warn: function () { host.log("warn", text(arguments)); },
        error: function () { host.log("error", text(arguments)); }
    });

    return {
        omabook: omabook,
        console: console,
        // A book's files, for a format's claims and open: that one book and
        // nothing else, and only while the call it was made for runs.
        source: function (token, name, folder) {
            return Object.freeze({
                name: name,
                isFolder: folder,
                entries: function () { return parse(host.bookEntries(token)); },
                contains: function (file) { return parse(host.bookEntries(token)).indexOf(String(file)) >= 0; },
                text: function (file) { return host.bookText(token, String(file)); },
                bytes: function (file) { return host.bookBytes(token, String(file)); },
                resolve: function (from, relative) { return host.bookResolve(String(from), String(relative)); }
            });
        },
        // What open answered, as JSON; a promise is not a book.
        book: function (value) {
            if (value !== null && typeof value === "object" && typeof value.then === "function")
                throw new TypeError("open should answer the book itself, not a promise of it: a book's files are read at once.");
            if (value === null || typeof value !== "object" || !Array.isArray(value.chapters))
                throw new TypeError("open should answer {title, authors, ..., chapters: [{title, html | markdown | text}]}.");
            return JSON.stringify(value);
        },
        // Whatever a call into the script answers, a value or a promise of
        // one, is handed back to the host when it is there.
        settle: function (token, value) {
            then.call(Promise.resolve(value),
                      function (result) { host.settled(token, true, JSON.stringify(result === undefined ? null : result), null); },
                      function (error) { host.settled(token, false, "", error); });
        }
    };
})
