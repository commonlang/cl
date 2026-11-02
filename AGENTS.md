# Working on Common (CL)

Common is a plain-English web language. This repository is its interpreter,
written in C. To **write Common pages**, read
[skills/common/SKILL.md](skills/common/SKILL.md): it lists every word with its
one accepted form, the traps from other languages, and the safety rules.
Do not guess the syntax.

Agents that maintain this repository on their own (triage, fixes, security
patches, releases) must also follow [MAINTAINING.md](MAINTAINING.md): changes
reach `main` only through pull requests that a person approves, agents run the
full checks themselves and post the results, and agents work only on issues a
person labelled `agent-ok`.

## Layout

- `src/cl.c` is the interpreter, dev server and CGI mode: the language itself,
  in one C11 file. The vocabulary is the `DICTIONARY` table near the top;
  `SYNONYMS` maps words from other languages to the Common word to use instead.
- `src/cl_module.h` is the module interface. `src/mod_sqlite.c` and
  `src/mod_postgres.c` are the two modules, each wrapping one library behind
  it; the core never includes a library. `docs/modules.md` says how to write
  one. Words, parsing and the safety rules stay in the core.
- `docs/dictionary.md` is the reference for people. `docs/deploy.md` covers hosting.
- `tests/run.sh` is the test suite. `tests/features.html` and
  `tests/features.expected` pin the output of every feature and error.
- `editor/vscode/` is syntax highlighting. `tests/check_grammar.py` fails when
  a dictionary word is missing from it.
- `skills/common/` is the skill for AI agents. `SKILL.md` is generated: edit
  `skills/SKILL.head.md` or the dictionary, then run `make agents`.

## Build and test

```sh
make              # build ./cl (SQLite and Postgres are added when installed)
make test         # the test suite; must pass before any change is done
make sanitize     # the tests under AddressSanitizer and UBSan, then make clean (rebuild after)
make agents       # regenerate skills/common/SKILL.md after changing words
make bench        # a rough speed check: run before and after a change to the interpreter
make release VERSION=X.Y.Z  # prepare a release; see MAINTAINING.md. Never tags or pushes
```

## Rules for changes (from CONTRIBUTING.md)

- One form per word. Never add a second way to write something; add the other
  spelling to `SYNONYMS` so the error points to the real word.
- Every new word needs a `DICTIONARY` entry with a working example, a section
  in `docs/dictionary.md`, the VS Code grammar, a test, and `make agents`.
- Errors teach: say what went wrong and show the line written correctly.
- Visitor values (`query`, `form`, `path`, `remembered`, database text) are
  marked `untrusted`/`tainted` and must never reach SQL text, file names,
  `go to` off the site, or email recipients. Keep that true for new words.
- Only optional libraries, detected by the Makefile. Plain C11 otherwise.
- Write for readers new to programming: plain words, short sentences.

## Housekeeping

Small chores that keep the repository honest. Do them as part of every change,
and go over the list whenever you finish a piece of work.

- **Test on Linux too.** macOS hides Linux-only failures: a newer gcc refuses
  what clang allows, Linux caps one command-line argument at 128 KB, Debian does
  not ship the `sqlite3` command with the library, and gcc's sanitizer uses far
  more stack per call. Any change to `src/`, `tests/` or the `Makefile` is
  also built and tested in a Linux container with a current gcc (14 or newer),
  including `make sanitize` with `ASAN_OPTIONS=detect_leaks=1`. CONTRIBUTING.md
  ("Checking for leaks") shows the Docker command; `debian:trixie-slim` with
  `apt install gcc make libsqlite3-dev sqlite3 curl python3 procps` works too.
- **Examples run as pasted.** Every code block in `README.md`, `docs/` and
  `skills/SKILL.head.md` must work when copied into a page on its own. When you
  touch one, run it through `./cl`. The tests already run the skill's blocks.
- **The changelog is complete.** Every user-visible change that merged has a
  line under `## Unreleased`. Check this after each merge, not only your own.
- **Releases do not lag.** `## Unreleased` is not a place to store finished
  work. Once it holds a security fix, release at once; otherwise release when
  something has waited there more than two weeks. Fixes only make a patch
  release, which an agent may prepare with `make release` and tag once merged.
  A new word or a behavior change makes a minor release, which a person
  decides: open an issue that proposes the version and lists what is waiting.
  Every version in `CHANGELOG.md` should have a `vX.Y.Z` tag and a GitHub
  release with the changelog section as its notes; if one is missing, say so.
- **Branches.** Delete your own branch once its pull request has merged. Never
  delete anyone else's.
