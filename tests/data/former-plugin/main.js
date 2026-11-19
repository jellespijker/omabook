// A plugin with an entry on the shelf and a form of every kind of field (layout_tests).
omabook.ui.launcher({id: "new", title: "Write a new story", author: "Storyteller", open: function () {}});
omabook.commands.register({id: "form", title: "A form", place: "global", run: function () {
    return omabook.ui.form({title: "Write a new story", accept: "Start Writing",
        intro: "Storyteller writes it for you, with the AI set up in Omabook. Chapter one is ready in a few minutes.",
        fields: [
            {id: "wish", type: "textarea", label: "What should it be?", placeholder: "A mood, a kind of story, a what-if.", required: true},
            {id: "where", type: "choice", label: "Where does it live?", choices: [
                {value: "new", label: "A world of its own", help: "A new world, new people, a new plot."},
                {value: "world", label: "A new story in the world of one of yours", help: "The world as you know it from that book, new people and a new plot."},
                {value: "sequel", label: "A sequel to one that has ended"},
                {value: "branch", label: "A branch of one, from a chapter"}]},
            {id: "skills", type: "multi", label: "Write it with", choices: ["Lush and lyrical", "Spare and concrete", "Wry", "Slow dread",
                "Chapter hooks", "Second person", "Present tense", "Kishotenketsu", "Heist", "Fair-clue mystery"]},
            {id: "pov", type: "choice", label: "Point of view", choices: ["Let the story decide", "First person", "Close third person"]},
            {id: "tone", type: "text", label: "Tone", placeholder: "Warm, eerie, wry, tender…"},
            {id: "chapters", type: "number", label: "Chapters", min: 2, max: 60, value: 12, help: "Where the plan aims."},
            {id: "words", type: "number", label: "Words a chapter", min: 150, max: 6000, step: 50, value: 1200},
            {id: "many", type: "choice", label: "Which story?", choices: ["One", "Two", "Three", "Four", "Five", "Six", "Seven"]},
            {id: "ahead", type: "toggle", label: "Write the next chapter as I read"}]});
}});
