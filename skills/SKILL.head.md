---
name: common
description: Write, read and debug Common (CL), a plain-English language for web pages. Use when a project has .html pages with <cl> ... </cl> blocks, when the user mentions Common, CL or the cl interpreter, or when asked to build a page, form or site in Common. Covers the one accepted form of every word, the traps that come from other languages, the safety rules for visitor values, and how to check a page with cl before finishing.
---

# Writing Common (CL)

Common is a small, strict web language. Code goes in `<cl> ... </cl>` blocks
inside an `.html` page; everything outside the blocks is sent as written. The
`cl` program runs the page on the server and replaces each block with what it
showed.

You have not seen Common in training. Do not guess from PHP, Python or
English: Common has exactly one form for every word and refuses everything
else. The full vocabulary is at the end of this file. When unsure, run
`cl words`.

```
<cl>
  set name to trim of query "name"
  if name is empty
    set name to "friend"
  end
  show "<h1>Hello, [name]!</h1>"
</cl>
```

## Check every page before you finish

Common's errors say exactly what to change, so run the page and read them.

```sh
cl page.html                          # print the finished HTML
cl page.html "name=Ada"               # with a query string (a GET)
cl page.html "" "sent=yes&name=Ada"   # with POST form data
cl page.html "" "" "/blog/hello"      # with an address path
cl page.html "" "" "" "cl_session=..."  # with a cookie printed by an earlier run
cl serve 8000 site                    # serve a folder: /about runs about.html
cl words                              # every word, one form each
```

- Query and form data are URL-encoded, as a browser sends them: `+` or `%20`
  for a space, `%40` for `@`, `%0A` for a new line.
- An error appears in the output as `<pre class="cl-error">Common error: Line N: ...</pre>`.
  Search the output for `Common error` after every change. Line numbers count
  lines of the `.html` file.
- A page that runs `go to` prints nothing on stdout. stderr says
  `cl: this page goes to /thanks`, and `cl: set-cookie: ...` when it remembered
  something. Pass that cookie back as the fifth argument to test the next page.
- Running a page does its work. `tell database` really writes, so test pages
  that write against a copy of the database. `send email` saves the email as a
  file in `outbox/` (in the page's folder for `cl page.html`, the site folder
  for `cl serve`) and only really sends under `cl cgi`. Each send is also
  counted in `mail.log` next to it: after 20 in an hour, sending stops with an
  error. Delete `mail.log` to reset the count while testing.

## The rules

1. One command per line, and every line starts with a command: `show`, `set`,
   `add`, `if`, `otherwise`, `end`, `for each`, `repeat`, `include`, `open`,
   `tell`, `go to`, `remember`, `forget`, `send email`, `to`, `give`, or the
   name of a verb you defined. The exceptions are the lines inside a block of
   their own: `column: VALUE` lines in a record or `send email`, and the text
   of a text block.
2. Everything is lowercase. No semicolons, no braces, no `=`, `==`, `>`, `<`, `!`.
3. `if`, `for each`, `repeat`, `to`, `send email`, a record and a text block
   are closed by a line that says just `end`. Indent the lines inside by two
   spaces.
4. Text is in double quotes. A value goes inside text in square brackets:
   `"Hello, [name]! You have [count of items] items."` Any value works in the
   brackets, including math and quoted text: `"[uppercase of "hi"]"`. Escapes
   inside quotes: `\"`, `\n`, `\\`, and `\[` for a real bracket.
5. `+` only adds numbers. There is no string concatenation: build text with
   brackets, `set line to "[line], [word]"`.
6. Names are letters, digits and `_`. A name cannot be one of the words listed
   under "Words that cannot be names" at the end of this file, such as `text`,
   `page`, `list`, `form`, `query`, `path` or `end`. Everything else is fine,
   including words only used before `of` (`count`, `first`, `date`, `email`).
7. `the` is silent and a full stop at the end of a line is ignored, so
   `show the count of items.` is fine.

## Traps from other languages

| Instead of | Write |
|---|---|
| `x = 5`, `let x = 5` | `set x to 5` |
| `print x`, `echo x` | `show x` |
| `"Hi " + name`, `f"Hi {name}"` | `"Hi [name]"` |
| `else`, `elif`, `else if` | `otherwise`, `otherwise if CONDITION` |
| `if a == b`, `a != b` | `if a is b`, `if a is not b` |
| `a > b`, `a >= b`, `a <= b` | `a is more than b`, `a is at least b`, `a is at most b` |
| `x in list`, `list.includes(x)` | `list contains x` |
| `len(x)` | `count of x` |
| `list.append(x)` | `add x to list` |
| `while`, `for i in range(n)` | `repeat N times`, `repeat 100 times until done`, `for each x in list` |
| `int(x)` | `number of x` (needed before math on text, such as a query value) |
| `return x` | `give x` (only inside a verb) |
| `function f(a, b)` and `f(1, 2)` | `to f a b` ... `end`, then `f 1 2` |
| `true`, `false` | `yes`, `no` |
| `null`, `""`, `[]` | `""` for no text, `empty list` for no items; test either with `is empty` |
| `{"name": "x"}` | `set r to record`, then `name: "x"` lines, then `end` |
| `header("Location: /x")` | `go to "/x"` |
| `$_POST["x"]`, `request.form` | `form "x"` (POST) or `query "x"` (GET) |
| `mail()` | `send email to "you@site.com"` block, see below |
| `JSON.parse(t)`, `json.loads(t)` | `read json t` |
| `JSON.stringify(x)`, `json.dumps(x)` | `json of x` |
| `fetch(url)`, `requests.get(url)`, `curl` | `fetch "https://..."` (a JSON reply comes back as records and lists) |

## Verbs (your own commands)

```
to card heading blurb
  show "<section><h2>[heading]</h2><p>[blurb]</p></section>"
end
to slug_of words
  give replace " " with "-" in lowercase of trim of words
end

card "Fast" "One file."              # values are separated by spaces, not commas
show "[slug_of "Hello World"]"       # a verb that gives is used as a value
```

- Arguments are separated by spaces. A list cannot be written inline as an
  argument: `set sizes to "S", "M"` first, then pass `sizes`.
- Use parentheses for math in an argument: `double (2 + 3)`.
- A verb sees the caller's variables, but anything it sets stays inside it.
  The only way to hand something back is `give`. To change a variable, set it
  from the result: `set total to add_tax total`.
- Verbs defined in an included file work on every page that includes it.

## Pages, forms and the server

- `/about` serves `about.html`. `/blog/hello-world` serves `blog.html`, which
  reads the rest from `path`: `set slug to last of split path by "/"`.
- `form "x"` is POST data, `query "x"` is the URL query. Both are text, and
  empty text when missing. There is no word for the request method: to know a
  form was submitted, give it a hidden field, `<input type="hidden" name="sent"
  value="yes">`, and test `if form "sent" is "yes"`.
- Handle a form at the top of the page, before any HTML: read, check, act, then
  `go to` a thanks address so a refresh does not submit again.
- `include "header.html"` runs another file in place, next to the page;
  `include "/header.html"` starts from the site's top folder, so a page in
  `shop/` can use the same header (`..` is never allowed). Included files share
  the page's names and verbs. The usual layout: each page sets `title`, then
  includes a header (which includes a settings file), then its content, then
  includes a footer. See `examples/site/`.
- A `404.html` in the folder is shown for any missing address.
- `remember "user" as name`, `remembered "user"`, `forget everything` keep
  small values for a visitor in a signed cookie.

## Safety: what Common refuses, and the pattern to use

Values that came from a visitor (`query`, `form`, `path`, `remembered`, text
read from a database, and anything `fetch` brings back) are marked, and stay marked through every word that
makes new text from them.

- **Showing them is always safe.** They are HTML-escaped when shown or placed in
  text. Never escape by hand. `show raw` turns escaping off: only use it for
  HTML you wrote. Always quote HTML attributes that hold a value
  (`href='[url]'`): in an unquoted attribute a space ends the value, and the
  escaping cannot help.
- **SQL:** never put a visitor's value inside the SQL text. Write `?` and pass
  the values after `with`:
  `ask database "select * from posts where slug = ?" with slug`.
- **include, open database:** file names must be written in the code. To choose
  between files, use `if` with fixed names.
- **go to:** a visitor's value may only go to an address on this site starting
  with `/`.
- **send email to:** the recipient and `from:` must be addresses written in the
  code. A visitor's address goes in `reply to:`. Check it first with
  `if email is not an email`.
- **fetch:** write the address in quotes right after `fetch`. A visitor's value
  may only go after the host, `fetch "https://api.example.com/posts/[slug]"`,
  and is encoded there. API keys go in `cl.keys` in the site's private folder
  (`api.example.com Authorization: Bearer KEY`), never in a page.

## Databases

```
open database "site.db"                     # SQLite file next to the page, or "postgres://user:pw@host/db"
set posts to ask database "select title, slug from posts order by id desc"
for each post in posts
  show "<a href='/blog/[slug of post]'>[title of post]</a>"
end
tell database "insert into notes (body, posted) values (?, ?)" with note, now
```

- `ask` gives a list of records, even for one row. Use `first of`, `for each`
  or `count of`. `COLUMN of RECORD` reads a column.
- Name every computed column with `as`, or it cannot be read:
  `set total to total of first of ask database "select count(*) as total from signups"`.
- Numbers come back as numbers, so `if total is 1` and math work directly.
  Text from the database is marked like a visitor's value.
- `now` is the local date and time as text, like `2026-10-28 18:30`, and
  `today` the date, like `2026-10-28`. Both sort correctly as text in SQL.
- The SQL inside the quotes is ordinary SQL. The database file must already
  exist: `open database` does not create it.

## Email

```
send email to "owner@example.com"
  subject: "Message from [name]"
  reply to: email
  from: "website@example.com"
  body: "[name] wrote:\n\n[message]"
end
go to "/contact?thanks=yes"
```

`subject:` and `body:` are required, `from:` is required unless `CL_MAIL_FROM`
is set. The body is plain text. At most 3 emails per page run and 20 an hour.
Not allowed inside `try`. See `examples/contact.html`.

## JSON and other sites

```
set posts to fetch "https://api.example.com/posts"
for each post in first 5 of posts
  show "<li>[title of post]</li>"
end
set order to record
  item: "lamp"
  qty: 2
end
set reply to fetch "https://api.example.com/orders" with order
show "Order [id of reply] is saved"
```

- A reply that says it is JSON becomes records and lists; anything else is
  text. Go deeper one `of` at a time: `title of attributes of article`. A
  column that is not one word: `item "field-image" of article`.
- `read json TEXT` reads JSON from a form or a text. `json of VALUE` writes it;
  answer a script's request with `show raw json of VALUE`.
- JSON written inside quotes needs `\"` for its quotes and `\[ \]` for its
  brackets, or they become holes: `read json "\[1, 2\]"`.
- A reply that is not a success stops the page with an error that says why.
  At most 20 fetches a page run, 10 seconds each. Not allowed inside `try`.
  `fetch` needs the curl module: `cl words` lists `fetch` when the build has it.

## Examples in this skill

- `examples/contact.html`: a form that validates and sends email.
- `examples/posts.html`: a list and a detail page from one file, with clean URLs.
- `examples/site/`: the header, settings and footer layout, and a verb for
  repeated markup.

