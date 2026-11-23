#include "manual.h"

// The keys that belong to one panel or dialog. They are not commands: they
// work only while that panel or dialog has the keyboard, and keys.json
// doesn't move them, so they are written here once, in the order the panel
// is used in, and the help (?), `omabook help keys` and the plugin manager's
// own list of keys all show them from here. A key that is a command (the
// graph's, the collections') is in the commands table, not here.
//
// The keys are named as the commands table names them ("Shift+Tab",
// "PgDown"), so they read the same wherever they are shown.
QList<Manual::Panel> Manual::panelList() { return readingPanels() + toolPanels() + libraryPanels(); }

// The page's own: what is selected, the contents, the Aa panel, the margin folded to dots and the ink.
QList<Manual::Panel> Manual::readingPanels() {
    return {
        {"reading_menu", tr("The menu beside selected words or a name"),
         {{"Up  Down  J  K", tr("Pick what to do")},
          {"Enter  Space", tr("Do it")},
          {"Escape", tr("Put it away")}}},
        {"selecting", tr("Selecting words with the keyboard"),
         {{"H  L  Left  Right", tr("Move a word on or back")},
          {"J  K  Up  Down", tr("Move a line down or up")},
          {"Shift", tr("Held with those, widens what is selected")},
          {"Enter  Space", tr("Show what to do with the words")},
          {"Ctrl+C", tr("Copy the words")},
          {"Escape  V", tr("Stop selecting")}}},
        {"contents", tr("The contents beside the page"),
         {{"Up  Down", tr("Move through the chapters and bookmarks")},
          {"Enter", tr("Go there")},
          {"Left  Right", tr("Fold or unfold a chapter's parts")},
          {"Delete", tr("Remove the selected bookmark")},
          {"Escape", tr("Close the contents")}}},
        {"format", tr("The Aa panel: how the text is set"),
         {{"Tab  Shift+Tab", tr("Go from one setting to the next")},
          {"Left  Right  -  +", tr("A step less or more")},
          {"Home  End", tr("The least or the most")},
          {"Escape", tr("Close the panel")}}},
        {"margin_dots", tr("The margin notes, folded to dots"),
         {{"Tab  Shift+Tab", tr("Go from one dot to the next")},
          {"Enter  Space", tr("Open the line's notes beside its words")},
          {"Escape", tr("Close them, the margin still folded")}}},
        {"ink", tr("Writing and drawing"),
         {{"Ctrl+Z", tr("Undo a pause of the pen at a time")},
          {"Ctrl+Shift+Z  Ctrl+Y", tr("Redo it")}}},
    };
}

// Asking, dictating, the lists, the AI page, the plugins, the tour and the help.
QList<Manual::Panel> Manual::toolPanels() {
    return {
        {"graph_details", tr("A thing's moments, in the graph's details"),
         {{"Tab  Shift+Tab", tr("Move through the moments")},
          {"Enter  Space", tr("Go to that moment in the text")},
          {"Escape", tr("Back to the graph")}}},
        {"ask", tr("Asking the AI, beside the page or about the library"),
         {{"Enter", tr("Ask")},
          {"Ctrl+M", tr("Held, or pressed once and again, asks by voice")},
          {"Escape", tr("Stop the spoken answer, else close the pane")}}},
        {"dictation", tr("Saying a note, or any text, instead of typing it"),
         {{"Ctrl+M", tr("Held while you speak, or pressed once to start and again to stop")},
          {"Escape", tr("Stop listening")}}},
        {"lists", tr("Lists to pick from: filters, orders, collections, menus, find, help"),
         {{tr("Typing"), tr("Narrows the list")},
          {"Up  Down", tr("Pick one")},
          {"Enter", tr("Take it")},
          {"Escape", tr("Close")}}},
        {"ai_page", tr("The AI page"),
         {{"Up  Down  J  K", tr("Move through the rows")},
          {"Enter", tr("Open or close the selected row")},
          {"T", tr("Test the row with one small real request")}}},
        {"model_finder", tr("Finding a model, in a row of the AI page"),
         {{"Enter", tr("Search for what is typed, or install the model whose Install has the focus")},
          {"Tab  Shift+Tab", tr("Go through the search, its buttons and each model's Install")}}},
        {"plugin_forms", tr("What a plugin asks, and installing one"),
         {{"Tab  Shift+Tab", tr("Go from one field to the next")},
          {"Enter", tr("Send it, or install the first that was found")},
          {"Escape", tr("Leave it unanswered")}}},
        {"plugins", tr("The plugin manager"),
         {{"Up  Down  J  K", tr("Move through the plugins")},
          {"Space", tr("Switch it on or off")},
          {"Enter", tr("Show its details")},
          {"/  Ctrl+F", tr("Find a plugin by name, kind or state")},
          {"N", tr("Start a plugin from a template")},
          {"I", tr("Install a plugin from a .zip or a folder")},
          {"T", tr("Try a look-up against a source")},
          {"Alt+Up  Alt+Down", tr("Ask a source earlier or later")},
          {"A", tr("Show the next plugin that needs attention")},
          {"U", tr("Check every plugin from GitHub for updates")},
          {"Shift+U", tr("Update this plugin from where it came from")},
          {"<  >", tr("Make the list narrower or wider")},
          {"O", tr("Open its folder, or the plugin folder")},
          {"R", tr("Read the plugin folder again, and ask Calibre again")},
          {"Shift+R", tr("Load this script again")},
          {"D  Delete", tr("Remove a plugin of your own (asks first)")},
          {"?", tr("Show or hide these keys")},
          {"Escape", tr("Back a step, then close")}}},
        {"tour", tr("The tour"),
         {{"Right  Enter  Space  J  L  N", tr("The next card")},
          {"Left  Backspace  H  K  P", tr("The card before")},
          {"Escape  Q", tr("Leave the tour")}}},
        {"help", tr("This help"),
         {{"Up  Down", tr("Pick a key or a page")},
          {"Enter", tr("Run it, or open the page")},
          {"Tab  Shift+Tab", tr("Turn to the next page or the one before")},
          {"Escape", tr("Empty the search, then close")}}},
    };
}

// The library's dialogs: tidying, the libraries, the hubs, a book's details and the reading sites.
QList<Manual::Panel> Manual::libraryPanels() {
    return {
        {"tidy", tr("Tidying the library"),
         {{"Up  Down", tr("Move through what was found")},
          {"Space", tr("Choose it, or leave it")}}},
        {"libraries", tr("The libraries"),
         {{"Enter", tr("Open the selected library")},
          {"Delete", tr("Forget the selected library")}}},
        {"hubs", tr("The hubs"),
         {{"Up  Down  J  K", tr("Move through the hubs, or a hub's computers")},
          {"Enter", tr("Show the selected hub's computers, or say yes to what was asked")},
          {"N", tr("Link this computer to another hub")},
          {"R", tr("Ask the hubs again how they stand")},
          {"S", tr("Sync the library with the selected hub")},
          {"P", tr("See what a sync with the selected hub would do, changing nothing")},
          {"A", tr("Pair this computer's library with the hub's, where a sync was refused for it (asks first)")},
          {"D  Delete", tr("Unlink the selected hub, or forget the selected computer (asks first)")},
          {"Escape", tr("Back a step, then close")}}},
        {"edit", tr("A book's details, by hand"),
         {{"Ctrl+S  Ctrl+Return", tr("Save")}}},
        {"lookup", tr("Looking a book's details up"),
         {{"Up  Down", tr("Move through what the sources found")},
          {"Enter", tr("Use the selected one")}}},
        {"sites", tr("Reading sites"),
         {{"Up  Down  J  K", tr("Move through the sites")},
          {"Enter", tr("Show the selected site")}}},
        {"snapshots", tr("The snapshots"),
         {{"Down", tr("From the filter into the snapshots")},
          {"Up  Down", tr("Move through the snapshots")},
          {"Enter", tr("Show what changed since the selected snapshot")},
          {"Escape", tr("Close")}}},
    };
}
