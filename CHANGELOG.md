# Changelog

Changes go under **Unreleased** as they are made. `make release VERSION=X.Y.Z` turns that section into the next version.

## Unreleased

- New: `read json TEXT` turns JSON into lists and records, and `json of VALUE`
  writes any value as JSON (with `<`, `>` and `&` escaped, so it is safe in a
  `<script>`). JSON from a visitor stays marked as a visitor's. Both are in the
  core and need no library.
- New: `fetch "https://..."` reads an address on the web, and
  `fetch "https://..." with RECORD` sends a record as JSON (a POST). A JSON
  reply becomes records and lists, anything else text, and both are escaped
  when shown like a visitor's text. The page always chooses the site: a
  visitor's value may only go after the host, where it is encoded. Keys for an
  API go in `cl.keys` in the site's private folder, one line per host, and are
  only sent to that host. At most 20 fetches a page, 10 seconds and 10 MB
  each; not allowed in `try`. It needs libcurl, in the new module
  `src/mod_curl.c`, which `make` adds when libcurl is installed.
- The module interface is now version 2 (`CL_MODULE_API`): it adds the fetch
  capability. Modules built for version 1 must be rebuilt.
- The built-in server and the `.htaccess` in `docs/deploy.md` never send
  `.keys` files.
- The SQLite and Postgres drivers are now modules, `src/mod_sqlite.c` and
  `src/mod_postgres.c`, behind the small interface in `src/cl_module.h`; the
  interpreter itself no longer includes either library, and `docs/modules.md`
  says how to write a module. Pages do not change. `cl words` now ends with
  the modules in the build. Building by hand with SQLite is
  `cc -DCL_SQLITE -o cl src/cl.c src/mod_sqlite.c -lsqlite3`.
- Fixed: a verb that opened a database while its page already had one open,
  then asked it, read the closed handle. Opening a database now always closes
  the one before it, and the new one answers.
- An address no module can open, like `mysql://...`, now says what this build
  can open instead of looking for a file by that name.

## 0.5.0 (2026-11-01)

- New: `include "/header.html"` starts from the site's top folder (the folder `cl serve` serves, the web root under `cl cgi`, or the folder you run `cl` from), so pages in subfolders can share one header instead of copying it. `..` is still refused.
Security fixes from an outside review. Every site should update.

- Fixed: a stray control byte (`\x01`) inside a text in practice code (`try`) crashed `cl`, on the practice, quest and share pages. It is now an error.
- Fixed: about 2,000 nested brackets, or thousands of `not`s, crashed the parser. Expressions now nest at most 200 deep.
- Fixed: `go to` with a visitor's value accepted `/\evil.example`, which browsers follow to another site. Backslashes are refused there and encoded in every redirect address.
- Fixed: `cl cgi` would run any file the web server pointed it at, so a request like `/cgi-bin/cl/cl.secret` could print the secret. It now runs only `.html` pages and never a private file.
- Fixed: `cl serve` sent memory past the end of its header buffer when a cookie or redirect address was larger than 8 KB. It now answers 500.
- Changed: `hash of` and `matches` are not allowed in practice code (`try`): password hashing is slow on purpose, and a loop of it could tie up the server for 30 seconds.
- Changed: under `cl cgi` over HTTPS, the session cookie is marked `Secure`.
- Docs: put HTML attributes that hold a value in quotes; escaping cannot protect an unquoted attribute.
- Fixed: the documented shared-hosting setup did not work on LiteSpeed, which many cPanel hosts run, because LiteSpeed ignores Apache's `Action` directive. The new `.htaccess` in `docs/deploy.md` uses only rewrite rules and works on Apache and LiteSpeed alike. `cl cgi` now finds the page from `SCRIPT_FILENAME`, `PATH_TRANSLATED` or `DOCUMENT_ROOT` plus `PATH_INFO`, whichever the server gives, and takes the visitor's address from `REQUEST_URI`, so clean addresses like `/blog/hello-world` work behind a rewrite.
- New: under `cl cgi`, a missing page shows the site's own `404.html` with a 404 status, as `cl serve` does.
- Fixed: more hostile practice code that crashed `cl`: `set 5 to` (a block with a number for a name), a long chain like `count of count of ...`, and thousands of nested blocks. Blocks now nest at most 500 deep.
- New: after 10 wrong passwords from one address within 10 minutes, `matches` refuses that address until the 10 minutes are up. Each check is slow on purpose, so this also stops one visitor from using the server's time for guessing.
- Changed: under `cl cgi`, `cl.secret`, `mail.log`, `login.log` and the outbox live outside the web root, in `<the folder above it>/.common/<its name>/` (for example `~/.common/public_html/`), readable only by you, so they no longer depend on `.htaccess` rules. `CL_PRIVATE_DIR` chooses another folder. Existing sign-ins end once, because a new `cl.secret` is made there.
- Fixed: `make` failed on Linux with gcc 14 (`implicit declaration of function 'realpath'`). glibc hides `realpath` under the strict POSIX setting the source asked for, and gcc 14 treats an undeclared function as an error where older versions only warned. The source now also asks for `_DEFAULT_SOURCE`. macOS and Windows were not affected.
- Fixed: on Linux without the `sqlite3` command, `make test` stopped half-way with only `sqlite3: not found`, although `cl` itself worked. The tests now say up front which commands they need and how to install them.
- Fixed: `make test` failed on Linux at "a long chain of count of": the test put a 180 KB argument on the command line, and Linux allows at most 128 KB in one argument, so `cl` never ran. The chain is now 5,000 deep, still far past the limit of 200.
- Fixed: a page with hundreds of blocks or brackets inside each other could crash `cl` on a computer that gives programs a small stack: Windows, some shared hosts, and the sanitizer build on Linux. Each nested block used about 11 KB of stack because the compiler folded the email, include and database helpers into the block runner; it now uses about 1 KB. `cl` also watches how much stack the page uses and stops it with "nested too deeply" before the stack runs out, whatever the computer's limit.

## 0.4.0 (2026-10-29)

Email, so a contact form can really send.

- **License: Common is now under the Apache License 2.0** (it was MIT before its first public release). Added `NOTICE`, `DISCLAIMER.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1), and issue and pull request templates.

- New: `send email to "you@site.com"`, then `subject:`, `body:`, `reply to:` and `from:` lines, then `end`. The recipient and sender must be written by the page, never a visitor's value, so a form cannot be used to send email to anybody. Line breaks in the subject become spaces, so a visitor cannot add headers. Bodies are plain text, and visitor text in them arrives exactly as typed.
- New: `TEXT is an email` (and `is not an email`), to check a visitor's address before replying to it. `email` is still fine as a variable name.
- Under `cl serve` and `cl page.html`, emails are saved in an `outbox/` folder instead of being sent. Under `cl cgi` they go to `sendmail`. `CL_MAIL`, `CL_SENDMAIL`, `CL_MAIL_FROM` and `CL_MAIL_PER_HOUR` change that.
- Limits: 3 emails each time a page runs, and 20 an hour for the site, counted in `mail.log`. `send email` is not allowed in `try`.
- The server never sends `outbox/`, `.eml` or `.log` files.
- Safety: a form posted to a Common site from a page on another site is refused (403), in `cl serve` and `cl cgi`, using the browser's `Origin` header. Posts with no `Origin`, and posts through a proxy that sets `X-Forwarded-Host`, still work.
- Safety: every response says `X-Content-Type-Options: nosniff` and `X-Frame-Options: SAMEORIGIN`, so a browser never runs a file as a script by guessing, and another site cannot show your pages inside a frame.
- Faster: about 8 times faster on loops. Each line is read into tokens once and kept, instead of on every run; a verb's body is split once, not on every call; the words in `[holes]` are kept by their text; and dictionary lookups use a hash table. A loop of a million turns went from 2.96 s to 0.36 s. `make bench` runs a rough speed check.
- Faster: calling a verb no longer copies every name on the page. A loop of 2000 verb calls over a 2000-item list went from 0.45 s to 0.02 s. Verbs still cannot change the caller's names.
- Fixed: text joined with a visitor's value, like `"From [form "name"]"`, was stored HTML-escaped by `tell database`, `ask database ... with` and `remember`, so it showed double-escaped later (`&amp;amp;`). It is now stored and sent as plain text, and escaped once when shown. A `go to` address joined with a visitor's value is no longer HTML-escaped either.
- Fixed: `remember` and `forget everything` inside a verb did nothing, because the change was noted on the verb instead of the page.
- Demo fixes: prices on the products and quote pages are rounded to the cent (the quote page cut digits off, so $61.969 showed as $61.96). Products only confirms "added to cart" for its own products. Recipes treats `?id=abc` as not found on Postgres too, instead of showing a database error. Admin makes clean addresses from any title ("Before/After: Part 2!" becomes `/blog/before-after-part-2`). The guestbook checks name length on the server and shows the newest 50 entries. Blog posts and recipes put their own title in the browser tab.
- The house-cleaning quote page moved from `/cj` to `/quote`, into the Demos section with the site's header and look. `/cj` still goes there. The standalone Bootstrap 5 copy that the zip is built from is `examples/downloads/house-quote.html`.
- Docs: every claim on the demo site was checked against the interpreter and corrected: the sandbox limit (20,000 steps, not 100,000), what the server never sends, sign-ins and the email limit on the install page, untested platforms marked as untested, and the wiki gained an Email section and a word list generated from the dictionary. Tests now check the site's numbers (cards, commands, lines of C, words, version) against the real thing.
- New demo pages: a poll (one vote per visitor), an RSVP page with a seat limit, a waitlist and an email to the host, Share (paste Common code, get a link that shows and runs it), and a to-do list kept in a signed cookie.
- Errors: `for each` over a record, text or a number says what it is and what to write instead. A misspelled column asks "Did you mean 'price'?".
- Fixed: memory leaks on error paths found by the Linux leak checker. Practice code that hit the 64 MB limit lost the text it was building, and errors in `add`, `include` and `try` inside practice code lost a value each.
- The demo contact page now sends its messages, and the house-cleaning quote page has a "Request this clean" form that emails the business.
- New: `cl words --markdown` prints the whole vocabulary, examples and the words Common refuses as Markdown.
- New: `skills/common/`, a skill for AI coding agents, which have never seen Common: the one form of every word (generated by `make agents`), the traps from other languages, the safety rules, how to check a page with `cl`, and tested example pages. `AGENTS.md` guides agents working on the interpreter itself. `make test` fails when the skill is out of date.

## 0.3.1 (2026-10-27)

Safety and correctness fixes from a review. Pages written for 0.3.0 run unchanged, except where they were relying on one of these bugs.

- Safety: `path` is now a visitor's value, like `query`. Before, `show path` (as on the demo 404 page) let a visitor put a script on the page, `include` could be pointed at `cl.secret` through the address, and `path` could be filled into SQL.
- Safety: a visitor's value stays marked through every word that makes new text from it. `trim of`, `replace`, `split`, `paragraphs`, `first of`, `last of`, words that turn lists into text, and `set NAME to` text blocks used to drop the mark, which let a visitor's value into `ask database`.
- Safety: visitor text escapes `'` as well as `"`, so it is safe inside `title='...'`. A visitor's value at the start of `href`, `src`, `action` and similar that begins with `javascript:`, `vbscript:` or `data:` becomes `#unsafe-link`.
- Safety: the dev server refuses private files whatever their case or trailing dots (`cl.SECRET`, `site.DB`), since macOS and Windows treat those as the same file. On Windows it also refuses `:` in an address.
- Safety: a request whose body is shorter than its `Content-Length` is refused. Before, it wrote past the end of the request buffer.
- Safety: `try` counts every turn of a loop as a step, so an empty `repeat 300000000 times` stops at 20,000, and practice code is limited to 64 MB, so text that doubles in a loop stops with an error instead of exhausting the machine. `cl cgi` now gives each page 30 seconds, like the dev server.
- Safety: `hash of` makes PBKDF2-HMAC-SHA256 hashes (`cl2$`, 200,000 rounds). `cl1$` hashes from earlier versions still match.
- Safety: session cookies carry a signed end date 30 days out, so a copied cookie stops working. Cookies from 0.3.0 are ignored once, which signs everyone out. The cookie is found by its exact name.
- Safety: on Windows, salts and the site secret come from `rand_s`. Before, they came from the clock. If no secure random source exists, `cl` stops instead of guessing.
- Fixed: `is more than`, `is less than`, `is at least`, `is at most` and `is between` compare text written as a number with a number as numbers. Before, `query "age" is more than 12` was yes for `age=9`, because "9" comes after "12" as text. A number against other text, or an empty field, is now an error that says so. `query "n" is 3` is also yes for `3.0`.
- Fixed: a text block ends only at a line that is just `end`. A line such as `end of story` inside it is text.
- Fixed: `first of` and `last of` on text give a whole letter, so `last of "café"` is `é`, not half of it.
- Fixed: `date of`, `year of`, `month of` and `day of` refuse dates that do not exist, like 31 February.

## 0.3.0 (2026-10-25)

- Postgres: `open database "postgres://user:password@host/dbname"` and the same `ask` and `tell` words. Optional, detected at build time through `pg_config`, like SQLite.
- `docker/`: Dockerfile and compose file that run the sample site on Postgres 17 with its data loaded. The site's database names moved into `examples/site/database.html` so one file switches between SQLite and Postgres.
- Fixed: in `count of ask database "..." with slug is more than 0` the comparison now applies to the count. Before, it was read as part of the parameter, so the admin page never noticed a duplicate title.
- Safety: a visitor's value may be shown but never run. `ask database` and `tell database` refuse SQL that has a visitor's value filled into it and show the `with ?` form to write instead; `include` and `open database` refuse names that came from a visitor, and `include` refuses `..`; `go to` with a visitor's value must point to a page on this site. Text that had a visitor's value filled in remembers that, so the rules hold through `set` and verbs.
- Dev server: each request gets 30 seconds, so a slow client or a runaway page cannot hold a process forever.
- Memory: whatever an error leaves behind as it unwinds is now freed: the line's token lists, the values an expression was holding, text buffers, and the environments of verbs the error happened inside. Checked with LeakSanitizer on Linux over the test suite and every demo page.
- Demo site: a ☾ / ☀ link in the nav switches light and dark. It is a plain link to `theme.html`, which remembers the choice in the session and sends the visitor back; the header puts the choice on `<body data-theme>` and two CSS lines apply it. The site is dark by default; the link switches to light.
- Learning: the flash cards open with a "Quotes or not?" section and close with "Gotchas" (80 cards now). The guide starts with three habits (quotes and names, `end`, `to` stores and `is` compares) and ends with an "If it does not run" checklist.
- Errors: `if status is "At Desk"` when `status` is a list now stops and says the list cannot be one value, and shows the `contains` line to write instead. Before, it quietly answered no.
- Errors: a stray word at the end of a line now names the Common word when one exists, so `if m says "x"` answers "In Common that is written 'is'". `says`, `eq` and `isnt` joined the synonym list.
- `cl serve` accepts `host:port`, such as `0.0.0.0:8000`, to accept connections from other machines or from outside a container. Plain `8000` still listens on this machine only.

- The output command is `show`, not `say`: a page shows things, it does not say them. Every page, lesson, card and error was updated. Writing `say` now gets an error that teaches `show`.

## 0.2.0 (2026-10-24)

- Verbs: `to NAME ARG … end` defines your own command; `give VALUE` hands a value back so a verb can be used as a value. Verbs read the page's variables and cannot change them.
- Records you make: `set NAME to record … end` with `column: VALUE` lines; `set COLUMN of RECORD to VALUE`; `item "column" of record`.
- `repeat N times until CONDITION`: stop early, with the number as the ceiling.
- Reads like a sentence: `the` is silent, a line may end with a full stop, `is between A and B`.
- Text blocks: `say` (or `set NAME to`) on its own line, the text as typed, then `end`.
- Noun-like words such as `year`, `count` and `date` may be variable names; they are Common words only before `of`.
- Forgiving hints: text without quotes after `say`, `set` or `print` gets the corrected line.
- Errors for unclosed blocks name the block and say what to add; form errors state the whole line.
- `try` runs text as Common in a sandbox; `dictionary` gives every word as records; `say code` highlights code from a value.
- Demo site: a Practice page with Scratchpad, Lessons and Exercises tabs, snippet buttons, hints, a nudge for bracket-less variables, and answers after three tries; a glossary generated from the dictionary; a wiki; flash cards; a build-a-guestbook guide; Bootstrap 6 alpha theme with dark mode.

## 0.1.0 (2026-10-24)

The first release. Common (CL) is a programming language written in plain
English that lives inside HTML pages and runs on the server.

### Language
- Output, variables, text with `[values]` inside, math, conditions, lists, loops, includes.
- Visitor input with `query`, `form` and `path`; `today` and `now`; `go to` redirects.
- Text words: `uppercase`, `lowercase`, `trim`, `split`, `replace`, `paragraphs`, `number of`,
  `first N of`, `last N of`, `sort of`, `reverse of`, `contains`.
- Dates: `date of`, `year of`, `month of`, `day of`.
- SQLite: `open database`, `ask database ... with`, `tell database ... with`, `column of record`.
- Sessions in a signed cookie: `remember`, `remembered`, `forget`, `forget everything`.
- Passwords: `hash of`, `matches`.
- `try` runs text as Common in a sandbox, for practice pages. `dictionary` gives every word as records. `say code` highlights code from a value.
- A strict dictionary with one form per word, and errors that name the right word or state the whole line.
- Noun-like words such as `year`, `count` and `date` may be variable names; they are Common words only before `of`.
- Reads like a sentence: `the` is silent, a line may end with a full stop, `is between A and B`.
- Text blocks: `say` (or `set NAME to`) on its own line, the text as typed, then `end`. No quotes, brackets still work. The way to write HTML.
- Forgiving hints: text written without quotes after `say`, `set` or `print` gets the corrected line, ready to copy.
- Text from visitors and the database is escaped automatically.

### Tools
- One C file, no required dependencies; SQLite is detected at build time.
- `cl page.html` runs a page. `cl serve` is a dev server with clean URLs, a
  custom 404, POST, cookies and one process per request. `cl cgi` runs behind
  Apache or nginx. `cl words` and `cl version`.
- `<cl-code>` shows highlighted code on a page.
- A VS Code extension in `editor/vscode`.

### Demo
- `examples/site`: a Bootstrap 6 site with a blog, guestbook, products, contact,
  sign-in and admin, learn, flash cards, wiki, install and errors pages.
- `examples/cookies`: a recipe book on SQLite.
