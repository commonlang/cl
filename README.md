# Common (CL)

**Common**, or **CL** for short, is a free and open-source programming language for the web written in plain English.
Website: [commonlang.org](https://commonlang.org).
Code lives in HTML pages inside `<cl>` tags, and the server runs it before the
page reaches the browser, the way PHP works. The browser never sees Common.

```html
<cl>
  set visitor to query "name"
  if visitor is empty
    set visitor to "friend"
  end
</cl>
<h1>Hello, <cl>show visitor</cl>!</h1>

<cl>
  set fruits to "apple", "banana", "cherry"
  show "<p>We have [count of fruits] fruits.</p>"
  for each fruit in fruits
    show "<li>[fruit]</li>"
  end
</cl>
```

No braces, no semicolons, no `$`, no `+` between strings: a value goes
inside the text as `"Hello, [name]!"`. Text typed by a visitor is made safe
automatically. Every word in the language is in [the dictionary](docs/dictionary.md),
with exactly one way to use it, and a wrong word gets an error that names the
right one:

```
Line 12: 'print' is not a Common word. Use 'show'
Line 15: Common has no '>'. Write 'is more than'
```

## Try it

Needs a C compiler. No other dependencies. SQLite support switches on by
itself when SQLite is installed, which it is on macOS and most Linux systems,
and `fetch` (reading other sites and JSON APIs) when libcurl is. Works on macOS, Linux and Windows;
see [docs/install.md](docs/install.md) for each platform.

```sh
make                          # builds ./cl
./cl examples/hello.html      # run a page, print the finished HTML
./cl serve 8000 examples      # serve a folder, like `php -S`
./cl cgi                      # run behind Apache or nginx, see docs/deploy.md
./cl words                    # print every word in the language
make test                     # run the tests
```

Then open http://localhost:8000/hello. Any `.html` file in the folder is a
page, at its name without the extension.

## The language in one screen

A page is HTML with Common inside `<cl>` tags. Open it as `/page?name=Ada`:

```html
<cl>
  set name to trim of query "name"
  if name is empty
    set name to "friend"
  end
  show "<h1>Hello, [name]!</h1>"

  set fruits to "apple", "banana", "cherry"
  add "plum" to fruits
  show "<p>[count of fruits] fruits:</p>"
  for each fruit in fruits
    show "<li>[uppercase of fruit]</li>"
  end
</cl>
```

The words you will use most:

| To | Write |
|---|---|
| Show something | `show "Hello, [name]!"` |
| Keep a value | `set total to price * 3` |
| Make a list, add to it | `set list to "a", "b"` · `add "c" to list` |
| Decide | `if age is at least 18` … `otherwise` … `end` |
| Repeat | `for each item in list` … `end` · `repeat 3 times` … `end` |
| Compare | `is` · `is not` · `is more than` · `is at least` · `is empty` · `and` · `or` · `not` |
| Read the visitor | `query "name"` · `form "email"` · `path` |
| Work with text | `trim of x` · `uppercase of x` · `split x by "/"` · `replace "a" with "b" in x` |
| Work with lists | `count of x` · `first of x` · `last of x` · `item 2 of x` |
| Dates | `today` · `now` · `date of today` |
| A database | `open database "site.db"` · `ask database "select …" with a` · `tell database "insert …" with a, b` |
| Build a site | `include "header.html"` · `go to "/thanks"` |
| Remember a visitor | `remember "user" as name` · `remembered "user"` · `forget everything` |
| Send email | `send email to "you@site.com"` … `end` · `is an email` |
| Your own commands | `to greet someone` … `end` · `give value` |

Every word has exactly one form. `./cl words` lists them all.

To show Common code on a page, wrap it in `<cl-code>`: the interpreter
escapes it and marks its own words so a stylesheet can color them.

Full reference: [docs/dictionary.md](docs/dictionary.md).
A guided tour of the basics: [docs/basics.md](docs/basics.md).

## Writing Common with an AI agent

AI coding agents have not seen Common, and Common refuses guesses. Give them
the skill in `skills/common/`: the one accepted form of every word, the traps
from other languages, the safety rules, and tested example pages.

```sh
# Claude Code, for every project:
mkdir -p ~/.claude/skills && cp -R skills/common ~/.claude/skills/
# or for one project: copy it to that project's .claude/skills/
```

Other agents can read `skills/common/SKILL.md` directly, for example by
pointing to it from the project's `AGENTS.md`. The word list in it is generated
from the interpreter (`cl words --markdown`), so it always matches the version
you built. `AGENTS.md` in this repository is for agents working on Common itself.

## Layout

```
src/cl.c          the interpreter and dev server: the language, in one file
src/cl_module.h   the module interface; mod_sqlite.c, mod_postgres.c and mod_curl.c are the modules
docs/             dictionary, design notes, install and deploy guides
examples/         hello.html
editor/vscode/    syntax highlighting for VS Code
skills/common/    the skill for AI agents (SKILL.md is generated: make agents)
tests/            run.sh plus a feature fixture with expected output
```

## Status

Works today (see [CHANGELOG.md](CHANGELOG.md) for each version, and `cl version` for yours): output, variables, text, math, lists, conditions,
loops, verbs of your own with give, records, text blocks for HTML, includes, GET and POST forms, dates, text words, reading and writing a
SQLite or Postgres database (plain English outside, real SQL inside the quotes,
parameters bound safely), sign-ins with signed cookies and hashed passwords,
email from forms (`send email`, saved to `outbox/` while developing, sendmail when hosted),
JSON (`read json`, `json of`) and other sites' APIs (`fetch`, keys kept in a private file),
redirects, clean URLs, a custom 404 page, a dev server that forks per
request, a CGI mode for real hosting, and a VS Code extension in
`editor/vscode`. Strict vocabulary with helpful errors, automatic escaping.
Not yet: file uploads, rate limiting beyond email, SMTP without sendmail.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. The short version: the
language stays small, every word has one form, and nothing lands without a
dictionary entry and a test. Everyone taking part follows the
[Code of Conduct](CODE_OF_CONDUCT.md). Report security problems privately, as
described in [SECURITY.md](SECURITY.md).

## License

Common is free and open source, under the [Apache License, Version 2.0](LICENSE).
You may use it, copy it, change it, fork it, redistribute it and use it
commercially, under the terms of that license. Copyright notices are in
[NOTICE](NOTICE).

Common is provided **as is, without warranties of any kind**. Nobody provides
paid support, maintenance, security response or performance guarantees
unless separately agreed in writing. Before relying on it, read
[LICENSE](LICENSE), [DISCLAIMER.md](DISCLAIMER.md), [SECURITY.md](SECURITY.md)
and [CONTRIBUTING.md](CONTRIBUTING.md).
