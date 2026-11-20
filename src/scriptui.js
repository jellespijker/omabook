// `omabook.ui`: what a plugin puts in the window and asks there, built here
// from ScriptWindow (scriptwindow.cpp), which is handed over as an argument
// and is never a global; scriptprelude.js calls what this answers with the
// plugin's host, for the questions, and its own checks. The table in
// scriptapiui.cpp describes every function here.
(function (window) {
    "use strict";

    var named = /^[a-z0-9_-]+$/;

    return function (host, info, need, oneOf, parse) {
        return {
            panel: function (panel) {
                need(panel, "object", "A panel");
                need(panel.source, "string", "The panel's source");
                oneOf(panel.place || "library.side", info.panelPlaces, "The panel's place");
                oneOf(panel.when || "always", ["always", "generated"], "The panel's when");
                window.registerPanel(panel.place || "library.side", String(panel.title || info.plugin.name),
                                     Number(panel.width) || 280, panel.source, panel.when || "always");
            },
            setVisible: function (source, visible) { window.setPanelVisible(String(source), !!visible); },
            // A view of the plugin's own (#77): in place of the shelf, or beside
            // the page. It comes with a command for the shelf and one for the
            // page, by its id, which open it and put it away again; with its key.
            view: function (view) {
                need(view, "object", "A view");
                need(view.id, "string", "The view's id");
                need(view.source, "string", "The view's source");
                if (!named.test(view.id))
                    throw new RangeError("A view's id is lower-case letters, digits, - and _; this one is \"" + view.id + "\".");
                var title = String(view.title || info.plugin.name);
                window.registerView(view.id, title, Number(view.width) || 480, view.source);
                ["library", "reader"].forEach(function (place) {
                    host.registerCommand(view.id, title, place, String(view.key || ""), "always",
                                         function () { window.showView(view.id, "{}", true); });
                });
            },
            showView: function (id, args) {
                need(id, "string", "The view's id");
                window.showView(id, JSON.stringify(args === undefined || args === null ? {} : args), false);
            },
            closeView: function (id) { window.closeView(String(id)); },
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
                if (!named.test(launcher.id))
                    throw new RangeError("A launcher's id is lower-case letters, digits, - and _; this one is \"" + launcher.id + "\".");
                if (launcher.removed !== undefined && launcher.removed !== null)
                    need(launcher.removed, "function", "The launcher's removed");
                window.registerLauncher(JSON.stringify({id: launcher.id, title: launcher.title, author: String(launcher.author || info.plugin.name),
                                                        description: String(launcher.description || ""), cover: String(launcher.cover || "")}),
                                        function (context) { return launcher.open(context); },
                                        launcher.removed ? function (context) { return launcher.removed(context); } : null);
            },
            removeLauncher: function (id) { window.removeLauncher(String(id)); }
        };
    };
})
