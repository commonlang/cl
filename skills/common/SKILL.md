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

## Every word

Generated by `cl words --markdown` from the interpreter's own dictionary (Common 0.5.0). Each word has exactly one form. Anything not listed here is an error.

### Commands: the first word of a line

| Form | Meaning |
|---|---|
| `show VALUE  /  show ... end` | show a value on the page (visitor text is made safe). On its own, show opens a block of text that runs until a line saying end |
| `set NAME to VALUE` | create or replace a variable; VALUE, VALUE makes a list. 'set COLUMN of RECORD to VALUE' changes one column |
| `add VALUE to LIST` | put a value at the end of a list |
| `if CONDITION` | run the lines below when the condition is yes |
| `otherwise [if CONDITION]` | what to run when the condition above was no |
| `end` | close an if, for each, repeat, to, send email, record or text block |
| `for each NAME in LIST` | run the lines below once for every item |
| `repeat NUMBER times [until CONDITION]` | run the lines below that many times, or fewer with 'until CONDITION': stop as soon as it is yes |
| `include "file.html"` | insert another page here. A name starting with / starts from the site's top folder: include "/header.html" |
| `open database "file.db"` | open a SQLite file next to the page, or a Postgres server with postgres://user:password@host/dbname |
| `tell database "SQL" with VALUE, ...` | run SQL that changes the database (insert, update, delete) |
| `go to "/page.html"` | send the visitor to another page and stop |
| `remember "name" as VALUE` | keep a value for this visitor until they forget it (a signed cookie) |
| `forget "name"  /  forget everything` | drop a remembered value, or all of them (sign out) |
| `send email to "you@site.com"  then subject:, body:, reply to:, from: lines, then end` | send an email to an address you wrote yourself (never a visitor's). While developing it is saved in the private outbox/ folder; behind a web server it goes through sendmail. At most 3 per page and 20 an hour |
| `to NAME ARG ...  then lines, then end` | define a verb: your own command. Call it by name with one value per ARG. Inside, 'give VALUE' hands a value back |
| `give VALUE` | inside a verb: hand this value back and finish. A verb that gives can be used wherever a value goes |

### Values, conditions and the rest

| Form | Meaning |
|---|---|
| `"text with [a value] inside"` | put a value into text. A value is anything you could write after 'set x to': a name, math, count of, query. Write \[ for a real bracket |
| `show raw VALUE` | show text exactly as it is, with no safety |
| `show code TEXT` | show Common code highlighted, the way a cl-code block does, but from a value |
| `yes` | the answer yes |
| `no` | the answer no |
| `count of LIST` | how many items a list has, or letters in text |
| `first of LIST  /  first N of LIST` | the first item, or the first N items (or letters of text) |
| `last of LIST  /  last N of LIST` | the last item, or the last N items (or letters of text) |
| `item N of LIST` | one item by position, counting from 1; or a column by name: item "price" of lamp |
| `query "name"` | what the visitor typed in the URL after ?name= |
| `path` | the URL path, like /about. The visitor typed it, so it is made safe like query |
| `try TEXT` | run text as Common in a sandbox and give back what it said. No files, database, cookies, redirects, email or password hashing; loops stop after 20000 steps and memory at 64 MB. For practice pages |
| `dictionary` | every word of the language as records: word, form, meaning, example |
| `form "field"` | what the visitor typed in a submitted form (POST) |
| `remembered "name"` | a value kept for this visitor, or empty text |
| `hash of TEXT` | a one-way scramble of a password, safe to store (PBKDF2-HMAC-SHA256) |
| `TEXT matches HASH` | yes if the text is the password behind the hash |
| `TEXT is an email` | yes if the text is one email address, like ada@example.com. Check a visitor's address with it before you use it |
| `A contains B` | yes if text A has B inside it, or list A has item B |
| `sort of LIST` | the list in order (numbers by size, text A to Z) |
| `reverse of LIST` | the list back to front |
| `date of TEXT` | a date like 2026-10-24 written out: 24 October 2026 |
| `year of TEXT` | the year of a date as a number |
| `month of TEXT` | the month of a date as a name |
| `day of TEXT` | the day of a date as a number |
| `today` | the date, like 2026-10-24 |
| `now` | the date and time, like 2026-10-24 18:30 |
| `number of TEXT` | the text as a number, so math works on it |
| `empty list` | a list with nothing in it yet |
| `uppercase of TEXT` | the text in capitals |
| `lowercase of TEXT` | the text in small letters |
| `trim of TEXT` | the text without spaces at the ends |
| `paragraphs of TEXT` | a list of the paragraphs (separated by blank lines) |
| `split TEXT by "/"` | a list of the pieces between each separator |
| `replace "a" with "b" in TEXT` | the text with every a changed to b |
| `ask database "SQL" with VALUE, ...` | run a query; each ? in the SQL takes the next value safely. Gives a list of records |
| `COLUMN of RECORD` | one column of a record, like name of recipe. Any column name works |
| `read json TEXT` | turn JSON text into lists and records, so 'COLUMN of RECORD' and 'for each' work on it. Text from a visitor or another site stays marked as theirs |
| `json of VALUE` | a list, record, text or number written as JSON text, for an API or a script. Answer with it using show raw |
| `fetch "https://..."  /  fetch "https://..." with RECORD` | read an address on the web. A JSON reply becomes lists and records, anything else text; both are marked as another site's. With a record, it is sent as JSON (a POST). A visitor's value may only go after the host. Keys go in the private cl.keys file, never in a page |
| `A is B` | equal. Also: is not, is more than, is less than, is at least, is at most, is between A and B, is empty |
| `A and B` | both must be yes |
| `A or B` | either can be yes |
| `not A` | flip yes and no |
| `set NAME to record  then column: VALUE lines, then end` | a record of your own, like a row you made up. Read a column with 'price of lamp', change one with 'set price of lamp to 15' |

Also: `+ - * / ( )` for math (numbers only), `# comment` to the end of a line, and `the` is silent.

### Words that cannot be names

A name cannot be one of these. Any other word made of letters, digits and `_` is fine, including the words only used before `of` (`count`, `first`, `date`, `email`, ...).

`show` `raw` `code` `set` `add` `if` `otherwise` `end` `for` `repeat` `include` `open` `tell` `go` `remember` `forget` `send` `yes` `no` `query` `path` `try` `dictionary` `form` `remembered` `matches` `contains` `today` `now` `list` `ask` `of` `fetch` `is` `and` `or` `not` `to` `give` `record` `until` `each` `in` `times` `database` `with` `by` `as` `everything` `the` `between` `an` `than` `at` `least` `most` `less` `more` `empty` `page` `text`

### Examples, one per word

```
# [ ]
set name to "Ada"
show "Hello, [name]! You have [count of messages] messages."

# show
show "Hello, [name]!"

# raw
show raw "<b>bold</b>"

# code
show code example of entry

# set
set price to 4.5

# add
add "cherry" to fruits

# if
if age is at least 18
  show "Welcome"
end

# otherwise
if paid
  show "Thanks"
otherwise
  show "Pay first"
end

# end
repeat 3 times
  show "hip"
end

# for
for each fruit in fruits
  show "<li>[fruit]</li>"
end

# repeat
repeat 5 times
  show "★"
end

# include
include "header.html"

# open
open database "site.db"

# tell
tell database "insert into notes (text) values (?)" with note

# go
go to "/thanks"

# remember
remember "user" as name

# forget
forget everything

# send
send email to "owner@example.com"
  subject: "Message from [name]"
  reply to: email
  body: message
end

# yes
set paid to yes

# no
set done to no

# count
show "[count of fruits] fruits"

# first
show first of fruits
show "[first 100 of body]…"

# last
show last of fruits
for each post in last 3 of posts

# item
show item 2 of fruits

# query
set name to query "name"

# path
set slug to last of split path by "/"

# try
set result to try form "code"

# dictionary
for each entry in dictionary
  show "<dt>[word of entry]</dt><dd>[meaning of entry]</dd>"
end

# form
set message to trim of form "message"

# remembered
if remembered "user" is empty
  go to "/login"
end

# hash
tell database "insert into users (name, password) values (?, ?)" with name, hash of form "password"

# matches
if form "password" matches password of user
  remember "user" as name
end

# email
if form "email" is not an email
  set problem to "Please check your email address."
end

# contains
if email contains "@"

# sort
for each name in sort of names

# reverse
for each post in reverse of posts

# date
show "[date of today]"

# year
show "[year of today]"

# month
show "[month of today]"

# day
show "[day of today]"

# today
show "Today is [today]"

# now
tell database "insert into log (at) values (?)" with now

# number
set qty to number of query "qty"

# list
set names to empty list

# uppercase
show "[uppercase of name]"

# lowercase
set slug to lowercase of title

# trim
set name to trim of form "name"

# paragraphs
for each paragraph in paragraphs of body
  show "<p>[paragraph]</p>"
end

# split
set parts to split "2026-10-24" by "-"

# replace
set slug to replace " " with "-" in title

# ask
set posts to ask database "select * from posts where slug = ?" with slug

# of
show "[title of recipe]"

# read
set point to read json "{\"x\": 3, \"y\": 4}"
show "[x of point], [y of point]"

# json
set point to record
  x: 3
  y: 4
end
show raw json of point

# fetch
set posts to fetch "https://api.example.com/posts"
for each post in posts
  show "<li>[title of post]</li>"
end

# is
if colour is "blue"
if age is more than 12
if name is empty

# and
if name is not empty and age is at least 18

# or
if paid or member

# not
if not done

# to
to greet someone
  show "<p>Hello, [someone]!</p>"
end

greet "Ada"

# give
to slug_of heading
  give replace " " with "-" in lowercase of heading
end

show "[slug_of title]"

# record
set lamp to record
  name: "Desk lamp"
  price: 19
end
show "[name of lamp] costs $[price of lamp]"

```

### Words from other languages

Common refuses these and names the word to use instead.

| You might write | Common |
|---|---|
| `print` | show |
| `echo` | show |
| `say` | show |
| `write` | show |
| `display` | show |
| `let` | set |
| `make` | set |
| `var` | set |
| `define` | set |
| `else` | otherwise |
| `elif` | otherwise if |
| `elseif` | otherwise if |
| `then` | nothing: just start the next line |
| `while` | repeat N times, or for each |
| `loop` | repeat N times, or for each |
| `foreach` | for each |
| `length` | count |
| `size` | count |
| `len` | count |
| `push` | add VALUE to LIST |
| `append` | add VALUE to LIST |
| `bigger` | more |
| `greater` | more |
| `larger` | more |
| `smaller` | less |
| `lower` | less |
| `equals` | is |
| `equal` | is |
| `says` | is |
| `eq` | is |
| `isnt` | is not |
| `true` | yes |
| `false` | no |
| `null` | empty |
| `nil` | empty |
| `require` | include |
| `import` | include |
| `get` | query |
| `param` | query |
| `post` | form |
| `redirect` | go to |
| `header` | go to |
| `int` | number of |
| `float` | number of |
| `parse` | number of |
| `upper` | uppercase of |
| `lower` | lowercase of |
| `strip` | trim of |
| `explode` | split ... by |
| `join` | [a] [b] inside text |
| `insert` | tell database |
| `update` | tell database |
| `delete` | tell database |
| `execute` | tell database |
| `session` | remember / remembered |
| `cookie` | remember / remembered |
| `login` | remember "user" as name |
| `logout` | forget everything |
| `password_hash` | hash of |
| `bcrypt` | hash of |
| `md5` | hash of |
| `password_verify` | matches |
| `sorted` | sort of |
| `reversed` | reverse of |
| `substr` | first N of |
| `substring` | first N of |
| `slice` | first N of |
| `mail` | send email |
| `sendmail` | send email |
| `smtp` | send email |
| `includes` | contains |
| `in_array` | contains |
| `strpos` | contains |
| `strftime` | date of |
| `json_decode` | read json |
| `decode` | read json |
| `json_encode` | json of |
| `stringify` | json of |
| `jsonify` | json of |
| `curl` | fetch |
| `wget` | fetch |
| `request` | fetch |
| `download` | fetch |
