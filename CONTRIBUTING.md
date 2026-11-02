# Contributing to Common

Common's whole value is that it stays small and readable. Pull requests that
make it bigger without making it clearer will be declined, kindly. Here is
what "simple but capable" means in practice.

Contributions of every size are welcome: a fixed typo, a clearer error
message, a test, a bug fix, or a new word that a maintainer has agreed to.
By taking part you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## The rules

1. **One way to say each thing.** A new feature gets one form. No aliases, no
   optional words, no "also accepts". If users guess a different phrasing, add
   it to `SYNONYMS` in `src/cl.c` so the error points them to the real word.
2. **Every word is in the dictionary.** A feature is not done until it is in
   `docs/dictionary.md` and in the `DICTIONARY` table in `src/cl.c`, with its
   form and a one-line meaning. `cl words` must show it.
3. **Every feature has a test.** Add a case to `tests/features.html`, review
   the new output, then refresh `tests/features.expected`. Error paths count
   as features: a new error message needs a test too.
4. **Errors teach.** An error says what went wrong and what to write instead,
   in plain English, with the HTML line number. No jargon, no "syntax error".
5. **Safe by default.** Anything that comes from a visitor is untrusted and
   escaped. A new source of visitor data (forms, cookies, files) must set
   `untrusted` on the values it produces.
6. **A small core, no required dependencies.** The language, the dev server
   and the safety rules stay in `src/cl.c`, C11, building with `make` on
   macOS, Linux and Windows with nothing installed. A library is never
   included there: it goes in a module, `src/mod_NAME.c`, behind the interface
   in `src/cl_module.h`, optional and detected by the Makefile, and the build
   works without it. SQLite and Postgres are the models; `docs/modules.md`
   says how to write one.
7. **Lowercase words, lines, and `end`.** No new punctuation. No braces, no
   sigils, no symbols beyond `+ - * / ( ) , " #`.

## Reporting a bug or asking for a feature

- **Bugs:** open an issue with the smallest `.html` page that shows the
  problem and the output of `./cl page.html` (with the query, form or path
  you used). Say which version (`./cl version`) and system you are on.
- **Security problems:** do not open a public issue. Follow
  [SECURITY.md](SECURITY.md).
- **New words or forms:** open an issue first and describe the problem you
  want to solve in Common today. Language changes are decided by the
  maintainers before any code is written, because every word is forever.

## Submitting a pull request

1. Fork the repository and make a branch for one change: one word, one fix,
   or one piece of documentation.
2. Build and test:

   ```sh
   make && make test    # must print "All tests passed"
   make sanitize        # must pass clean under AddressSanitizer and UBSan
   make                 # sanitize ends with make clean, so build again
   make agents          # if you changed a word, its meaning or example
   ```

   The tests also use `curl`, `python3`, `pkill` and, when `cl` has SQLite, the `sqlite3` command. macOS has them all. Debian/Ubuntu: `apt install curl python3 procps sqlite3`.

3. Follow the rules above: a dictionary entry, docs, the VS Code grammar and
   a test for every new word or error.
4. Open the pull request, fill in the template, and explain why the change
   belongs in a language this small.

The maintainers may accept a contribution, ask for changes, or decline it,
including for reasons of scope or style. A declined pull request is not a
judgement of you or your work.

## Legal

- **License of contributions.** Unless you clearly say otherwise in writing,
  any contribution you intentionally submit for inclusion is licensed under
  the [Apache License, Version 2.0](LICENSE), as described in Section 5 of
  that license. You keep the copyright in your contribution.
- **Your right to submit.** Only submit work that you wrote yourself, or that
  you otherwise have the legal right to submit under the Apache License 2.0
  (for example, your employer allows it). If part of a contribution comes
  from someone else, say so in the pull request, with its source and license.
- **No proprietary code, secrets or unlicensed material.** Do not submit code,
  text, images or data that you do not have permission to share, and never
  submit passwords, keys, tokens, personal data, databases or `cl.secret`
  files.
- **Third-party code.** Common has no third-party source code and its only
  libraries are optional (SQLite and libpq, used from the system). Adding
  third-party code or a new library needs a maintainer's agreement first, and
  its license must be compatible with the Apache License 2.0.

## Good first contributions

- Running `make test` on Windows and reporting what fails, or a GitHub Actions workflow that runs it on Linux, macOS and Windows.
- Publishing the VS Code extension in `editor/vscode` to the marketplace.
- Rate limiting for public forms.
- File uploads.
- A per-request memory arena in `src/cl.c`, so error paths cannot leak.
- Watching a newcomer write their first page for an hour, and filing what they stumble on.

## Before you open a pull request

The short version: `make test` and `make sanitize` must pass, and the change
must keep to one word or one fix. See "Submitting a pull request" above.

## Checking for leaks

macOS has no LeakSanitizer, so run it in a Linux container from the repository
root (the first run downloads the image):

```sh
docker run --rm -v "$PWD":/w -w /w -e ASAN_OPTIONS=detect_leaks=1 gcc:13 sh -c \
  'apt-get update -qq && apt-get install -y -qq sqlite3 libsqlite3-dev curl python3 >/dev/null && make sanitize'
```

Errors are the hard case: `fail()` is a longjmp, so anything a frame was holding
would leak. Token lists, held values, text buffers and verb environments are
noted in small registries (`g_live`, `g_held`, `g_bufs`, `g_frames`) and
`run_block` frees whatever is left above its mark when an error arrives. When
you add a parse branch that holds a value across another parse or a `fail()`,
wrap it in `held_push` / `held_pop`.

## Releasing (maintainers)

Each change adds a line under `## Unreleased` in `CHANGELOG.md`. To turn those
into a version:

```sh
make release VERSION=0.4.1   # sets cl version, dates the changelog, runs every test
git commit -am "Release 0.4.1"
git tag v0.4.1
git push && git push --tags
```

`make release` refuses a version that is not newer or an empty Unreleased
section, and puts everything back if a test fails. It never commits or pushes.
Use the next patch number for fixes (0.4.1), the next minor for new words or
changed behavior (0.5.0). Then rebuild `cl` for your server: a binary does not
update itself.

