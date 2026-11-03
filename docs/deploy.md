# Putting a Common site on the internet

The built-in server (`cl serve`) is for development. It is fine for a personal
site behind a reverse proxy, and it forks one process per request so a slow or
broken page cannot take it down, but it speaks HTTP/1.0 and knows nothing about
TLS. For hosting, run Common the way PHP was first run: as a CGI program behind
a real web server.

## How CGI mode works

`cl cgi` reads the standard CGI variables the web server sets, runs the page
named by `SCRIPT_FILENAME`, and prints the response:

- `REQUEST_METHOD`, `QUERY_STRING` and the request body feed `query` and `form`.
- `PATH_INFO` (or `SCRIPT_NAME`) is what the page sees as `path`.
- `go to` becomes a `Status: 303` with a `Location` header.

## cPanel shared hosting (HawkHost and similar)

Shared cPanel plans (64-bit Linux with CGI allowed) are all Common needs. Build
a static `cl` with SQLite inside it, so the server needs no libraries. On any
Linux x86-64 machine, or in a container (`docker run --rm -v "$PWD":/w -w /w gcc:13 ...`):

```sh
gcc -O2 -std=c11 -DCL_SQLITE -static -o cl src/cl.c src/mod_sqlite.c -lsqlite3 -lpthread -ldl -lm
./cl version     # check it is the version you expect before uploading
```

Rebuild and upload again after every release: a binary does not update itself.
That build is SQLite only. A static Postgres build is not worth the trouble on
shared hosting; use a normal `make` on a server where you control the packages.

1. **Upload the interpreter.** In cPanel's File Manager (or over SFTP), open
   `public_html/cgi-bin/` (create it if needed), upload `cl`, and set its
   permissions to `755`.
2. **Upload the site.** Put your pages in `public_html/`.
3. **Add the `.htaccess`** below to `public_html/`, or append its lines to the
   one you have. It works on Apache and on LiteSpeed, which many shared hosts
   run: it uses only rewrite rules, because LiteSpeed ignores Apache's
   `Action` directive.
4. **Open the site.** The first request creates `cl.secret`, the key that signs
   sign-in cookies, outside the web root: in `~/.common/public_html/`, readable
   only by you. (If that folder cannot be made, it goes next to the pages, and
   the `.htaccess` keeps it private. `SetEnv CL_PRIVATE_DIR /some/folder`
   chooses another place.) Wrong-password counts (`login.log`), `cl.keys` (the
   keys `fetch` sends to other sites) and, with `CL_MAIL=outbox`, saved emails
   go there too. Then lock down the data:
   `chmod 600 site.db`. Shared hosts run CGI
   as your own account (suEXEC), so `600` keeps other customers out.

```apache
# Common (CL) on Apache or LiteSpeed. Uses only rewrite rules (no Action
# directive, which LiteSpeed does not support). Needs mod_rewrite and a
# cgi-bin folder with the cl program in it; cPanel hosts have both.
RewriteEngine on

# Never send databases, seeds, the session secret, API keys or saved email.
RewriteRule \.(db|sqlite|sqlite3|sql|secret|keys|eml|log)$ - [F,NC]
RewriteRule \.db-(journal|wal|shm)$ - [F,NC]
RewriteRule (^|/)outbox(/|$) - [F,NC]

# Requests that already go to cl pass straight through.
RewriteRule ^cgi-bin/ - [L]

# Pages run through cl: the home page, and any .html address.
RewriteRule ^$ /cgi-bin/cl/index.html [L,QSA]
RewriteRule ^(.+\.html)$ /cgi-bin/cl/$1 [L,QSA,NC]

# Clean addresses: /about runs about.html, /blog/hello-world runs blog.html.
RewriteCond %{REQUEST_FILENAME} !-f
RewriteCond %{REQUEST_FILENAME} !-d
RewriteRule ^([^./]+)(/.*)?$ /cgi-bin/cl/$1.html [L,QSA]
```

Over SSH instead: `mkdir -p ~/public_html/cgi-bin && cp cl ~/public_html/cgi-bin/ && chmod 755 ~/public_html/cgi-bin/cl`.
If the host has gcc and SQLite headers, you can also build there: `gcc -O2 -std=c11 -DCL_SQLITE -o cgi-bin/cl src/cl.c src/mod_sqlite.c -lsqlite3`.

**If it does not work**

- **Raw `<cl>` text on the page.** The rewrite rules are not applied. Check
  that `.htaccess` is in `public_html/`, that it starts with `RewriteEngine on`,
  and that `cl` is at `public_html/cgi-bin/cl`. An older setup that used
  `Action common /cgi-bin/cl` does not work on LiteSpeed: use the rules above.
- **Pages in a subfolder** (`shop/index.html`) are reached as `/shop/index.html`.
  The clean-address rule covers pages in the top folder only.
- **500 error.** Usually permissions: `cl` must be `755`, and the folders above
  it must not be group-writable (`755`, not `775`). cPanel's Error Log has the reason.
- **Database is locked / cannot open.** The folder must be writable by your
  account, and the `.db` file `600` or `644`.
- **Sign-in forgets everyone.** `cl.secret` could not be written: make
  `~/.common/` writable, or create `~/.common/public_html/cl.secret` yourself
  with 64 random hex characters.
- **"Too many wrong passwords from this address".** Someone (maybe you) typed
  10 wrong passwords within 10 minutes. Wait, or delete `login.log` in the
  private folder.

## Apache or LiteSpeed (shared hosting, .htaccess)

Copy the `cl` binary into `cgi-bin/` and use the `.htaccess` from the cPanel
section above. It is the same file on Apache and on LiteSpeed: rewrite rules
send every page to `/cgi-bin/cl/page.html`, `cl` runs it, and the visitor's
address (`/blog/hello-world`) still reaches the page as `path`.

## nginx with fcgiwrap

```
location ~ \.html$ {
    include fastcgi_params;
    fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
    fastcgi_pass unix:/run/fcgiwrap.socket;
}
location / {
    try_files $uri $uri.html /index.html;
}
```

and point fcgiwrap at `cl cgi`.

## Caddy, reverse proxy to cl serve

The simplest option for a small personal site on your own machine:

```
example.com {
    reverse_proxy localhost:8000
}
```

with `cl serve 8000 /var/www/site` kept running by systemd. Caddy adds TLS.

Inside a container the dev server must listen on every interface, not just the
machine itself: `cl serve 0.0.0.0:8000 /site`.

## Postgres instead of SQLite

When Common is built on a machine with the Postgres client library (`brew
install libpq` on macOS, `apt install libpq-dev` on Debian and Ubuntu), the
same three words also talk to a Postgres server:

```
open database "postgres://app:secret@localhost/site"
set posts to ask database "select * from posts where slug = ?" with slug
tell database "insert into notes (body) values (?)" with note
```

Write `?` for each value as with SQLite; Common turns them into `$1`, `$2`
for Postgres.

Numbers, booleans and nulls come back as numbers, yes/no and
empty text. Keep the address line in an included `.html` file: Common runs
pages instead of sending them, so the password never reaches a browser.

## Email from forms

`send email` (see the dictionary) works with no setup on most hosts. Under
`cl cgi` it hands each email to the server's `sendmail` program, which cPanel
and nearly every Linux host provide.

- **The from address.** Write one on your own domain, like
  `from: "website@yourdomain.com"`. Mail from a domain you do not own is
  usually marked as spam. On cPanel, the domain's email settings should show
  SPF and DKIM as valid.
- **sendmail somewhere else?** Set `CL_SENDMAIL` to its path, with
  `SetEnv CL_SENDMAIL /path/to/sendmail` in `.htaccess` on Apache.
- **Testing on the live site without sending:** `SetEnv CL_MAIL outbox` (Apache) saves
  emails in `outbox/` as files instead, like the development server does.
- **Keep the files private.** `outbox/` and `mail.log` sit next to the pages.
  The built-in server never sends them. Behind Apache or LiteSpeed, the
  `.htaccess` above refuses `.eml`, `.log` and `outbox/`. On nginx, add
  `location ~ (/outbox/|\.(eml|log)$) { deny all; }`.
- **The speed limit** is 20 emails an hour for the site, counted in
  `mail.log`. `SetEnv CL_MAIL_PER_HOUR 50` raises it.

Sending through a mail service such as Postmark or Mailgun over SMTP, without
sendmail, is not built in yet.

## Things to know

- The built-in server never sends `.db`, `.sqlite` or `.sql` files, so the
  database can sit next to the pages. Behind Apache or nginx it is the web
  server that sends static files, so add the same rule there, or keep the
  database in a folder above the site and `open database "../data/site.db"`.
  Apache and LiteSpeed: the `.htaccess` in the cPanel section does it.
  nginx: `location ~ \.(db|sqlite|sql)$ { deny all; }`.
- The file must be writable by the user the web server runs as, and readable
  by nobody else: `chmod 600 site.db`. On hosts where every site runs as the
  same user, other customers' scripts can read anything that user can read.
- Use a local disk. SQLite's locking is unreliable on NFS and some network
  storage. Common waits up to three seconds for a write lock, which is plenty
  for a small site.
- Back up with `sqlite3 site.db ".backup copy.db"`, not `cp`, so you never
  copy a file mid-write.
- `cl.secret` signs session cookies. Common creates it on first use (outside
  the web root under `cl cgi`), readable only by you, and never serves it. If it
  ever leaks, delete it: everyone is signed out and a new one is made.
- `cl.keys` holds the keys `fetch` sends to other sites, one line per host
  (see "JSON and Other Sites" in the dictionary). You write it; put it in the
  same private folder as `cl.secret` and `chmod 600` it. The built-in server
  and the `.htaccess` above never send it. On nginx, add `keys` to the
  `location` rule that refuses `.db` files.
- `fetch` needs a `cl` built with libcurl, which the static build above leaves
  out (a static libcurl drags in a TLS library and more). Build on the host
  instead, adding `-DCL_CURL src/mod_curl.c -lcurl` to the command. Some shared
  hosts block requests to other sites; ask yours.
- Common escapes output and binds database parameters. It has no rate limiting
  except on email, so a public guestbook will collect spam.
- Common refuses a form posted from another site (the browser's `Origin`
  header names a different host than the request). Behind a reverse proxy
  that rewrites `Host`, pass the visitor's host on as `X-Forwarded-Host`
  (Caddy and most proxies do), or forms on your site will be refused too.
- Every response says `X-Frame-Options: SAMEORIGIN`, so your pages cannot be
  shown inside a frame on another site.
- Set the server's clock and timezone. `today` and `now` use the machine's
  local time.
