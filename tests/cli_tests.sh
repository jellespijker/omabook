#!/bin/bash
# Drives the real binary the way an agent would: no display, a scratch home,
# every command, checking output and exit codes.
set -uo pipefail

omabook="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" XDG_CACHE_HOME="$work/home/.cache" XDG_DATA_HOME="$work/home/.local/share"
unset DISPLAY WAYLAND_DISPLAY ANTHROPIC_API_KEY
mkdir -p "$HOME" "$XDG_CONFIG_HOME/omabook"
# AI off, so `ask` has a certain answer whatever is installed on this machine.
echo '{"ai_provider": "off"}' > "$XDG_CONFIG_HOME/omabook/config.json"
library="$work/Library"
failures=0

fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
# expect <what> <wanted exit code> <text the output must contain> <command...>
expect() {
  local what="$1" code="$2" text="$3" output status
  shift 3
  output="$("$@" 2>&1)"; status=$?
  [ "$status" -eq "$code" ] || fail "$what: exit $status, wanted $code: $output"
  [[ "$output" == *"$text"* ]] || fail "$what: no \"$text\" in: $output"
}

# A plain text book, and a small EPUB zipped with libarchive's bsdtar.
printf 'CHAPTER 1\n\nIt was a dark night.\n\nCHAPTER 2\n\nThe whale appeared.\n' > "$work/Sea Story - Ann Sailor.txt"
mkdir -p "$work/epub/META-INF"
cat > "$work/epub/META-INF/container.xml" <<'XML'
<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
XML
cat > "$work/epub/content.opf" <<'XML'
<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Command Book</dc:title><dc:creator>Cy Shell</dc:creator><dc:language>en</dc:language></metadata>
<manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="a"/></spine></package>
XML
echo '<html><body><h1>Only Chapter</h1><p>Words in an EPUB.</p></body></html>' > "$work/epub/a.xhtml"
(cd "$work/epub" && bsdtar --format zip -cf "$work/command.epub" META-INF content.opf a.xhtml) || fail "bsdtar could not make the EPUB"

expect "bare omabook prints the commands" 0 "omabook <command>" "$omabook"
expect "help for a command" 0 "Copy books into the library" "$omabook" add --help
# The help the window shows, for a terminal: the same pages, the keys as bound.
expect "help lists its topics" 0 "omabook help <topic>" "$omabook" help
expect "a help page" 0 "Turning pages" "$omabook" help reading
expect "a help page names keys, not commands" 0 "Space turns the page" "$omabook" help reading
expect "a help page as JSON" 0 '"command": "reader.page_down"' "$omabook" help reading --json
expect "the keys" 0 "Read the selected book" "$omabook" help keys
echo '{"library": {"Space": "", "R": "read"}}' > "$XDG_CONFIG_HOME/omabook/keys.json"
expect "the keys follow keys.json" 0 "  R                   Read the selected book" "$omabook" help keys
expect "a help page follows keys.json" 0 "R opens the selected book" "$omabook" help start
rm "$XDG_CONFIG_HOME/omabook/keys.json"
# A plugin's keys are among them, under its name and saying where they work;
# switched off, they are gone. The keys of a panel of its own are there too.
mkdir -p "$XDG_CONFIG_HOME/omabook/plugins/shelver"
echo '{"name": "Shelver", "script": "main.js"}' > "$XDG_CONFIG_HOME/omabook/plugins/shelver/plugin.json"
echo 'omabook.commands.register({id: "shelve", title: "Shelve it", place: "reader", key: "Ctrl+R", run: function () {}});' \
  > "$XDG_CONFIG_HOME/omabook/plugins/shelver/main.js"
expect "a plugin's keys are listed under its name" 0 "Plugin: Shelver" "$omabook" help keys
expect "a plugin's key says where it works" 0 "Shelve it (while reading)" "$omabook" help keys
expect "a plugin's key as JSON" 0 '"plugin": "Shelver"' "$omabook" help keys --json
expect "the keys of a panel" 0 "Remove the selected bookmark" "$omabook" help keys
expect "a plugin switched off" 0 "off" "$omabook" plugins disable folder:shelver
"$omabook" help keys | grep -q "Shelver" && fail "a plugin switched off still has its keys listed"
rm -rf "$XDG_CONFIG_HOME/omabook/plugins/shelver"
expect "what is new" 0 "Pages or scroll" "$omabook" help new
expect "what is new as JSON" 0 '"features"' "$omabook" help new --json
expect "an unknown help topic fails" 1 "no help on" "$omabook" help nonsense
expect "an empty library lists nothing" 0 "" "$omabook" list --library "$library"

expect "add" 0 "Sea Story" "$omabook" add --library "$library" "$work/Sea Story - Ann Sailor.txt" "$work/command.epub"
expect "adding again finds the duplicate" 0 "already in the library" "$omabook" add --library "$library" "$work/command.epub"
expect "adding what is not a book fails" 1 "not a book format" "$omabook" add --library "$library" "$work/epub/content.opf"
[ -f "$work/Sea Story - Ann Sailor.txt" ] || fail "add removed the original"
[ -f "$library/Cy Shell/Command Book (2)/Command Book - Cy Shell.epub" ] || fail "the EPUB was not copied into a readable folder"

expect "list" 0 "Command Book" "$omabook" list --library "$library"
expect "list as JSON" 0 '"title": "Sea Story"' "$omabook" list --library "$library" --json
expect "search by author" 0 "Sea Story" "$omabook" list --library "$library" author:sailor
[ -z "$("$omabook" list --library "$library" author:sailor | grep Command)" ] || fail "search did not filter"
expect "show" 0 '"authors"' "$omabook" show 2 --library "$library" --json
expect "show a missing book fails" 1 "no book 9" "$omabook" show 9 --library "$library"

expect "set" 0 '"title": "Renamed"' "$omabook" set 2 --library "$library" --title Renamed --tags "sea, test" --json
expect "set is kept" 0 "sea, test" "$omabook" show 2 --library "$library"
expect "set refuses an empty title" 1 "needs a title" "$omabook" set 2 --library "$library" --title ""
# What the book is called elsewhere: added, changed, forgotten; never "generated", which only a plugin sets.
expect "set gives identifiers" 0 '"goodreads": "4671"' "$omabook" set 2 --library "$library" --identifier goodreads=4671 --identifier isbn=9780000000001 --json
expect "show lists them as scheme=value" 0 "goodreads=4671" "$omabook" show 2 --library "$library"
expect "show has them in JSON" 0 '"isbn": "9780000000001"' "$omabook" show 2 --library "$library" --json
expect "set changes an identifier" 0 "goodreads=5000" bash -c "'$omabook' set 2 --library '$library' --identifier goodreads=5000 && '$omabook' show 2 --library '$library'"
"$omabook" set 2 --library "$library" --identifier isbn= > /dev/null || fail "set could not forget an identifier"
"$omabook" show 2 --library "$library" | grep -q "isbn=" && fail "an identifier given empty was not forgotten"
expect "set refuses the plugins' own scheme" 1 '"generated"' "$omabook" set 2 --library "$library" --title Nope --identifier generated=mine
"$omabook" show 2 --library "$library" | grep -q "Nope" && fail "a refused identifier still changed the title"
"$omabook" show 2 --library "$library" | grep -q "generated=" && fail "generated was set from the command line"
expect "set wants scheme=value" 1 "scheme=value" "$omabook" set 2 --library "$library" --identifier goodreads

expect "info on a file" 0 '"title": "Command Book"' "$omabook" info "$work/command.epub" --json
expect "toc" 0 "CHAPTER 2" "$omabook" toc 1 --library "$library"
expect "text of one section" 0 "The whale appeared." "$omabook" text 1 --library "$library" --section 0
expect "text of a file outside the library" 0 "Words in an EPUB." "$omabook" text "$work/command.epub"
expect "text of a section that is not there fails" 1 "sections 0 to 0" "$omabook" text 1 --library "$library" --section 7
expect "find" 0 "whale" "$omabook" find 1 --library "$library" the whale
expect "find nothing exits 1" 1 "" "$omabook" find 1 --library "$library" kraken

[ -d "$library/Cy Shell/Renamed (2)" ] || fail "the book's folder did not follow its new title"
expect "export" 0 "Command Book - Cy Shell.epub" "$omabook" export 2 "$work/out" --library "$library"
[ -f "$work/out/Command Book - Cy Shell.epub" ] || fail "export wrote no file"
expect "export does not overwrite" 1 "already a file" "$omabook" export 2 "$work/out" --library "$library"

expect "another format joins a book" 0 '"formats"' "$omabook" add --library "$library" --to 2 "$work/Sea Story - Ann Sailor.txt" --json
expect "the book now has both formats" 0 "epub, txt" "$omabook" show 2 --library "$library"
expect "progress is set" 0 '"section": 3' "$omabook" progress 2 --library "$library" --section 3 --fraction 0.5 --percent 40 --updated 1700001234 --json
expect "progress is kept, with its time" 0 '"updated": 1700001234' "$omabook" progress 2 --library "$library" --json
expect "the manifest lists files by hash" 0 '"hash"' "$omabook" manifest --library "$library"
printf '{"title": "Described", "authors": ["By Hand"], "tags": ["given"]}' > "$work/details.json"
printf 'Another text.\n' > "$work/other.txt"
expect "add with given details" 0 '"id": 3' "$omabook" add --library "$library" "$work/other.txt" --meta "$work/details.json" --json
expect "the given details are used" 0 '"title": "Described"' "$omabook" show 3 --library "$library" --json
expect "sync needs a computer" 1 "computer" "$omabook" sync --library "$library"
expect "identity makes a key pair and prints its id" 0 "Fingerprint:" "$omabook" identity --library "$library"
[ "$(stat -c %a "$XDG_DATA_HOME/omabook/person.key")" = 600 ] || fail "the key file can be read by others"
[ ! -e "$library/omabook-person.key" ] || fail "the key file is kept in the library folder, not per user"
id="$("$omabook" identity --export --library "$library")"
expect "identity keeps the same id" 0 "\"id\": \"$id\"" "$omabook" identity --json --library "$library"
expect "identity is in the help" 0 "identity [--export]" "$omabook" help
# A shared space (#38) between two people, each with an Omabook data folder of
# their own, through a folder something else syncs.
other() { XDG_DATA_HOME="$work/other/.local/share" "$omabook" "$@"; }
bo="$(other identity --export --library "$work/Bo")"
expect "a shared space is founded" 0 "Founded the shared space Club" "$omabook" spaces create Club --library "$work/Ada"
expect "the spaces are listed with kinds and roles" 0 "(shared; you are a writer)" "$omabook" spaces --library "$work/Ada"
expect "someone is invited" 0 "is a writer of Club now" "$omabook" spaces invite Club "$bo" --out "$work/bo.omabook-invite" --library "$work/Ada"
expect "an invite is only for its invitee" 1 "for someone else" "$omabook" spaces join "$work/bo.omabook-invite" --library "$work/Ada"
mkdir -p "$work/synced"
expect "the space syncs through a folder" 0 "written to the folder" "$omabook" spaces sync Club --folder "$work/synced" --library "$work/Ada"
expect "the invitee joins" 0 "Joined Club" other spaces join "$work/bo.omabook-invite" --library "$work/Bo"
expect "and takes what is in the folder" 0 '"taken": ' other spaces sync Club --folder "$work/synced" --json --library "$work/Bo"
expect "and is a writer of it" 0 '"role": "writer"' other spaces list --json --library "$work/Bo"
! grep -rqa "Club" "$work/synced" || fail "the folder holds plaintext"
expect "spaces is in the help" 0 "spaces [create|invite" "$omabook" help

expect "the plugins are listed" 0 "builtin:Open Library" "$omabook" plugins
expect "the plugins say what they bring" 0 '"fields"' "$omabook" plugins --json
expect "a source is switched off" 0 "off	metadata	builtin:Crossref" "$omabook" plugins disable builtin:Crossref
expect "and on again" 0 "on	metadata	builtin:Crossref" "$omabook" plugins enable builtin:Crossref
expect "a plugin by its plain name" 0 "off	metadata	builtin:Crossref" "$omabook" plugins disable crossref
expect "and by its id again" 0 "on	metadata	builtin:Crossref" "$omabook" plugins enable builtin:Crossref
expect "a name that is no plugin says what it may mean" 1 "Did you mean builtin:Crossref" "$omabook" plugins enable Cross
expect "what Omabook is made of can't be switched" 1 "no plugin format:EPUB" "$omabook" plugins disable format:EPUB
# The Calibre plugin, in a home that has no Calibre library.
expect "Calibre's libraries are listed, here none" 0 "[" "$omabook" calibre libraries --json
expect "with no Calibre library there is nothing to import" 1 "No Calibre library" "$omabook" calibre import --library "$library"
expect "a folder that is no Calibre library is refused" 1 "no Calibre library at" "$omabook" calibre import "$work" --library "$library"
expect "the Calibre plugin is switched off" 0 "off	calibre	calibre" "$omabook" plugins disable calibre
expect "and then nothing of Calibre's is looked at" 1 "switched off" "$omabook" calibre libraries
expect "nor converted with" 1 "switched off" "$omabook" convert 1 --to mobi --library "$library"
expect "and on again" 0 "on	calibre	calibre" "$omabook" plugins enable calibre
# A look-up with two catalogs of the user's own and nothing else, so it needs no network.
for plugin in builtin:Open\ Library "builtin:Google Books" "builtin:Apple Books" builtin:Crossref builtin:Goodreads builtin:Hardcover calibre; do
  "$omabook" plugins disable "$plugin" > /dev/null || fail "could not switch $plugin off"
done
for name in alpha beta; do
  mkdir -p "$XDG_CONFIG_HOME/omabook/plugins/$name"
  printf '#!/bin/sh\ncat > /dev/null\necho '"'"'{"candidates": [{"title": "From %s", "authors": ["Cy Shell"]}]}'"'"'\n' "$name" > "$XDG_CONFIG_HOME/omabook/plugins/$name/lookup"
  chmod +x "$XDG_CONFIG_HOME/omabook/plugins/$name/lookup"
  printf '{"name": "%s", "fields": ["title"], "command": ["%s"]}' "$name" "$XDG_CONFIG_HOME/omabook/plugins/$name/lookup" > "$XDG_CONFIG_HOME/omabook/plugins/$name/plugin.json"
done
expect "lookup lists the candidates in the sources' order" 0 "1	alpha	From alpha" "$omabook" lookup 2 --library "$library"
expect "each candidate has a number and an id" 0 '"candidate_id": "beta-' "$omabook" lookup 2 --library "$library" --json
expect "a number from a listing made without --ai is refused with --ai" 1 "made without --ai" "$omabook" lookup 2 --library "$library" --ai --apply 1
expect "and the refusal names the candidate's id" 1 "--apply alpha-" "$omabook" lookup 2 --library "$library" --ai --apply 1
# The catalog answers otherwise now; the number still means what was shown.
sed -i 's/From alpha/Changed alpha/' "$XDG_CONFIG_HOME/omabook/plugins/alpha/lookup"
expect "a number means the candidate that was listed" 0 '"title": "From alpha"' "$omabook" lookup 2 --library "$library" --apply 1 --json
beta="$("$omabook" lookup 2 --library "$library" --json | grep -o '"candidate_id": "beta-[0-9a-f]*"' | cut -d'"' -f4)"
[ -n "$beta" ] || fail "the listing gave beta no id"
expect "a candidate is taken by its id" 0 '"title": "From beta"' "$omabook" lookup 2 --library "$library" --apply "$beta" --json
expect "an id no source answers with now is refused" 1 "No candidate is called alpha-00000000" "$omabook" lookup 2 --library "$library" --apply alpha-00000000
"$omabook" set 2 --library "$library" --title Renamed > /dev/null
rm -rf "$XDG_CONFIG_HOME/omabook/plugins/alpha" "$XDG_CONFIG_HOME/omabook/plugins/beta"
for plugin in builtin:Open\ Library "builtin:Google Books" "builtin:Apple Books" builtin:Crossref builtin:Goodreads builtin:Hardcover calibre; do
  "$omabook" plugins enable "$plugin" > /dev/null
done

expect "a catalog as csv" 0 "id,title,authors" "$omabook" catalog --library "$library"
expect "a catalog as bibtex, searched" 0 "@book{" "$omabook" catalog --library "$library" --format bibtex described
expect "a catalog format there isn't" 1 "csv or bibtex" "$omabook" catalog --library "$library" --format docx

# A script plugin, the way an agent writes one: from a template, checked, run, used, broken and switched off.
expect "a plugin starts from a template" 0 "plugins/reading-list" "$omabook" plugin new reading-list --template catalog
expect "a plugin is checked with no window" 0 "1 catalogs" "$omabook" plugin check reading-list
expect "a script's catalog format is one of the catalog's" 0 "# Books" "$omabook" catalog --library "$library" --format reading-list
expect "part of a plugin is run" 0 "# Books" "$omabook" plugin run reading-list catalog:reading-list --library "$library"
expect "the script plugins are listed" 0 '"running": true' "$omabook" plugin list --json
expect "the scripting reference is printed" 0 "omabook.library.setMeta" "$omabook" plugin api
expect "the skill says how to write a plugin" 0 "# Writing a plugin" "$omabook" skill plugins
expect "the skill points to its parts" 0 "omabook skill <part>" "$omabook" skill
expect "the skill names an unknown part" 1 "Its parts are library, graph and plugins" "$omabook" skill nonsense
expect "the whole skill" 0 "omabook graph build" "$omabook" skill --all
echo 'omabook.nothing.here();' > "$XDG_CONFIG_HOME/omabook/plugins/reading-list/main.js"
expect "a broken plugin says where" 1 "main.js:1" "$omabook" plugin check reading-list
expect "a broken plugin leaves the other catalogs alone" 0 "id,title,authors" "$omabook" catalog --library "$library"
expect "a plugin is switched off" 0 "is off" "$omabook" plugin disable reading-list

# A plugin someone else wrote: installed from a folder or a zip, checked first and left off, and removed again.
mkdir -p "$work/handed/Shelf Notes"
echo '{"name": "Shelf Notes", "script": "main.js", "permissions": ["http"]}' > "$work/handed/Shelf Notes/plugin.json"
echo 'omabook.on("book.added", function () {});' > "$work/handed/Shelf Notes/main.js"
expect "a plugin is installed from a folder, and left off" 0 "It is off" "$omabook" plugin install "$work/handed/Shelf Notes"
expect "what it asks for is said" 0 "It may: http" "$omabook" plugin install "$work/handed/Shelf Notes" --replace
expect "one of the same name is in the way" 1 "already" "$omabook" plugin install "$work/handed/Shelf Notes"
(cd "$work/handed" && bsdtar -a -cf "$work/shelf-notes.zip" "Shelf Notes")
expect "a plugin is installed from a zip, switched on" 0 "from-zip is on" "$omabook" plugin install "$work/shelf-notes.zip" --name from-zip --enable
expect "an installed plugin loads" 0 "from-zip loads" "$omabook" plugin check from-zip
expect "what is no plugin is refused" 1 "no plugin.json" "$omabook" plugin install "$work/epub"
expect "a plugin is removed" 0 "from-zip is" "$omabook" plugin remove from-zip
[ ! -e "$XDG_CONFIG_HOME/omabook/plugins/from-zip" ] || fail "the removed plugin is still in the plugin folder"
expect "what isn't there can't be removed" 1 "no plugin from-zip" "$omabook" plugin remove from-zip
# Plugins from GitHub: nothing here asks the network. An address that isn't GitHub is refused before anything is asked.
expect "no plugin came from a repository yet" 0 "omabook plugin add owner/repo" "$omabook" plugin sources
expect "only GitHub is fetched from" 1 "Only GitHub" "$omabook" plugin add https://example.com/someone/plugin
expect "what is done about updates is a setting" 0 "only told" "$omabook" plugin sources --updates tell
expect "and only one of three" 1 "auto" "$omabook" plugin sources --updates sometimes
expect "a plugin without a source has nothing to update" 1 "No source" "$omabook" plugin update reading-list --check
expect "the host's version is in the reference" 0 '"omabook"' "$omabook" plugin api --json
expect "a new plugin says which Omabook it was written on" 0 '"omabook"' cat "$XDG_CONFIG_HOME/omabook/plugins/reading-list/plugin.json"

# A book format added by a script, from the template: read by the commands with no window and nobody loading the plugin for them.
expect "a format plugin starts from a template" 0 "plugins/textbook" "$omabook" plugin new textbook --template format
mkdir -p "$work/The Long Way"
printf 'title: The Long Way\nauthor: Ann Author\n\n01-start.txt   Setting Out\n02-road.txt    On the Road\n' > "$work/The Long Way/index.txt"
printf 'We left at dawn.\n\nNobody waved.\n' > "$work/The Long Way/01-start.txt"
printf 'The road went on.\n' > "$work/The Long Way/02-road.txt"
expect "a script's open is run on a folder" 0 '"title": "The Long Way"' "$omabook" plugin run textbook "format:Text book" "\"$work/The Long Way\""
expect "a folder a script claims is added as one book" 0 "The Long Way" "$omabook" add "$work/The Long Way" --library "$library"
expect "a script's format is listed" 0 "txtbook" "$omabook" list --library "$library" title:Long
longway="$("$omabook" list --library "$library" title:Long | cut -f1)"
expect "a script's format has contents" 0 "On the Road" "$omabook" toc "$longway" --library "$library"
expect "a script's format is read" 0 "Nobody waved." "$omabook" text "$longway" --library "$library" --section 0
expect "a script's format is searched" 0 "road went on" "$omabook" find "$longway" --library "$library" road
expect "a library command emits no Qt warnings on exit" 0 "Sea Story" env QT_FATAL_WARNINGS=1 "$omabook" list --library "$library"
expect "manifest emits no Qt warnings on exit" 0 '"books"' env QT_FATAL_WARNINGS=1 "$omabook" manifest --library "$library"
"$omabook" remove "$longway" --library "$library" >/dev/null
"$omabook" plugin disable textbook >/dev/null

expect "customize says where the interface lives" 0 ".config/omabook" "$omabook" customize
[ -f "$XDG_CONFIG_HOME/omabook/AGENTS.md" ] || fail "customize wrote no guide"
grep -q "reader.page_down" "$XDG_CONFIG_HOME/omabook/AGENTS.md" || fail "the guide doesn't list the commands"
expect "customize copies a view to edit" 0 "ui/DetailsPane.qml" "$omabook" customize DetailsPane
[ -w "$XDG_CONFIG_HOME/omabook/ui/DetailsPane.qml" ] || fail "the copied view can't be edited"
expect "customize won't overwrite your view" 1 "already" "$omabook" customize DetailsPane
expect "customize knows its views" 1 "no built-in view" "$omabook" customize Nope

# Themes: no Omarchy and no Aether in this home, so Automatic is Tokyo Night.
expect "the theme in use" 0 "Tokyo Night" "$omabook" theme
expect "the theme says why" 0 "neither Omarchy nor Aether" "$omabook" theme show
expect "the themes there are" 0 "catppuccin-latte" "$omabook" theme list
expect "the themes as JSON" 0 '"choice": "auto"' "$omabook" theme list --json
expect "a theme by its name" 0 "Rose Pine" "$omabook" theme use "Rose Pine"
grep -q '"theme": "rose-pine"' "$XDG_CONFIG_HOME/omabook/style.json" || fail "theme use wrote no theme into style.json"
expect "info says which theme" 0 '"kind": "bundled"' "$omabook" info --json
expect "an unknown theme is refused" 1 "no theme called" "$omabook" theme use plaid
grep -q '"theme": "rose-pine"' "$XDG_CONFIG_HOME/omabook/style.json" || fail "a refused theme changed style.json"
mkdir -p "$work/mine"
printf 'mode = "light"\nbackground = "#fafafa"\nforeground = "#202020"\naccent = "#005fcc"\n' > "$work/mine/colors.toml"
expect "a colors.toml of your own" 0 '"kind": "file"' "$omabook" theme use "$work/mine/colors.toml" --json
mkdir -p "$XDG_CONFIG_HOME/aether/blueprints"
printf '{"name": "Harbour", "palette": {"colors": ["#101418","#e06c75","#98c379","#e5c07b","#61afef","#c678dd","#56b6c2","#dcdfe4","#5c6370","#e06c75","#98c379","#e5c07b","#61afef","#c678dd","#56b6c2","#ffffff"]}}' > "$XDG_CONFIG_HOME/aether/blueprints/harbour.json"
expect "the blueprints are listed" 0 '"harbour"' "$omabook" theme list --json
expect "an Aether blueprint is imported" 0 "omabook theme use harbour" "$omabook" theme import Harbour
[ -f "$XDG_CONFIG_HOME/omabook/themes/harbour/colors.toml" ] || fail "theme import wrote no colors.toml"
expect "an imported theme is used" 0 "your own" "$omabook" theme use harbour
expect "automatic again" 0 "Tokyo Night" "$omabook" theme use auto
grep -q '"theme"' "$XDG_CONFIG_HOME/omabook/style.json" && fail "theme use auto left a theme in style.json"
rm -rf "$XDG_CONFIG_HOME/aether" "$XDG_CONFIG_HOME/omabook/style.json"
rm -rf "$XDG_CONFIG_HOME/omabook/ui"

printf 'One copy.\n' > "$work/Twice - Reader, Rita.txt"
printf 'Another copy.\n' > "$work/twice - Rita Reader.md"
"$omabook" add --library "$library" "$work/Twice - Reader, Rita.txt" "$work/twice - Rita Reader.md" > /dev/null
expect "tidy finds the same book twice" 0 "duplicate" "$omabook" tidy --library "$library"
expect "tidy finds a name turned round" 0 "Reader, Rita -> Rita Reader" "$omabook" tidy --library "$library"
expect "tidy changes nothing unasked" 0 "Reader, Rita" "$omabook" list --library "$library"
expect "tidy makes the changes when asked" 0 "changes made" "$omabook" tidy --library "$library" --apply
[ "$("$omabook" list --library "$library" twice | wc -l)" -eq 1 ] || fail "tidy did not fold the two copies into one"
expect "the kept copy has both formats" 0 "md,txt" "$omabook" list --library "$library" twice
expect "a tidy library has nothing to tidy" 0 "" "$omabook" tidy --library "$library"
[ -z "$("$omabook" tidy --library "$library")" ] || fail "tidy still proposes changes after applying them"
expect "suggest says when there is no AI" 1 "AI is turned off" "$omabook" suggest --library "$library"
expect "ask all says when there is no AI" 1 "AI is turned off" "$omabook" ask all --library "$library" "What do I have?"
expect "change says when there is no AI" 1 "AI is turned off" "$omabook" change "make it warmer"
expect "change has nothing to undo" 1 "no change to undo" "$omabook" change --undo

expect "ask says when there is no AI" 1 "AI is turned off" "$omabook" ask 1 --library "$library" "Who narrates?"
expect "convert of a missing book fails" 1 "no such book" "$omabook" convert 9 --library "$library"

expect "remove" 0 "" "$omabook" remove 1 --library "$library"
expect "removed books are gone" 1 "no book 1" "$omabook" show 1 --library "$library"

expect "info without a file is about Omabook" 0 '"source": "none"' "$omabook" info --json
expect "the MCP server lists its tools" 0 "books_search" "$omabook" mcp --tools
expect "the MCP server answers on standard output" 0 '"serverInfo"' sh -c "echo '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}' | '$omabook' mcp --library '$library'"
expect "registering the server can be shown without doing it" 0 "mcp add" "$omabook" mcp install claude --print

# Goodreads, from the file it lets its users download: no network is asked.
shelves="$work/Shelves"
printf 'Book Id,Title,Author,ISBN,ISBN13,My Rating,Binding,Date Read,Exclusive Shelf,Owned Copies\n701,"Read, Long Ago (The Past, #1)",Ann Reader,="",="",4,Paperback,2020/02/03,read,1\n702,Some Day,Bo Wisher,="",="",0,Kindle Edition,,to-read,0\n' > "$work/goodreads_library_export.csv"
expect "a Goodreads export is imported" 0 "2 new, 0 updated" "$omabook" goodreads import "$work/goodreads_library_export.csv" --library "$shelves" --no-covers
expect "what was owned is a paper book" 0 "Read, Long Ago" "$omabook" list --library "$shelves" is:paper status:read rating:4
expect "what was wished for is only known of" 0 "Some Day" "$omabook" list --library "$shelves" is:wishlist status:to-read
expect "importing it again changes nothing" 0 '"unchanged": 2' "$omabook" goodreads import "$work/goodreads_library_export.csv" --library "$shelves" --no-covers --json
expect "the way back is a file for Goodreads" 0 "2 books written" "$omabook" goodreads export "$work/back.csv" --library "$shelves"
grep -q '^701,"Read, Long Ago",Ann Reader,.*2020/02/03.*,read,' "$work/back.csv" || fail "the export lacks the book that was read: $(cat "$work/back.csv")"
expect "another file is not a Goodreads export" 1 "not a Goodreads export" "$omabook" goodreads import "$work/Sea Story - Ann Sailor.txt" --library "$shelves"
expect "sync says when nobody is named" 1 "goodreads_user" "$omabook" goodreads sync --library "$shelves"
expect "Open Library says when nobody is named" 1 "openlibrary_user" "$omabook" openlibrary sync --library "$shelves"

# The reading sites as one is connected to them. No keyring is there for
# these (the bus address leads nowhere), so no test can reach the real one.
nokeyring() { DBUS_SESSION_BUS_ADDRESS="unix:path=$work/no-bus" "$@"; }
expect "every site is listed, not connected" 0 "hardcover	none" nokeyring "$omabook" sites
expect "the list has no token in it" 0 '"has_token": false' nokeyring "$omabook" sites --json
expect "what names nobody is refused" 1 "names nobody on Open Library" nokeyring "$omabook" sites connect openlibrary "ann reader"
expect "Hardcover wants a token" 1 "needs a token" nokeyring "$omabook" sites connect hardcover
echo "hc_pat_CLITEST_TOKEN" > "$work/token"
expect "without a keyring a token is not kept, and that is said" 1 "Nothing was kept" nokeyring "$omabook" sites connect hardcover --token-file "$work/token"
grep -q "CLITEST" "$XDG_CONFIG_HOME/omabook/config.json" && fail "the token is in config.json"
expect "a token is never an argument" 1 "token-stdin" nokeyring "$omabook" sites connect hardcover ann hc_pat_CLITEST_TOKEN extra
expect "there is nothing to sync until a site is connected" 1 "No reading site is connected" nokeyring "$omabook" sites sync --library "$shelves"
expect "disconnecting leaves the books" 0 "Your books are as they were" nokeyring "$omabook" sites disconnect goodreads
expect "the books are still there" 0 "Some Day" "$omabook" list --library "$shelves"
expect "an unknown site is named as such" 1 "no reading site called nowhere" nokeyring "$omabook" sites check nowhere

# How books are set on the page: the defaults, and one book's own over them.
book="$("$omabook" list --library "$library" --json | grep -o '"id": [0-9]*' | head -1 | grep -o '[0-9]*$')"
expect "the format says what every book is set with" 0 "line_height        150%" "$omabook" format --library "$library"
expect "the defaults change" 0 '"align": "justify"' "$omabook" format --library "$library" --align justify --line-height 170 --json
grep -q '"line_height": 170' "$XDG_CONFIG_HOME/omabook/config.json" || fail "the default line height is not in config.json"
expect "a value out of bounds says what it may be" 1 "--line-height is 100 to 300 (percent); 400 is not" "$omabook" format --library "$library" --line-height 400
expect "a word that isn't an alignment says the four" 1 "start, justify, center or end" "$omabook" format --library "$library" --align middle
expect "a number that isn't one says the range" 1 "--margins takes a whole number from 0 to 160" "$omabook" format --library "$library" --margins wide
expect "nothing changed after a refusal" 0 "line_height        170%" "$omabook" format --library "$library"
expect "a book gets its own" 0 "align              center   (this book)" "$omabook" format "$book" --library "$library" --align center --font-size 23
expect "and keeps the defaults for the rest" 0 "line_height        170%" "$omabook" format "$book" --library "$library"
expect "its own as JSON" 0 '"own": {' "$omabook" format "$book" --library "$library" --json
expect "the defaults are not the book's" 0 "align              justify" "$omabook" format --library "$library"
expect "a book that isn't there is said to be" 1 "There is no book 99" "$omabook" format 99 --library "$library" --indent 8
expect "reset goes alone" 1 "--reset goes alone" "$omabook" format "$book" --library "$library" --reset --indent 8
expect "reset puts the book back to the defaults" 0 "(this book)" "$omabook" format "$book" --library "$library" --align end
expect "reset puts the book back to the defaults" 0 "align              justify" "$omabook" format "$book" --library "$library" --reset
"$omabook" format "$book" --library "$library" | grep -qF "(this book)" && fail "the book still has its own after --reset"
expect "reset without a book puts the defaults back" 0 "line_height        150%" "$omabook" format --library "$library" --reset
# The longest line in characters, and the pages side by side.
expect "the measure is in characters" 0 "measure            66 characters" "$omabook" format --library "$library"
expect "the measure changes" 0 "measure            58 characters" "$omabook" format --library "$library" --measure 58
expect "a measure out of bounds says what it may be" 1 "--measure is 40 to 90 (characters); 120 is not" "$omabook" format --library "$library" --measure 120
expect "a book gets a measure of its own" 0 "measure            72 characters   (this book)" "$omabook" format "$book" --library "$library" --measure 72
expect "the pages are automatic until chosen" 0 "pages              auto (as many as fit)" "$omabook" format --library "$library"
expect "the pages are chosen" 0 '"pages": 3' "$omabook" format --library "$library" --pages 3 --json
grep -q '"pages": 3' "$XDG_CONFIG_HOME/omabook/config.json" || fail "the pages are not in config.json"
expect "pages that aren't a count say what they may be" 1 "--pages is auto, 1, 2 or 3; 4 is not" "$omabook" format --library "$library" --pages 4
expect "pages are for every book" 1 "--pages is for every book" "$omabook" format "$book" --library "$library" --pages 2
expect "pages go back to automatic" 0 '"pages": "auto"' "$omabook" format --library "$library" --pages auto --json
# Pages or a scrolling column, per book or for every book, and the first page of two side by side.
expect "the layout is automatic until chosen" 0 "layout             auto" "$omabook" format --library "$library"
expect "every book scrolls" 0 '"layout": "scroll"' "$omabook" format --library "$library" --layout scroll --json
grep -q '"layout": "scroll"' "$XDG_CONFIG_HOME/omabook/config.json" || fail "the layout is not in config.json"
expect "a book turns pages of its own" 0 "layout             pages   (this book)" "$omabook" format "$book" --library "$library" --layout pages
expect "the rest still scroll" 0 "layout             scroll" "$omabook" format --library "$library"
expect "a layout that isn't one says the three" 1 "--layout is auto, pages or scroll; columns is not" "$omabook" format --library "$library" --layout columns
expect "the first page pairs" 0 '"first_page": "paired"' "$omabook" format --library "$library" --first-page paired --json
expect "a first page that isn't one says what it may be" 1 "--first-page is auto, alone or paired" "$omabook" format --library "$library" --first-page left
expect "reset puts the layout back" 0 "layout             auto" "$omabook" format --library "$library" --reset
"$omabook" format "$book" --library "$library" --reset >/dev/null
"$omabook" format --library "$library" --reset >/dev/null

# The options every command takes may come before its name too, and a
# failing command fails the whole && chain.
expect "--library and --json before the command" 0 '"title": "Renamed"' "$omabook" --library "$library" --json list
expect "--library=folder before the command" 0 "Renamed" "$omabook" --library="$library" list
expect "--help before the command is the command's" 0 "List the library's books" "$omabook" --help list
expect "an option no command takes is named" 1 "Unknown option 'jsn'" "$omabook" --jsn list --library "$library"
expect "a word that is no command says so" 1 '"lsit" is not a command' "$omabook" --library "$library" lsit --json
expect "and so does one with no options" 1 '"lsit" is not a command' "$omabook" lsit
output="$(bash -c "'$omabook' show 99 --library '$library' && echo went on" 2>&1)"
[[ "$output" == *"went on"* ]] && fail "a failing command didn't stop the && chain: $output"

# --library is for one run: no command that takes it changes config.json,
# with the option before the command's name or after, nor does a window
# opened on another library. Every command in the table is run.
config="$XDG_CONFIG_HOME/omabook/config.json"
elsewhere="$work/Elsewhere"
printf 'CHAPTER 1\n\nAnother dark night.\n' > "$work/elsewhere.txt"
calibre="$work/Calibre Elsewhere"
if command -v sqlite3 > /dev/null; then
  mkdir -p "$calibre/Writer, Ann/Same Book (1)"
  printf 'Calibre words.\n' > "$calibre/Writer, Ann/Same Book (1)/Same Book - Ann Writer.txt"
  sqlite3 "$calibre/metadata.db" "CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, series_index REAL, pubdate TEXT, path TEXT, has_cover BOOL, uuid TEXT);
    CREATE TABLE data (id INTEGER PRIMARY KEY, book INTEGER, format TEXT, name TEXT);
    INSERT INTO books VALUES (1, 'Same Book', 1.0, '2001-01-01', 'Writer, Ann/Same Book (1)', 0, 'uuid-1');
    INSERT INTO data VALUES (1, 1, 'TXT', 'Same Book - Ann Writer');" || fail "sqlite3 could not make a Calibre library"
fi
cp "$config" "$work/config.before"
# What each command is given to get as far as the library.
arguments_for() {
  case "$1" in
    show|bookmarks|notes|toc|text|progress|format) words=(1) ;;
    add) words=("$work/elsewhere.txt") ;;
    set) words=(1 --tags elsewhere) ;;
    find) words=(1 dark) ;;
    ask) words=(1 "What happens?") ;;
    remove|lookup|convert) words=(99) ;;
    export) words=(99 "$work") ;;
    graph) words=(show) ;;
    sync) words=(nohost.invalid) ;;
    plugin) words=(list) ;;
    calibre) words=(import "$calibre") ;;
    goodreads|openlibrary) words=(shelves) ;;
    theme) words=(list) ;;
    change) words=(--undo) ;;
    help) words=(start) ;;
    *) words=() ;;
  esac
}
commands="$(grep -o 'CliCommand{"[a-z]*"_L1' "$(dirname "$0")/../src/cli.cpp" | sed 's/CliCommand{"\([a-z]*\)".*/\1/')"
[ -n "$commands" ] || fail "no commands were read from the command table"
for name in $commands; do
  "$omabook" "$name" --help 2>&1 | grep -q -- "--library" || continue
  arguments_for "$name"
  for order in after before; do
    if [ "$order" = after ]; then
      timeout 60 "$omabook" "$name" "${words[@]}" --library "$elsewhere" < /dev/null > /dev/null 2>&1
    else
      timeout 60 "$omabook" --library "$elsewhere" "$name" "${words[@]}" < /dev/null > /dev/null 2>&1
    fi
    if ! cmp -s "$work/config.before" "$config"; then
      fail "$name with --library $order it changed config.json: $(diff "$work/config.before" "$config")"
      cp "$work/config.before" "$config"
    fi
  done
done
[ -f "$elsewhere/omabook.db" ] || fail "the commands didn't use the library they were given"
expect "the library in use is still the one it was" 0 "" "$omabook" info --json
"$omabook" info --json | grep -qF "\"library\": \"$elsewhere\"" && fail "info says the library in use is the one given once"
QT_QPA_PLATFORM=offscreen timeout 120 "$omabook" open --library "$elsewhere" --screenshot "$work/elsewhere.png" > /dev/null 2>&1 \
  || fail "the window didn't open on another library"
cmp -s "$work/config.before" "$config" || fail "opening the window on another library changed config.json: $(diff "$work/config.before" "$config")"

# Collections: nested, named as the user likes, filled by a rule and by
# hand, listed as a tree, and narrowing list, ask and suggest.
shelf="$work/Collections"
"$omabook" add "$work/Sea Story - Ann Sailor.txt" "$work/command.epub" --library "$shelf" > /dev/null 2>&1 || fail "no books for the collections"
expect "no collections yet" 0 "no collections yet" "$omabook" collections --library "$shelf"
expect "a nested collection, its parents made too" 0 "Werk/Hydraulica" "$omabook" collections add "Werk/Hydraulica" --rule "title:sea" --library "$shelf"
expect "a collection with --parent" 0 "Werk/Plezier" "$omabook" collections add Plezier --parent Werk --library "$shelf"
expect "a name that two collections share is refused" 1 "already" "$omabook" collections add Hydraulica --parent Werk --library "$shelf"
expect "a rule it can't read is refused" 1 "auther:" "$omabook" collections rule Werk/Plezier "auther:x" --library "$shelf"
expect "a book put in by hand" 0 "Werk/Plezier" "$omabook" collections include Werk/Plezier 2 --library "$shelf"
expect "the tree, indented" 0 "  Hydraulica" "$omabook" collections --library "$shelf"
expect "a parent holds what is inside it" 0 '"count": 2' "$omabook" collections show Werk --json --library "$shelf"
expect "list keeps to a collection" 0 "Command Book" "$omabook" list --collection Werk/Plezier --library "$shelf"
"$omabook" list --collection Werk/Hydraulica --library "$shelf" | grep -q "Command Book" && fail "list --collection showed a book outside it"
expect "list names a collection that isn't there" 1 "no collection called" "$omabook" list --collection Nowhere --library "$shelf"
expect "a collection renamed with a path moves, what is inside along" 0 "Studie" "$omabook" collections rename Werk Studie --library "$shelf"
expect "found by its new path" 0 "Hydraulica" "$omabook" collections show Studie/Hydraulica --json --library "$shelf"
expect "moved to the top" 0 "Plezier" "$omabook" collections move Studie/Plezier "" --library "$shelf"
expect "a collection's own reading format" 0 "justify" "$omabook" collections format Plezier --set align=justify --library "$shelf"
expect "a format value out of bounds is refused" 1 "40 to 90" "$omabook" collections format Plezier --set measure=10 --library "$shelf"
expect "taken out by hand" 0 "0 books" "$omabook" collections exclude Plezier 2 --library "$shelf"
expect "sorting needs the AI, and says so" 1 "" "$omabook" collections sort --library "$shelf"
expect "asking about one collection needs the AI too" 1 "" "$omabook" ask all "Which?" --collection Plezier --library "$shelf"
expect "removed, its books stay" 0 "stay in the library" "$omabook" collections remove Studie --library "$shelf"
expect "what was inside it moved up" 0 "Hydraulica" "$omabook" collections --library "$shelf"
expect "every book is still there" 0 "Command Book" "$omabook" list --library "$shelf"

# The search finds books by what is known of them, as far as they were read,
# and with --ai asks a stand-in model, whose answer is kept for next time.
found="$work/Found"
"$omabook" add "$work/Sea Story - Ann Sailor.txt" "$work/command.epub" --library "$found" > /dev/null 2>&1 || fail "no books for the search"
"$omabook" progress 1 --section 0 --fraction 0.5 --percent 50 --library "$found" > /dev/null
"$omabook" graph add --kind character --name "Mara Venn" --book 1 --section 0 --library "$found" > /dev/null
"$omabook" graph add --kind character --name "Hidden Holt" --book 2 --section 0 --library "$found" > /dev/null
expect "a word finds a book by a character met in it" 0 "Mara Venn (character)" "$omabook" search venn --library "$found"
expect "knows: looks only at what is known" 0 '"name": "Mara Venn"' "$omabook" search knows:mara --json --library "$found"
[ -z "$("$omabook" search holt --library "$found")" ] || fail "the search found a book by what is in a book not begun"
expect "a book's own details are found too" 0 "Command Book" "$omabook" search command --library "$found"
[ -z "$("$omabook" list venn --library "$found")" ] || fail "list found a book by what is known of it"
expect "knows: is no collection's rule" 1 "knows:" "$omabook" collections add Known --rule knows:mara --library "$found"
expect "asking the AI needs one" 1 "AI is turned off" "$omabook" search --ai "a lighthouse keeper" --library "$found"
cat > "$work/model" <<SCRIPT
#!/bin/bash
cat > "$work/model.prompt"
echo asked >> "$work/model.calls"
echo '{"books": [{"id": 1, "why": "A keeper by the sea", "score": 80}, {"id": 77, "why": "not here", "score": 99}]}'
SCRIPT
chmod +x "$work/model"
cp "$XDG_CONFIG_HOME/omabook/config.json" "$work/config.off"
printf '{"ai_provider": "command", "ai_command": "%s"}' "$work/model" > "$XDG_CONFIG_HOME/omabook/config.json"
expect "the AI finds books for a question" 0 "A keeper by the sea" "$omabook" search --ai "a lighthouse keeper" --library "$found"
grep -q "Mara Venn" "$work/model.prompt" || fail "the AI wasn't told what is known of the books"
grep -q "Hidden Holt" "$work/model.prompt" && fail "the AI was told what is in a book not begun"
expect "a question asked again is answered from what was kept" 0 '"cached": true' "$omabook" search --ai "a lighthouse keeper" --json --library "$found"
[ "$(wc -l < "$work/model.calls")" -eq 1 ] || fail "asking again asked the AI again"
"$omabook" search --ai "a lighthouse keeper" --json --library "$found" | grep -q '"id": 77' && fail "the AI's answer named a book that isn't there"

# Sorting within one collection: its books, placed inside it, staying in it;
# a place outside it is the user's to decide.
"$omabook" collections add Kennis --library "$found" > /dev/null
"$omabook" collections include Kennis 1 2 --library "$found" > /dev/null
cat > "$work/model" <<SCRIPT
#!/bin/bash
cat > "$work/model.prompt"
echo '[{"id": 1, "collection": "Kennis/Zee", "new": true, "why": "about the sea"}, {"id": 2, "collection": "Elders", "new": true, "why": "somewhere else"}]'
SCRIPT
expect "sort within a collection names one that isn't there" 1 "There is no collection Nowhere" "$omabook" collections sort Nowhere --library "$found"
expect "sort within a collection proposes one inside it" 0 '"collection": "Kennis/Zee"' "$omabook" collections sort Kennis --json --library "$found"
grep -q "already in the collection" "$work/model.prompt" && grep -q "Kennis/" "$work/model.prompt" || fail "the AI was not told the books stay in Kennis"
"$omabook" collections sort Kennis --json --library "$found" --pick 1 > /dev/null 2>&1 || true
expect "a place outside the collection is left to decide" 0 "outside Kennis" "$omabook" collections sort Kennis --json --again --library "$found"
expect "picked: the book is inside it" 0 "Sea Story" "$omabook" collections show Kennis/Zee --library "$found"
expect "and still in the collection itself" 0 '"count": 2' "$omabook" collections show Kennis --json --library "$found"
cp "$work/config.off" "$XDG_CONFIG_HOME/omabook/config.json"

expect "the skill is bundled" 0 "omabook list --json" "$omabook" skill
expect "the skill installs" 0 "Installed" "$omabook" skill install
[ -f "$HOME/.agents/skills/omabook/SKILL.md" ] && [ -L "$HOME/.claude/skills/omabook" ] || fail "skill install left no skill"
for part in library graph plugins; do
    [ -s "$HOME/.agents/skills/omabook/references/$part.md" ] || fail "skill install left out the $part part"
done
[ "$("$omabook" skill | wc -c)" -lt 12000 ] || fail "the skill read every time is too long"

[ "$failures" -eq 0 ] && echo "cli: all passed" || { echo "cli: $failures failed" >&2; exit 1; }
