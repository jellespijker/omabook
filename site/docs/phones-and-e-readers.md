<!-- Phones and e-readers: https://jellespijker.github.io/omabook/docs/phones-and-e-readers.html -->

# Phones and e-readers

Both are off until you choose them in Settings, under *E-readers and phones*, which also says where Omabook listens: on this computer, on your tailnet, or (with a warning) on every network.

## OPDS

Omabook serves the library, or one collection, as an OPDS catalog. KOReader, Moon+ Reader, Librera and Thorium browse and download from it. It is read-only, needs its password, and never offers a book a plugin keeps private.

## KOReader

Omabook also serves KOReader’s progress sync, so KOReader keeps its reading place in step with the library’s, matched by the hash KOReader names a file by. Or sync reading places with a KOReader sync server elsewhere: `omabook koreader login`, `omabook koreader sync`.

## Send a book

Send a book to an e-reader mounted over USB (Kobo, Kindle, PocketBook), converted with Calibre when needed, or mail it to a Kindle: `omabook send`. `omabook opds` browses other OPDS catalogs. `omabook serve --reach tailnet` serves from the command line.
