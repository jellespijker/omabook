# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Jelle Spijker
#
# This file runs inside Calibre and uses Calibre's own Python modules, so
# unlike the rest of Omabook (MIT) it is under the GNU General Public License,
# version 3 or later, as Calibre is (Copyright (c) Kovid Goyal and
# contributors, https://calibre-ebook.com). It is installed beside Omabook as
# a file of its own and is not part of the Omabook program.
#
# Runs inside Calibre (calibre-debug -e) so Omabook can use Calibre's own
# metadata source plugins, the built-in ones and any the user installed.
#
#   calibre-debug -e calibre_bridge.py -- list
#   calibre-debug -e calibre_bridge.py -- identify <request.json>
#
# The request is {"title", "authors": [...], "isbn", "plugins": [names] or null}.
# The answer is one line starting with OMABOOK-JSON: so Calibre's own chatter
# on standard output can be told apart from it.
import json
import sys
from queue import Empty, Queue
from threading import Event, Thread


def describe(plugin):
    from calibre.customize.ui import is_disabled
    return {
        'name': plugin.name,
        'version': '.'.join(str(part) for part in plugin.version),
        'author': plugin.author,
        'description': plugin.description,
        'fields': sorted(plugin.touched_fields),
        'identifies': 'identify' in plugin.capabilities,
        'enabled': not is_disabled(plugin),
        'configured': bool(plugin.is_configured()),
        # Added to Calibre by the user, rather than shipped with it.
        'installed': bool(getattr(plugin, 'plugin_path', None)),
    }


def to_candidate(plugin, book):
    published = ''
    if book.pubdate is not None:
        try:
            published = book.pubdate.strftime('%Y-%m-%d')
        except Exception:
            published = ''
    cover = ''
    try:
        cover = plugin.get_cached_cover_url(book.identifiers) or ''
    except Exception:
        cover = ''
    series_index = book.series_index if book.series else 0
    return {
        'source': plugin.name,
        'title': book.title or '',
        'authors': [name for name in (book.authors or []) if name and name != 'Unknown'],
        'series': book.series or '',
        'series_index': float(series_index or 0),
        'publisher': book.publisher or '',
        'published': published,
        'language': (book.languages or [''])[0] if book.languages else (book.language or ''),
        'isbn': (book.identifiers or {}).get('isbn', '') or (book.isbn or ''),
        'description': book.comments or '',
        'tags': list(book.tags or [])[:8],
        'cover_url': cover,
    }


def identify(request):
    from calibre.customize.ui import all_metadata_plugins, is_disabled
    from calibre.utils.logging import Log

    wanted = request.get('plugins')
    plugins = [plugin for plugin in all_metadata_plugins()
               if 'identify' in plugin.capabilities and plugin.is_configured()
               and (plugin.name in wanted if wanted is not None else not is_disabled(plugin))]
    identifiers = {'isbn': request['isbn']} if request.get('isbn') else {}
    abort = Event()
    log = Log(level=Log.ERROR)
    answers = []

    def ask(plugin):
        found = Queue()
        problem = ''
        try:
            problem = plugin.identify(log, found, abort, title=request.get('title') or None,
                                      authors=request.get('authors') or None, identifiers=identifiers, timeout=20) or ''
        except Exception as error:
            problem = str(error)
        books = []
        while True:
            try:
                books.append(found.get_nowait())
            except Empty:
                break
        books.sort(key=lambda book: getattr(book, 'source_relevance', 0))
        answers.append({'plugin': plugin.name, 'error': str(problem),
                        'candidates': [to_candidate(plugin, book) for book in books[:3]]})

    threads = [Thread(target=ask, args=(plugin,), daemon=True) for plugin in plugins]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(30)
    abort.set()
    return answers


def main(arguments):
    if arguments[:1] == ['list']:
        from calibre.customize.ui import all_metadata_plugins
        answer = [describe(plugin) for plugin in all_metadata_plugins()]
    elif arguments[:1] == ['identify'] and len(arguments) == 2:
        with open(arguments[1], encoding='utf-8') as request:
            answer = identify(json.load(request))
    else:
        raise SystemExit('usage: list | identify <request.json>')
    sys.stdout.write('\nOMABOOK-JSON:' + json.dumps(answer) + '\n')
    sys.stdout.flush()


main([argument for argument in sys.argv[1:] if argument != '--'])
