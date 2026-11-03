# The Common Dictionary

Common has a small, fixed vocabulary. Every word the language understands is
on this page, with exactly one way to use it. Common does not guess: if a
phrasing is not here, it is an error, and the error will point you to the
closest word that is.

Why so strict? Languages that try to accept many English phrasings (AppleScript,
HyperTalk) end up with hidden rules that nobody can learn. Common takes the
other path: few words, one meaning each, and code that still reads aloud.

Run `cl words` to print this list from the interpreter itself.

---

## Rules

1. One statement per line. No semicolons. A full stop at the end of a line is fine.
2. All Common words are lowercase. `Say` is an error, `show` is correct.
3. Blocks end with `end`. A text block ends at a line that is just `end`; a line like `end of story` inside it is text.
4. Text goes in double quotes: `"like this"`. A value goes inside the text in brackets: `"Hello, [name]!"`. Inside text, `\"` is a quote, `\n` a new line, `\\` a backslash, `\[` a plain bracket. Longer text, like a piece of HTML, can be a block: a line with just `show` (or `set NAME to`), then the text as typed, then a line with just `end`. No quotes needed, and `[values]` still work.
5. The word `the` is silent. `set total to the count of fruits` is the same as `set total to count of fruits`. It is there for reading, not for meaning.
6. Names are made of letters, digits and underscores. A name cannot be a command or a word that stands alone, like `show`, `today` or `yes`. Words that are only ever used before `of`, such as `count`, `year`, `date` or `first`, may also be names: `set year to 2026` and `year of today` live side by side, because the shape tells them apart.
7. Text typed by a visitor is always made safe before it is shown or joined with other text. Put attribute values in quotes, `href="[url]"` or `title='[name]'`: escaping protects a quoted attribute, but in an unquoted one a space ends the value. You never need to escape anything by hand. That covers `query`, `form`, `path`, `remembered` and text read from the database, and it holds through every word that makes new text from them, such as `trim of`, `replace` or a text block. A visitor's value at the start of a link (`href="[url]"`) that begins with `javascript:`, `vbscript:` or `data:` becomes `#unsafe-link`. When text joined with a visitor's value leaves the page as data (a database value after `with`, `remember`, an email, a `go to` address), it goes as plain text, so it is not escaped twice.
8. Code lives inside `<cl> ... </cl>` tags in an HTML page. Everything outside the tags is sent to the browser untouched.
9. To show Common code on a page instead of running it, put it in `<cl-code> ... </cl-code>`. It comes out escaped, with the words of the language wrapped in `<span class="cl-word">` and text, holes and comments in `cl-text`, `cl-hole` and `cl-note`, ready for a stylesheet to color.

---

## Commands

A command is the first word on a line.

| Word | Form | Meaning |
|---|---|---|
| `show` | `show VALUE` | Show a value on the page. Visitor text is made safe. |
| `show raw` | `show raw VALUE` | Show text exactly as it is, with no safety. Only for HTML you wrote yourself. |
| `show` block | `show` on its own line, then text, then `end` | Show several lines of text as typed, with `[values]` filled in. The way to write HTML. |
| `set` block | `set NAME to` on its own line, then text, then `end` | Keep several lines of text in a name. |
| `show code` | `show code TEXT` | Show Common code highlighted, the way `<cl-code>` does, but from a value. |
| `set` | `set NAME to VALUE` | Create or replace a variable. |
| `set` | `set NAME to VALUE, VALUE, ...` | Create a list. |
| `add` | `add VALUE to NAME` | Put a value at the end of a list. |
| `if` | `if CONDITION` | Run the lines below only when the condition is yes. |
| `otherwise if` | `otherwise if CONDITION` | Another condition to try if the one above was no. |
| `otherwise` | `otherwise` | What to run if no condition above was yes. |
| `end` | `end` | Close an `if`, `for each`, `repeat`, `to`, `send email`, record or text block. |
| `for each` | `for each NAME in LIST` | Run the lines below once for every item in the list. |
| `repeat` | `repeat NUMBER times` | Run the lines below that many times. |
| `repeat … until` | `repeat 100 times until done` | The same, but stop as soon as the condition is yes. The number is the ceiling, so a page always finishes. |
| `include` | `include "file.html"` | Insert another page here. It can contain Common and sees your variables. The name is next to the page, like `"parts/menu.html"`; a name starting with `/` starts from the site's top folder, like `"/header.html"`, so pages in subfolders share one header. `..` is never allowed. |
| `send email` | `send email to "you@site.com"`, then `subject:`, `body:`, `reply to:`, `from:` lines, then `end` | Send an email to an address you wrote yourself. See [Email](#email). |

---

## Values

| Word | Form | Meaning |
|---|---|---|
| `"..."` | `"Hello"` | Text. |
| `[...]` | `"Hello, [name]! You have [count of items] items."` | A value placed inside text. Any value works: a name, math, `count of`, `query`. Visitor text is made safe. A list reads `a, b, c`. |
| number | `42`, `4.5`, `-3` | A number. |
| `yes` / `no` | `yes` | The two answers to a condition. |
| name | `price` | The value of a variable. |
| `count of` | `count of LIST` | How many items a list has, or how many letters text has. |
| `first of` | `first of LIST` | The first item. |
| `last of` | `last of LIST` | The last item. |
| `item ... of` | `item 2 of LIST` | One item by position, counting from 1. |
| `query` | `query "name"` | What the visitor typed in the URL after `?name=`. Empty text if missing. |
| `form` | `form "name"` | What the visitor typed in a submitted form (a POST). Empty text if missing. |
| `today` | `today` | The date, like `2026-10-24`. |
| `now` | `now` | The date and time, like `2026-10-24 18:30`. |
| `empty list` | `set names to empty list` | A list with nothing in it yet. Use `add` to fill it. |
| `number of` | `number of query "page"` | Text turned into a number so math works on it. An error if it is not a number. |
| `contains` | `email contains "@"`, `tags contains "new"` | Yes if text has the piece inside it, or a list has the item. |
| `path` | `path` | The part of the URL after the site name, like `/about`. The visitor typed it, so it is made safe like `query`. |
| `try` | `set result to try form "code"` | Run text as Common in a sandbox and give back what it said. Files, the database, cookies, redirects, email and password hashing (`hash of`, `matches`) are off, expressions nest at most 200 deep, loops stop after 20,000 steps (a line run or a turn of a loop), memory is capped at 64 MB, and output is capped. For practice pages. |
| `dictionary` | `for each entry in dictionary` | Every word of the language as records with `word`, `form`, `meaning`, `example` and `command`. The glossary page is built from it. |

---

## Math

| Symbol | Form | Meaning |
|---|---|---|
| `+` | `2 + 3` | Add two numbers. Only numbers: to put a value in text, use brackets. |
| `-` | `10 - 4` | Subtract. |
| `*` | `price * 3` | Multiply. |
| `/` | `total / 2` | Divide. |
| `( )` | `(1 + 2) * 3` | Group. |

---

## Conditions

A condition is a comparison, or several joined with `and`, `or`, `not`.

Visitors only ever send text. When one side is a number, text written as a
number is compared as a number, so `query "age" is more than 12` works with
`age=9`. Comparing a number with other text, or with an empty field, is an
error that says so, instead of a wrong answer. Two pieces of text compare in
A to Z order.

| Words | Form | Meaning |
|---|---|---|
| `is` | `name is "Ada"` | Equal. |
| `is not` | `name is not "Ada"` | Not equal. |
| `is more than` | `age is more than 17` | Greater. |
| `is less than` | `age is less than 18` | Smaller. |
| `is at least` | `age is at least 18` | Greater or equal. |
| `is at most` | `age is at most 17` | Smaller or equal. |
| `is between` | `age is between 13 and 19` | Greater or equal to the first, smaller or equal to the second. |
| `is empty` | `name is empty` | Empty text, empty list, or zero. |
| `is not empty` | `name is not empty` | Has something in it. |
| `is an email` | `form "email" is an email` | Yes if the text is one email address, like `ada@example.com`. `is not an email` is the opposite. |
| `and` | `a is 1 and b is 2` | Both must be yes. |
| `or` | `a is 1 or b is 2` | Either can be yes. |
| `not` | `not done` | Flip yes and no. |

---

## Text

| Word | Form | Meaning |
|---|---|---|
| `uppercase of` | `uppercase of name` | The text in capitals. |
| `lowercase of` | `lowercase of name` | The text in small letters. |
| `trim of` | `trim of form "name"` | The text without spaces at the ends. |
| `split ... by` | `split path by "/"` | A list of the pieces between each separator. Empty pieces are skipped. |
| `replace ... with ... in` | `replace " " with "-" in title` | The text with every match changed. |
| `paragraphs of` | `paragraphs of body` | A list of the paragraphs, which are separated by blank lines. |
| `first N of`, `last N of` | `first 100 of body`, `last 3 of names` | The first or last N letters of text, or items of a list. |
| `sort of` | `sort of names` | The list in order: numbers by size, text A to Z. |
| `reverse of` | `reverse of posts` | The list back to front. |
| `date of` | `date of "2026-10-24"` | The date written out: 24 October 2026. |
| `year of`, `month of`, `day of` | `month of today` | Parts of a date: 2026, October, 24. |

```
set slug to replace " " with "-" in lowercase of trim of title
for each paragraph in paragraphs of post_body
  show "<p>[paragraph]</p>"
end
```

---

## Pages and the Server

| Word | Form | Meaning |
|---|---|---|
| `go to` | `go to "/thanks"` | Send the visitor to another address and stop this page. Use it after handling a form, so a refresh does not resubmit. |

The server also does these without any words:

- **Clean URLs.** `/about` serves `about.html`. `/blog/hello-world` serves `blog.html`, and the page reads the rest with `path`.
- **A custom 404.** If the folder has a `404.html`, it is shown for any missing address, with a real 404 status.
- **Forms.** A form with `method="post"` arrives in `form "field"`. A form with `method="get"` arrives in `query "field"`.

---

## Verbs: your own commands

A verb is a recipe you name once and call like a built-in command. Define it
with `to`, close it with `end`, and call it by name with one value per
argument.

| Word | Form | Meaning |
|---|---|---|
| `to` | `to NAME ARG ARG` … `end` | Define a verb. The names after it are the values it takes. |
| `give` | `give VALUE` | Inside a verb: hand this value back and finish. A verb that gives can be used wherever a value goes. |

```
to greet someone
  show "<p>Hello, [someone]!</p>"
end

to slug_of heading
  give replace " " with "-" in lowercase of trim of heading
end

greet "Ada"
greet query "name"
set slug to slug_of title
show "[slug_of "Hello World"]"
```

A verb can read every variable set before it was called, and its own
arguments. What it sets inside stays inside: the only way out is `give`. Verbs
defined in an included file work on every page that includes it, which is how
a site builds its own vocabulary. A verb may call itself, up to 100 levels.

Errors teach the form: calling with the wrong number of values says "'greet'
takes 1 value, but got 2. It is written: greet someone".

---

## Records you make

A record keeps named values together, like a row you made up. Read a column
with `COLUMN of RECORD`, the same as a database row.

| Word | Form | Meaning |
|---|---|---|
| `record` | `set NAME to record` then `column: VALUE` lines, then `end` | Make a record. |
| `set … of` | `set price of lamp to 15` | Change one column, or add it. |
| `item … of` | `item "price" of lamp` | A column by a name held in text. The same word that picks a list item by number. |

```
set lamp to record
  name: "Desk lamp"
  price: 19
end
show "[name of lamp] costs $[price of lamp]"
set price of lamp to 15
add lamp to products
```

---

## Visitors who sign in

A value you `remember` is kept for that visitor in a signed cookie and comes
back on every later page until they `forget` it, for up to 30 days after it
was last changed. Nobody can forge it: the cookie and its end date are signed
with a secret that Common creates in `cl.secret` on first use, and never
serves: next to the pages under `cl serve`, and outside the web root under
`cl cgi` (see `docs/deploy.md`). `forget everything` signs out this browser; a copied
cookie keeps working until its 30 days are up. To sign everyone out at once,
delete `cl.secret`.

After 10 wrong passwords from one address within 10 minutes, `matches` refuses
that address until the 10 minutes are up, so nobody can keep guessing.

| Word | Form | Meaning |
|---|---|---|
| `remember` | `remember "user" as name` | Keep a value for this visitor. |
| `remembered` | `remembered "user"` | Read it back. Empty text if nothing was remembered. |
| `forget` | `forget "user"` | Drop one value. |
| `forget everything` | `forget everything` | Drop them all: sign out. |
| `hash of` | `hash of form "password"` | A salted one-way scramble, safe to store (PBKDF2-HMAC-SHA256, 200,000 rounds). Hashes made by 0.3.0 and earlier still match. |
| `matches` | `form "password" matches stored` | Yes if the text is the password behind the hash. At most 10 wrong passwords per address every 10 minutes. |

```
set users to ask database "select name, password from users where name = ?" with name
if count of users is 1 and form "password" matches password of first of users
  remember "user" as name
  go to "/admin"
end

if remembered "user" is empty
  go to "/login"
end
```

Remembered values are text. Keep them small: a name or an id, not a whole
record. The cookie lasts 30 days, is not readable by JavaScript, and is not
sent on cross-site POSTs.

---

## Email

`send email` sends a plain-text email. It is the one command that reaches
outside the site, so it has rules that the other commands do not need:

```
if form "email" is not an email
  set problem to "Please check your email address."
otherwise
  send email to "owner@example.com"
    subject: "Message from [form "name"]"
    reply to: form "email"
    from: "website@example.com"
    body: form "message"
  end
  go to "/contact?thanks=yes"
end
```

| Line | Meaning |
|---|---|
| `send email to ADDRESS` | Who gets it. One address, written by you. A visitor's value is refused, so nobody can use your form to send email to anybody else. |
| `subject: VALUE` | Required. Visitor text is fine. Line breaks become spaces, so nobody can add a header such as `Bcc:`. |
| `body: VALUE` | Required. Plain text, never HTML. Visitor text arrives exactly as typed. |
| `reply to: VALUE` | Optional. Usually the visitor's address, so pressing Reply in your mail program answers them. If it is not one email address it is left off. |
| `from: ADDRESS` | Required, written by you: an address on your own domain, so the email is not marked as spam. The `CL_MAIL_FROM` environment variable can stand in for it. |

Where it goes:

- **`cl serve`, and `cl page.html`** save each email as a file in an `outbox/`
  folder next to the site instead of sending it, and print which file. Nothing
  leaves your machine while you build, and the server never sends `outbox/`.
- **`cl cgi`**, behind a real web server, hands the email to the server's
  `sendmail` program, which nearly every Linux and cPanel host has. Set
  `CL_SENDMAIL` if it is not at `/usr/sbin/sendmail`.
- `CL_MAIL=outbox` or `CL_MAIL=sendmail` picks one or the other in any mode.

Limits: at most 3 emails each time a page runs, and 20 an hour for the whole
site (`CL_MAIL_PER_HOUR` changes it). Sends are counted in a private `mail.log`
next to the site, which the server never sends. `send email` is not allowed
inside `try`.

Text you join before the block, like `set note to "[form "message"]"`, is
fine too: the HTML safety that visitor text gets on a page is taken off again
in the email, because an email body is plain text.

---

## Databases

Common can read a SQLite database file kept next to the page. This needs SQLite
on the machine when Common is built; `make` turns it on automatically when it
finds it, and `cl words` lists `ask database` when it is there.

| Word | Form | Meaning |
|---|---|---|
| `open database` | `open database "shop.db"` | Open a SQLite file next to the page, or a Postgres server with `open database "postgres://user:password@host/dbname"`. Do this once, before asking. The other two words work the same on both. |
| `ask database` | `ask database "select name, price from products where id = ?" with chosen` | Run a query. Each `?` in the SQL takes the next value after `with`, sent separately so nothing a visitor types can change the SQL. Gives a list of records, one per row. |
| `tell database` | `tell database "insert into notes (text, posted) values (?, ?)" with note, now` | Run SQL that changes the database: insert, update, delete, create. Same `with` rules. |
| `... of` | `name of product` | One column of a record. Any column name works, even one that is a Common word. Use `first of`, `item 2 of` or `for each` to pick a record from the list first. |

```
open database "shop.db"
set products to ask database "select name, price from products order by name"
show "[count of products] products:"
for each product in products
  show "<li>[name of product] costs $[price of product]</li>"
end
```

Text from the database is made safe before it is shown, the same as visitor
text.

The SQL inside the quotes is SQL, not Common. This is the one place where a
second language lives inside the first, the same as in PHP or Python, and it
is deliberate: wrapping SQL in English works for simple queries and breaks at
the first join. Common keeps the SQL honest and makes the dangerous parts,
parameters and escaping, automatic.

---

## JSON and Other Sites

Many services speak JSON: a content system like Drupal or WordPress, a shop, a
weather or payment service. Two words read and write it, and `fetch` reads an
address on the web.

| Word | Form | Meaning |
|---|---|---|
| `read json` | `read json TEXT` | Turn JSON text into values: an object becomes a record, an array a list, `true` and `false` become `yes` and `no`, and `null` becomes empty text. Text from a visitor or another site stays marked as theirs, so it is made safe when shown. |
| `json of` | `json of VALUE` | A list, record, text or number written as JSON text. `<`, `>` and `&` are written as `<`, `>` and `&`, so it is safe inside a `<script>` too. To answer with JSON, use `show raw json of VALUE`. |
| `fetch` | `fetch "https://..."` | Read an address on the web. A reply that says it is JSON becomes records and lists; anything else is text. |
| `fetch … with` | `fetch "https://..." with RECORD` | Send a record or a list as JSON (a POST) and read the reply. |

```
set posts to fetch "https://jsonplaceholder.typicode.com/posts"
for each post in first 3 of posts
  show "<li><a href=\"/post?id=[id of post]\">[title of post]</a></li>"
end
```

Read a column with `COLUMN of RECORD`, the same as a database row, and go
deeper one `of` at a time. A Drupal site with JSON:API switched on lists its
articles at `/jsonapi/node/article`; each one is in `data of` the reply, with
its title in `title of attributes of article`. A column whose name is not one
word, like `field-image`, is read with `item "field-image" of RECORD`.

Sending works the same way:

```
set note to record
  title: "Hello"
  body: "Sent from Common"
end
set reply to fetch "https://jsonplaceholder.typicode.com/posts" with note
show "Saved as number [id of reply]"
```

The rules that keep it safe:

- **The page chooses the site.** The address is written in quotes right
  after `fetch`. A visitor's value may only go after the host, like
  `fetch "https://api.example.com/posts/[query "id"]"`, and it is encoded
  there, so `../admin` or `1&delete=yes` arrives as plain letters. A visitor's
  value before the path, or a whole address from a visitor, is refused.
- **Keys stay out of pages.** A service that wants a key gets it from
  `cl.keys`, a file in the site's private folder (where `cl.secret` is), one
  line per host: the host, then the header to send. The key is only ever sent
  to that host, and the server never sends the file.

  ```text
  api.example.com  Authorization: Bearer abc123
  ```

- **Another site's text is treated like a visitor's.** It is escaped when
  shown. HTML from a site you run yourself, like a Drupal article's body, can
  be shown with `show raw body of article`; never do that for a site you do
  not control.
- **Limits:** 20 fetches each time a page runs, 10 seconds and 10 MB each. A
  redirect is followed up to 3 times, but not when a key is sent. `fetch` is
  not allowed inside `try`.

A reply that is not a success is an error that says what it means, like
`answered 404: there is nothing at that address`. Each fetch happens while the
visitor waits, so a slow service makes a slow page.

`fetch` needs libcurl when Common is built; `make` turns it on when it finds
it, and `cl words` lists `fetch` when it is there. `read json` and `json of`
are always there.

JSON written in a page is text in quotes, so its quotes need a `\`, and its
square brackets too, or they would be holes: `read json "\[1, 2, 3\]"`.

---

## Comments

| Symbol | Form | Meaning |
|---|---|---|
| `#` | `# a note to yourself` | The rest of the line is ignored. |

---

## Not in the Dictionary

These are common guesses. Common rejects them and tells you the word to use.

| You might write | Write this instead |
|---|---|
| `print "x"`, `echo "x"`, `say "x"` | `show "x"` |
| `let x be 5`, `x = 5`, `make x 5` | `set x to 5` |
| `is bigger than`, `is greater than`, `>` | `is more than` |
| `is smaller than`, `<` | `is less than` |
| `equals`, `==` | `is` |
| `else`, `elif`, `else if` | `otherwise`, `otherwise if` |
| `loop`, `while`, `for x in` | `for each x in`, `repeat 3 times` |
| `length of`, `size of` | `count of` |
| `push`, `append` | `add VALUE to LIST` |
| `redirect`, `header("Location")` | `go to "/page"` |
| `mail()`, `sendmail`, `smtp` | `send email to "you@site.com"` |
| `int()`, `parseInt`, `float()` | `number of TEXT` |
| `upper`, `lower`, `strip` | `uppercase of`, `lowercase of`, `trim of` |
| `explode`, `.split()` | `split TEXT by ","` |
| `$_POST`, `request.form` | `form "field"` |
| `JSON.parse`, `json_decode` | `read json TEXT` |
| `JSON.stringify`, `json_encode` | `json of VALUE` |
| `curl`, `wget`, `request` | `fetch "https://..."` |
| `"Hello " + name`, `"Hello " . name`, `"Hello $name"`, `"Hello {name}"` | `"Hello [name]"` |
| `Show`, `SET` | lowercase: `show`, `set` |

---

## Every Reserved Word

These are the words of the language. The ones in bold cannot be variable names; the others only mean their Common word when written before `of` (or, for `split` and `replace`, before a value):

**`add` `and` `as` `ask` `at` `by` `code` `contains` `database` `dictionary`
`each` `empty` `end` `everything` `for` `forget` `form` `go` `if` `in`
`include` `is` `least` `less` `list` `matches` `more` `most` `no` `not`
`now` `of` `open` `or` `otherwise` `path` `query` `raw` `remember`
`remembered` `repeat` `show` `set` `tell` `than` `the` `times` `to` `today` `try`
`with` `yes` `between` `give` `record` `until` `send` `an` `fetch`**

Usable as names too: `count` `date` `day` `email` `first` `hash` `item` `last`
`json` `lowercase` `month` `number` `paragraphs` `read` `replace` `reverse` `sort` `split`
`trim` `uppercase` `year`

Two forms are not shaped `word of x`: `split TEXT by SEP` and `replace A with B in TEXT`. They are kept that way because the English reads better, and both are rare in a first week.

Reserved for future use: `page` `text`.
