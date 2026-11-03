#!/bin/sh
# Common test suite. Run from the repo root with: make test
set -e
cd "$(dirname "$0")/.."
fail() { echo "FAIL: $1"; exit 1; }
# Wait until a test server answers (sanitizer builds start slowly), up to 10 seconds
wait_for() { i=0; until curl -s -o /dev/null "http://127.0.0.1:$1/" || [ $i -ge 100 ]; do sleep 0.1; i=$((i + 1)); done; }
# Besides cl, the tests use a few commands. Say up front which one is missing, instead of
# stopping half-way with only "not found". macOS has them all; a bare Linux box may not.
need() { command -v "$1" >/dev/null 2>&1 || fail "the tests need the '$1' command. Debian/Ubuntu: apt install $2"; }
need curl curl; need python3 python3; need pkill procps
has_db=; ./cl words | grep -q 'ask database' && has_db=yes   # database tests only when this build has SQLite
[ -z "$has_db" ] || need sqlite3 sqlite3   # this cl has SQLite, so its database tests need the sqlite3 command too

# 1. The hello page
out=$(./cl tests/hello.html)
echo "$out" | grep -q 'Hello World!'                      || fail "hello world"
echo "$out" | grep -q 'First line'                        || fail "multi-line block"
echo "$out" | grep -q 'this is a comment'                 && fail "comment leaked into output"
echo "$out" | grep -q '<cl>'                              && fail "cl tag leaked into output"
echo "$out" | grep -q "Common error: Line 24: I do not understand 'shout'" || fail "error with file line number"

# 2. Every language feature and error, compared against known-good output
./cl tests/features.html "name=Ada+Lovelace&x=1&evil=%3Cb%3Ex%3C%2Fb%3E" "name=%3Cb%3EAda&n=21" 2>/dev/null | diff -u tests/features.expected - || fail "features.html output changed"
t=$(mktemp -d)
printf '<cl>show "one"\ngo to "/thanks?who=Ada L"\nshow "never"</cl>\n<p>never sent</p>' > $t/go.html
[ -z "$(./cl $t/go.html 2>/dev/null)" ]                            || fail "go to sends no page body"
./cl $t/go.html 2>&1 >/dev/null | grep -q "goes to /thanks?who=Ada%20L" || fail "go to is reported on the command line"
rm -rf $t


# A small database for the tests that need one, made fresh each time
fixture_db() {
  sqlite3 "$1" "create table recipes (id integer primary key, name text);
insert into recipes (name) values ('Chocolate chip'), ('Oatmeal');
create table posts (id integer primary key, slug text, title text);
insert into posts (slug, title) values ('hello-world', 'Hello, world');
create table guestbook (id integer primary key, name text, message text, posted text);"
}

# 3. Small language checks
t=$(mktemp -d)
printf '<cl>show Hello there</cl>' > $t/q.html
./cl $t/q.html | grep -q 'need to be in double quotes: show &quot;Hello there&quot;' || fail "forgiving hint for unquoted show"
printf '<cl>set greeting to Good morning</cl>' > $t/q2.html
./cl $t/q2.html | grep -q 'set greeting to &quot;Good morning&quot;' || fail "forgiving hint for unquoted set"
printf '<cl>set n to 3.\nshow "[the n]".\nif n is between 1 and 5\nshow "ok"\nend\nshow\n  <b>[n]</b>\nend</cl>' > $t/s.html
[ "$(./cl $t/s.html | tr -d '\n')" = "3ok<b>3</b>" ]                 || fail "silent the, full stops, is between and show blocks"
printf '<cl>to double x\n  give x * 2\nend\nshow "[double 21]"\ndouble 1</cl>' > $t/v.html
[ "$(./cl $t/v.html | tr -d '\n')" = "42" ]                         || fail "a verb used as a value and as a command"
printf '<cl>set total to 1\nshow totl</cl>' > $t/q3.html
./cl $t/q3.html | grep -q "Did you mean 'total'"                   || fail "a typo of a name still gets the name suggestion"
rm -rf $t

# 3c. Database errors (only when this build has SQLite)
if ./cl words | grep -q 'ask database'; then
  t=$(mktemp -d); fixture_db $t/cookies.db
  printf '<cl>set r to ask database "select 1"</cl>' > $t/a.html
  ./cl $t/a.html | grep -q 'no database is open'                 || fail "ask before open"
  printf '<cl>open database "nope.db"</cl>' > $t/b.html
  ./cl $t/b.html | grep -q 'cannot open the database'            || fail "missing database file"
  printf '<cl>open database "cookies.db"\nset r to ask database "select nope from recipes"</cl>' > $t/c.html
  ./cl $t/c.html | grep -q 'did not understand the query'        || fail "bad SQL"
  printf '<cl>open database "cookies.db"\nset r to ask database "select * from recipes where id = ?"</cl>' > $t/d.html
  ./cl $t/d.html | grep -q '1 question mark but 0 values'        || fail "parameter count"
  printf '<cl>open database "cookies.db"\nset r to ask database "select name from recipes"\nshow "[name of r]"</cl>' > $t/e.html
  ./cl $t/e.html | grep -q 'needs one record, but this is a list' || fail "field of a list"
  printf '<cl>open database "cookies.db"\nset r to first of ask database "select name from recipes"\nshow "[price of r]"</cl>' > $t/f.html
  ./cl $t/f.html | grep -q "has no 'price'. It has: name"         || fail "unknown field"
  printf '<cl>open database "cookies.db"\nset r to first of ask database "select name as count from recipes"\nshow "[count of r]"</cl>' > $t/g.html
  [ "$(./cl $t/g.html)" = "Chocolate chip" ]                       || fail "a column named after a Common word still reads"
  printf '<cl>open database "cookies.db"\nset r to first of ask database "select 1 as first, 2 as item, 3 as path"\nshow "[first of r] [item of r] [path of r]"</cl>' > $t/h.html
  [ "$(./cl $t/h.html)" = "1 2 3" ]                                || fail "columns named first, item, path"
else
  echo "(this build has no SQLite, database tests skipped)"
fi

# 3c1. Modules: each database driver is a module behind src/cl_module.h; the words and
# their errors are the core's and must not change. Regression tests for the split.
if [ -n "$has_db" ]; then
  t=$(mktemp -d); fixture_db $t/a.db
  sqlite3 $t/b.db "create table recipes (id integer primary key, name text); insert into recipes (name) values ('From b')"
  ./cl words | grep -q '^Modules in this build:'                                 || fail "cl words lists the modules"
  ./cl words | grep -q '^  sqlite .*SQLite .*open database "site.db"'            || fail "cl words names the SQLite module"
  printf '<cl>open database "a.db"\nopen database "b.db"\nshow "[name of first of ask database "select name from recipes"]"</cl>' > $t/two.html
  [ "$(./cl $t/two.html)" = "From b" ]                                            || fail "opening a second database replaces the first"
  printf '<cl>to connect\n  open database "a.db"\nend\nconnect\nshow "[count of ask database "select * from recipes"]"</cl>' > $t/verb1.html
  [ "$(./cl $t/verb1.html)" = "2" ]                                               || fail "a database a verb opened stays open for the page"
  printf '<cl>open database "a.db"\nto names\n  give ask database "select name from recipes"\nend\nshow "[count of names]"</cl>' > $t/verb2.html
  [ "$(./cl $t/verb2.html)" = "2" ]                                               || fail "a verb asks the database its page opened"
  printf '<cl>open database "a.db"\nto switch\n  open database "b.db"\n  give ask database "select name from recipes"\nend\nset r to switch\nshow "[name of first of r]|[name of first of ask database "select name from recipes"]"</cl>' > $t/verb3.html
  [ "$(./cl $t/verb3.html)" = "From b|From b" ]                                   || fail "a verb that opens another database asks the new one (it used to read the closed handle)"
  printf '<cl>open database "mysql://user@host/db"</cl>' > $t/scheme.html
  ./cl $t/scheme.html | grep -q 'cannot open the database &quot;mysql://user@host/db&quot;: this build of Common opens a file next to the page (open database &quot;site.db&quot;)' || fail "an address no module claims says what this build opens"
  rm -rf $t
fi
# The core builds and runs with no module linked (plain cc src/cl.c, as the docs say);
# the database words are then off, with an error that says what to install.
if command -v cc >/dev/null 2>&1; then
  t=$(mktemp -d)
  cc -std=c11 -o $t/cl-bare src/cl.c 2>$t/cc.log || { cat $t/cc.log; fail "the core builds with no module"; }
  $t/cl-bare words | grep -q 'ask database'                                        && fail "a build without modules must not list the database words"
  $t/cl-bare words | grep -q 'database words are off'                             || fail "a build without modules says the database words are off"
  $t/cl-bare words | grep -q 'Modules in this build'                              && fail "a build without modules lists none"
  printf '<cl>open database "x.db"</cl>' > $t/o.html
  $t/cl-bare $t/o.html | grep -q 'no database support, so &quot;x.db&quot; cannot be opened. Install SQLite' || fail "open database without a module says what to install"
  printf '<cl>set r to ask database "select 1"</cl>' > $t/a.html
  $t/cl-bare $t/a.html | grep -q 'no database support. Install SQLite'            || fail "ask database without a module says what to install"
  printf '<cl>set x to 2\nshow "[x * 21]"</cl>' > $t/s.html
  [ "$($t/cl-bare $t/s.html)" = "42" ]                                            || fail "the core runs pages with no module"
  if [ -n "$has_db" ]; then
    cc -std=c11 -DCL_SQLITE -o $t/cl-sqlite src/cl.c src/mod_sqlite.c -lsqlite3 2>$t/cc.log || { cat $t/cc.log; fail "the core builds with only the SQLite module"; }
    printf '<cl>open database "postgres://user:pw@host/db"</cl>' > $t/pg.html
    $t/cl-sqlite $t/pg.html | grep -q 'no Postgres support. Install libpq'        || fail "a build without the Postgres module says what to install"
    $t/cl-sqlite words | grep -q '^  postgres '                                    && fail "a build without the Postgres module must not list it"
    $t/cl-sqlite words | grep -q '^  sqlite '                                      || fail "a build with only the SQLite module lists it"
  fi
  rm -rf $t
else
  echo "(no cc: the build of the core without modules is not tested)"
fi

t=$(mktemp -d); command -v sqlite3 >/dev/null && fixture_db $t/site.db
# 3a9. comparing a list with one value is a teaching error, not a quiet no
printf '<cl>set status to "Working", "At Desk"\nif status is "At Desk"\n  show "y"\nend</cl>' > $t/_t.html
./cl $t/_t.html | grep -q "status is a list of 2 items, so it cannot be one value. To ask whether it holds &quot;At Desk&quot;, write: status contains &quot;At Desk&quot;" || fail "list is text hints at contains"
printf '<cl>set a to 1, 2\nset b to 1, 2\nif a is b\n  show "same"\nend\nif a is not 3\n  show "nope"\nend</cl>' > $t/_t.html
./cl $t/_t.html | grep -q 'same' || fail "list is list still compares"
./cl $t/_t.html | grep -q 'a is a list of 2 items, so it cannot be one number. To ask whether it holds 3, write: not a contains 3' || fail "is not hints at not LIST contains"

# 3a7. visitor values may be shown, never run: SQL, file names, database names, off-site redirects
printf '<cl>include query "p"</cl>' > $t/_t.html
./cl $t/_t.html "p=../../../../../../../../etc/passwd" | grep -q "cannot take a name that came from a visitor" || fail "include refuses a visitor's file name"
printf '<cl>include "../header.html"</cl>' > $t/_t.html
./cl $t/_t.html | grep -q "never a path with .." || fail "include refuses .."
if [ -n "$has_db" ]; then
  printf '<cl>open database "site.db"\nset n to query "n"\nset r to ask database "select * from posts where slug = %s[n]%s"</cl>' "'" "'" > $t/_t.html
  ./cl $t/_t.html "n=x' or '1'='1" | grep -q "Put the value after 'with' and write ? in its place" || fail "ask refuses SQL with a visitor's value inside"
  printf '<cl>open database "site.db"\nset n to query "n"\ntell database "delete from posts where slug = %s[n]%s"</cl>' "'" "'" > $t/_t.html
  ./cl $t/_t.html "n=x" | grep -q "Put the value after 'with' and write ? in its place" || fail "tell refuses SQL with a visitor's value inside"
  printf '<cl>open database query "d"</cl>' > $t/_t.html
  ./cl $t/_t.html "d=site.db" | grep -q "cannot take a name that came from a visitor" || fail "open database refuses a visitor's name"
fi
printf '<cl>go to query "u"</cl>' > $t/_t.html
./cl $t/_t.html "u=https://evil.example/" | grep -q "may only point to a page on this site" || fail "go to refuses an off-site visitor address"
./cl $t/_t.html "u=//evil.example/" | grep -q "may only point to a page on this site"      || fail "go to refuses a protocol-relative address"
./cl $t/_t.html "u=/blog" 2>&1 >/dev/null | grep -q 'goes to /blog'                        || fail "go to still follows a visitor's path on this site"
printf '<cl>set msg to "Hi [query "n"]"\nshow msg\nshow "<b>[msg]</b>"</cl>' > $t/_t.html
[ "$(./cl $t/_t.html 'n=<i>x</i>' | grep -c 'Hi &lt;i&gt;x&lt;/i&gt;')" = 2 ] || fail "filled text is escaped exactly once"

# 3b0. a stray word at the end of a line names the Common word when there is one
printf '<cl>set m to "x"\nif m says "x"\n  show "y"\nend</cl>' > $t/_t.html
./cl $t/_t.html | grep -q "unexpected 'says'. In Common that is written 'is'" || fail "says hints at is"

# 3b1. a comparison after "with VALUE" belongs to the whole ask, not the parameter
if [ -n "$has_db" ]; then
  printf '<cl>open database "site.db"\nset slug to "hello-world"\nif count of ask database "select id from posts where slug = ?" with slug is more than 0\n  show "found"\nend</cl>' > $t/_t.html
  ./cl $t/_t.html | grep -q '^found' || fail "with VALUE is more than 0 compares the count"
fi

# 3b2. the dev server takes host:port and rejects nonsense
./cl serve nope 2>&1 | grep -q 'not a port number' || fail "serve rejects a bad port"
rm -rf $t

# 3c2. Postgres, only when this build has it and CL_PG_URL points at a test server
if ./cl words | grep -q 'postgres://' && [ -n "$CL_PG_URL" ]; then
  t=$(mktemp -d)
  printf '<cl>open database "%s"\nset notes to ask database "select id, body, stars, done, posted from notes order by id"\nshow "[count of notes] notes"\nfor each note in notes\n  show "[id of note]|[body of note]|[stars of note]|[done of note]|[posted of note]"\nend\nshow "[stars of first of notes + 1]"\nset hits to ask database "select body from notes where stars = ?" with 5\nshow "[body of first of hits]"\ntell database "insert into notes (body, stars) values (?, ?)" with "from Common", 1\nshow "[count of ask database "select id from notes"]"\ntell database "delete from notes where body = ?" with "from Common"\nshow "[count of ask database "select id from notes where body = ?" with "x or 1=1"]"</cl>' "$CL_PG_URL" > $t/pg.html
  out=$(./cl $t/pg.html)
  echo "$out" | grep -q 'Common error'                             && { echo "$out" | grep -o 'Common error[^<]*'; fail "postgres page has an error"; }
  echo "$out" | grep -q '^2 notes'                                 || fail "postgres ask returns rows"
  echo "$out" | grep -q '1|first note|3|yes|2026-10-25'            || fail "postgres column types: number, text, boolean, date"
  echo "$out" | grep -q 'second &lt;b&gt;note&lt;/b&gt;'           || fail "postgres text is made safe"
  echo "$out" | grep -q '^4$'                                      || fail "postgres numbers are numbers"
  echo "$out" | grep -q '^3$'                                      || fail "postgres tell inserts a row"
  echo "$out" | grep -q '^0$'                                      || fail "postgres parameters are bound, not pasted"
  printf '<cl>open database "%s"\nset r to ask database "select nope from notes"</cl>' "$CL_PG_URL" > $t/pgbad.html
  ./cl $t/pgbad.html | grep -q 'did not understand the query'      || fail "postgres bad SQL"
  printf '<cl>open database "postgres://nobody:wrong@127.0.0.1:1/none"</cl>' > $t/pgdown.html
  ./cl $t/pgdown.html | grep -q 'cannot reach the Postgres database' || fail "postgres unreachable server"
  rm -rf $t
else
  echo "(no Postgres test: build without libpq or CL_PG_URL not set)"
fi

# 3d. CGI mode
t=$(mktemp -d)
printf '<cl>show "q=[query "q"] path=[path] form=[form "f"]"</cl>' > $t/p.html
SCRIPT_FILENAME=$t/p.html REQUEST_METHOD=GET QUERY_STRING='q=1' PATH_INFO=/p ./cl cgi | grep -q 'q=1 path=/p form=' || fail "cgi GET"
printf 'f=hi' | SCRIPT_FILENAME=$t/p.html REQUEST_METHOD=POST CONTENT_LENGTH=4 ./cl cgi | grep -q 'form=hi' || fail "cgi POST body"
printf '<cl>go to "/x"</cl>' > $t/r.html
SCRIPT_FILENAME=$t/r.html ./cl cgi | grep -q 'Status: 303' || fail "cgi redirect"
SCRIPT_FILENAME=$t/nope.html ./cl cgi | grep -q 'Status: 404' || fail "cgi missing file"
# The page and the visitor's address, however the web server names them
printf '<cl>show "page=[path] q=[query "q"]"</cl>' > $t/blog.html
printf '<cl>show "own 404 for [path]"</cl>' > $t/404.html
SCRIPT_FILENAME=/srv/cgi-bin/cl PATH_INFO=/blog.html DOCUMENT_ROOT=$t REQUEST_URI='/blog/hello%20world?q=1' QUERY_STRING=q=1 ./cl cgi | grep -q '^page=/blog/hello world q=1' || fail "cgi behind a rewrite (Apache or LiteSpeed): the page from PATH_INFO, the address from REQUEST_URI"
SCRIPT_FILENAME=/srv/cgi-bin/cl PATH_TRANSLATED=$t/blog.html REQUEST_URI=/blog ./cl cgi | grep -q '^page=/blog ' || fail "cgi with Apache's Action: the page from PATH_TRANSLATED"
SCRIPT_FILENAME=$t/blog.html REQUEST_URI=/blog ./cl cgi | grep -q '^page=/blog '                || fail "cgi with nginx and fcgiwrap: the page from SCRIPT_FILENAME"
c=$(SCRIPT_FILENAME=/srv/cgi-bin/cl PATH_INFO=/nope.html DOCUMENT_ROOT=$t REQUEST_URI=/nope ./cl cgi)
echo "$c" | grep -q '^Status: 404 Not Found'                        || fail "cgi: a missing page is a 404"
echo "$c" | grep -q '^own 404 for /nope'                            || fail "cgi: a missing page shows the site's own 404.html"
rm -rf $t

t=$(mktemp -d); printf '<cl-code>show "x"' > $t/u.html
./cl $t/u.html | grep -q 'cl-code&gt; block is never closed'       || fail "unclosed cl-code"
rm -rf $t

t=$(mktemp -d)
# 3d0. Every glossary example uses real Common words (names it never set are fine: examples are fragments)
printf '<cl>\nfor each entry in dictionary\n  if try example of entry contains "is not a Common word"\n    show "stale: [word of entry]"\n  end\nend\n</cl>' > $t/_g.html
./cl $t/_g.html | grep 'stale:'                            && fail "a dictionary example uses a word Common does not have"
rm -rf $t

# 3d2. Sessions: remember / remembered / forget through the server, with a cookie jar
port=$((21000 + $$ % 10000)); t=$(mktemp -d); jar=$t/jar
printf '<cl>\nif form "name" is not empty\n  remember "user" as form "name"\n  go to "/"\nend\nif query "bye" is "yes"\n  forget everything\n  go to "/"\nend\nshow "user=[remembered "user"]"\n</cl>' > $t/index.html
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
[ "$(curl -s -c $jar -b $jar http://localhost:$port/)" = "user=" ]                         || fail "session starts empty"
curl -s -i -c $jar -b $jar -X POST -d 'name=Ada' http://localhost:$port/ | grep -q '^Set-Cookie: cl_session=' || fail "remember sets a cookie"
[ "$(curl -s -c $jar -b $jar http://localhost:$port/)" = "user=Ada" ]                      || fail "remembered reads the cookie back"
grep -q HttpOnly $jar || curl -s -i -c $jar -b $jar -X POST -d 'name=Ada' http://localhost:$port/ | grep -q 'HttpOnly; SameSite=Lax' || fail "cookie is HttpOnly and SameSite"
cookie=$(grep cl_session $jar | awk '{print $7}')
[ "$(curl -s -b "cl_session=$(printf '%s' "$cookie" | sed 's/^\(.\{4\}\)[0-9a-f]/\1z/')" http://localhost:$port/)" = "user=" ] || fail "a tampered cookie is ignored"
curl -s -c $jar -b $jar "http://localhost:$port/?bye=yes" >/dev/null
[ "$(curl -s -c $jar -b $jar http://localhost:$port/)" = "user=" ]                         || fail "forget everything signs out"
[ "$(curl -s -o /dev/null -w '%{http_code}' http://localhost:$port/cl.secret)" = 403 ]    || fail "server must not serve cl.secret"
[ -f $t/cl.secret ]                                                                        || fail "cl.secret is created in the site folder"
pkill -f "cl serve $port" ; rm -rf $t

# 3e. The server never serves database files, and waits for a write lock
port=$((20000 + $$ % 10000)); t=$(mktemp -d)
printf '<cl>show "page"</cl>' > $t/index.html; printf 'secret' > $t/site.db; printf 'secret' > $t/seed.sql; printf 'secret' > $t/data.sqlite
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
[ "$(curl -s -o /dev/null -w '%{http_code}' http://localhost:$port/site.db)" = 403 ]      || fail "server must not serve .db files"
[ "$(curl -s -o /dev/null -w '%{http_code}' http://localhost:$port/seed.sql)" = 403 ]     || fail "server must not serve .sql files"
[ "$(curl -s -o /dev/null -w '%{http_code}' http://localhost:$port/data.sqlite)" = 403 ]  || fail "server must not serve .sqlite files"
[ "$(curl -s http://localhost:$port/)" = "page" ]                                         || fail "server still serves pages"
printf 'secret' > $t/cl.secret
for u in cl.SECRET SITE.DB site.db. Seed.Sql; do
  [ -e "$t/$u" ] || continue                                    # only on file systems that ignore case
  [ "$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$port/$u")" = 403 ]        || fail "server must not serve $u (same file as a private one)"
done
python3 -I -c "
import socket,sys; s=socket.create_connection(('localhost',int(sys.argv[1])))
s.sendall(b'POST / HTTP/1.0\\r\\nContent-Length: 900000\\r\\n\\r\\nname=a'); s.shutdown(socket.SHUT_WR)
sys.exit(0 if s.recv(64).startswith(b'HTTP/1.0 400') else 1)" $port                     || fail "a body shorter than Content-Length is a bad request"
pkill -f "cl serve $port" ; rm -rf $t
if ./cl words | grep -q 'ask database'; then
  t=$(mktemp -d); fixture_db $t/site.db
  printf '<cl>open database "site.db"\ntell database "insert into guestbook (name, message, posted) values (?, ?, ?)" with "Load", "test", now\nshow "ok"</cl>' > $t/w.html
  for i in 1 2 3 4 5 6 7 8; do ./cl $t/w.html > $t/out.$i & done; wait
  cat $t/out.* | grep -q 'Common error'                           && fail "concurrent writes should wait for the lock, not fail"
  [ "$(sqlite3 $t/site.db "select count(*) from guestbook where name = 'Load'")" = 8 ] || fail "all 8 concurrent writes should land"
  rm -rf $t
fi

# The version lives in one place, CL_VERSION in src/cl.c. It must be well formed and
# have its own section in CHANGELOG.md (make release does both).
v=$(./cl version | sed -n 's/^Common (CL) \([0-9]*\.[0-9]*\.[0-9]*\)$/\1/p')
[ -n "$v" ]                                                         || fail "cl version prints Common (CL) MAJOR.MINOR.PATCH"
grep -q "^## $v (" CHANGELOG.md                                     || fail "CHANGELOG.md has no section for $v: run make release, or add it"
python3 -c 'import json,sys; [json.load(open(f)) for f in sys.argv[1:]]' editor/vscode/package.json editor/vscode/syntaxes/cl.json editor/vscode/syntaxes/cl.injection.json || fail "vscode grammar is valid json"
python3 -I tests/check_grammar.py >/dev/null || fail "vscode grammar is missing a word"

# 3f. A visitor's value stays a visitor's value
t=$(mktemp -d); printf 'secret' > $t/cl.secret
printf '<cl>show path</cl>' > $t/p1.html
[ "$(./cl $t/p1.html "" "" "/<b>x")" = "/&lt;b&gt;x" ]                       || fail "path is made safe when shown"
printf '<cl>include last of split path by "/"</cl>' > $t/p2.html
./cl $t/p2.html "" "" "/x/cl.secret" | grep -q 'came from a visitor'         || fail "include refuses a file name from the path"
if [ -n "$has_db" ]; then
  printf '<cl>set sql to
  select "[query "q"]"
  end
  show sql
  set rows to ask database sql</cl>' > $t/p3.html
  ./cl $t/p3.html "q=x" | grep -q "SQL has a visitor's value"                 || fail "a text block remembers a visitor's value"
  for w in 'trim of' 'uppercase of' 'replace "a" with "b" in' 'first 50 of' 'last of split' ; do
    case "$w" in 'last of split') e='last of split "x [query "q"]" by " "' ;; *) e="$w \"select [query \"q\"]\"" ;; esac
    printf '<cl>set s to %s\nset rows to ask database s</cl>' "$e" > $t/p4.html
    ./cl $t/p4.html "q=x" | grep -q "SQL has a visitor's value"               || fail "'$w' keeps a visitor's value marked"
  done
fi
printf '<cl>show "<a title=%s[query "t"]%s href=\\"[query "u"]\\">x</a>"</cl>' "'" "'" > $t/p5.html
[ "$(./cl $t/p5.html "t=%27+onclick%3D%27x&u=+JavaScript%3Aalert(1)")" = "<a title='&#39; onclick=&#39;x' href=\"#unsafe-link\">x</a>" ] || fail "single quotes and javascript: links from a visitor are made safe"
printf '<cl>show "<a href=\\"/search?q=[query "u"]\\">x</a>"</cl>' > $t/p6.html
[ "$(./cl $t/p6.html "u=javascript:x")" = '<a href="/search?q=javascript:x">x</a>' ] || fail "a visitor's value later in a link is left alone"
printf '<cl>show first of "héllo"\nshow last of "café"</cl>' > $t/p7.html
[ "$(./cl $t/p7.html | tr -d '\n')" = "hé" ]                                || fail "first of and last of take whole letters"

# 3g. Numbers from visitors, text blocks, dates
printf '<cl>set age to query "age"\nif age is more than 12\n  show "older"\notherwise\n  show "younger"\nend</cl>' > $t/n1.html
[ "$(./cl $t/n1.html "age=9")" = "younger" ]                                 || fail "9 from a visitor is less than 12"
[ "$(./cl $t/n1.html "age=100")" = "older" ]                                 || fail "100 from a visitor is more than 12"
./cl $t/n1.html "age=abc" | grep -q 'cannot compare &quot;abc&quot; with the number 12' || fail "comparing words with a number is an error"
./cl $t/n1.html | grep -q 'got empty text'                                   || fail "comparing an empty field with a number is an error"
printf '<cl>if query "n" is 3\n  show "same"\nend\nif "b" is more than "a"\n  show "text order"\nend</cl>' > $t/n2.html
[ "$(./cl $t/n2.html "n=3.0" | tr -d '\n')" = "sametext order" ]            || fail "3.0 is 3, and text still compares as text"
printf '<cl>show\nThe end.\nend of story\nend\nshow "after"</cl>' > $t/n3.html
[ "$(./cl $t/n3.html | tr -d '\n')" = "The end.end of storyafter" ]          || fail "only a line that is just end closes a text block"
printf '<cl>show date of "2026-02-31"</cl>' > $t/n4.html
./cl $t/n4.html | grep -q 'is not a date'                                    || fail "31 February is not a date"
printf '<cl>show date of "2028-02-29"</cl>' > $t/n5.html
[ "$(./cl $t/n5.html)" = "29 February 2028" ]                                || fail "29 February in a leap year is a date"

# 3h. Practice code cannot take the machine down
printf '<cl>show try form "code"</cl>' > $t/s1.html
./cl $t/s1.html "" "code=set+x+to+%22ab%22%0Arepeat+40+times%0A++set+x+to+%22%5Bx%5D%5Bx%5D%22%0Aend" | grep -q 'too much memory' || fail "practice code has a memory limit"
./cl $t/s1.html "" "code=repeat+900000000+times%0Aend" | grep -q 'more than 20000 steps' || fail "an empty loop counts its turns"
./cl $t/s1.html "" "code=set+n+to+0%0Arepeat+5+times%0A++set+n+to+n+%2B+1%0Aend%0Ashow+n" | grep -q '^5' || fail "ordinary practice code still runs"

# 3i. Passwords: new hashes are PBKDF2, old cl1 hashes still sign in
printf '<cl>set h to hash of "pw"\nshow first 11 of h\nshow "pw" matches h\nshow "px" matches h\nshow "cookies" matches "cl1$52cf453c37c853f56d3e600819018ab9$b861e0f764fce70c042a4dedc1d04cabcc19e16c52279c221c4c6efcd6f69e19"</cl>' > $t/h1.html
./cl $t/h1.html | tr -d '\n' | grep -q '^cl2\$200000\$yesnoyes$'    || fail "hash of makes a cl2 PBKDF2 hash that matches"
rm -rf $t

# 3j. Email: only the page picks the recipient, headers stay headers, and there is a speed limit
t=$(mktemp -d)
mail() { printf '<cl>\n%s\n</cl>\n' "$1" > $t/m.html; ./cl $t/m.html "$2" "$3" 2>&1; }
ok='  subject: "Hi"
  from: "site@example.com"
  body: "Hello"
end'
mail "send email to form \"to\"
$ok" "" "to=victim%40example.com" | grep -q "cannot take an address that came from a visitor" || fail "email: a visitor cannot choose the recipient"
mail "send email to \"owner@example.com\"
  subject: \"Hi\"
  from: form \"f\"
  body: \"Hello\"
end" "" "f=a%40b.com" | grep -q "cannot take an address that came from a visitor" || fail "email: a visitor cannot choose the sender"
mail "send email to \"not an address\"
$ok" | grep -q "needs one email address"                            || fail "email: the recipient must be one address"
mail 'send email to "owner@example.com"
  subject: "Hi"
  body: "Hello"
end' | grep -q "say who the email is from"                           || fail "email: a sender is required"
mail 'send email to "owner@example.com"
  cc: "x@example.com"
end' | grep -q "not 'cc:'"                                          || fail "email: only subject, body, reply to and from"
rm -rf $t/outbox $t/mail.log
mail 'send email to "owner@example.com"
  subject: "From [form "n"]"
  reply to: form "r"
  from: "site@example.com"
  body: "Hello"
end' "" "n=Ada%0D%0ABcc:+evil%40example.com&r=a%40b.com%0ABcc:+evil%40example.com" | grep -q "saved in outbox/" || fail "email: saved in outbox while developing"
e=$(cat $t/outbox/*.eml)
echo "$e" | grep -q '^Bcc'                                          && fail "email: a line break in the subject cannot add a header"
echo "$e" | grep -q '^Subject: From Ada  Bcc: evil@example.com$'    || fail "email: line breaks in the subject become spaces"
echo "$e" | grep -q '^Reply-To'                                     && fail "email: a reply-to that is not one address is left off"
grep -q 'owner@example.com' $t/mail.log                             || fail "email: every send is logged for the speed limit"
mail "repeat 4 times
  send email to \"owner@example.com\"
  $ok
end" | grep -q "at most 3 emails"                                  || fail "email: at most 3 per page"
CL_MAIL_PER_HOUR=4 ./cl $t/m.html 2>&1 | grep -q "already sent 4 emails in the last hour" || fail "email: a speed limit per hour"
printf '<cl>show try "send email to \\"o@example.com\\"\\n  subject: \\"x\\"\\n  from: \\"s@example.com\\"\\n  body: \\"x\\"\\nend"</cl>' > $t/s.html
./cl $t/s.html | grep -q "not allowed in practice code"             || fail "email: not allowed in practice code"
printf '<cl>set l to "ada@example.com", "x@y.co", "nope", "a@b", "a b@c.com", "a@b.com, c@d.com", "<a@b.com>"\nfor each a in l\n  show a is an email\nend\nset email to "a name"\nshow email</cl>' > $t/is.html
[ "$(./cl $t/is.html | tr -d '\n')" = "yesyesnonononono" ] || [ "$(./cl $t/is.html | tr -d '\n')" = "yesyesnononononoa name" ] || fail "is an email"
./cl $t/is.html | grep -q '^a name$'                                || fail "email is still fine as a name"
rm -rf $t/outbox $t/mail.log
printf '#!/bin/sh\ncat > %s/piped.txt\n' $t > $t/fake-sendmail; chmod +x $t/fake-sendmail
mail "send email to \"owner@example.com\"
$ok" >/dev/null
SCRIPT_FILENAME=$t/m.html CL_SENDMAIL=$t/fake-sendmail ./cl cgi >/dev/null
grep -q '^To: owner@example.com$' $t/piped.txt                      || fail "email: cl cgi hands the email to sendmail"
[ -d $t/outbox ] && [ "$(ls $t/outbox | wc -l | tr -d ' ')" = 1 ]   || fail "email: only the development run used the outbox"
printf '#!/bin/sh\nexit 1\n' > $t/bad-sendmail; chmod +x $t/bad-sendmail
CL_MAIL=sendmail CL_SENDMAIL=$t/bad-sendmail ./cl $t/m.html 2>&1 | grep -q "did not take the email" || fail "email: a sendmail failure is an error"
port=$((20000 + ($$ + 7) % 10000))
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
f=$(ls $t/outbox | head -1)
[ "$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$port/outbox/$f")" = 403 ] || fail "server must not serve the outbox"
[ "$(curl -s -o /dev/null -w '%{http_code}' http://localhost:$port/mail.log)" = 403 ]   || fail "server must not serve mail.log"
pkill -f "cl serve $port" ; rm -rf $t

# 3k. Verbs read the caller's names without copying them, and still cannot change them
t=$(mktemp -d)
cat > $t/v.html <<'CL'
<cl>
set total to 1
set items to "a", "b"
set lamp to record
  price: 19
end
to change_all
  set total to 99
  add "c" to items
  set price of lamp to 5
  show "inside: [total] [count of items] [price of lamp]"
end
change_all
show "outside: [total] [count of items] [price of lamp]"
to outer
  set level to "outer"
  inner
end
to inner
  show "inner sees [level] and [total]"
end
outer
to countdown n
  if n is more than 0
    show "[n]"
    countdown n - 1
  end
end
countdown 3
</cl>
CL
v=$(./cl $t/v.html)
echo "$v" | grep -q '^inside: 99 3 5$'                              || fail "a verb changes its own copies"
echo "$v" | grep -q '^outside: 1 2 19$'                             || fail "a verb cannot change the caller's names, lists or records"
echo "$v" | grep -q '^inner sees outer and 1$'                      || fail "a verb called from a verb sees both callers' names"
[ "$(echo "$v" | grep -c '^[123]$')" = 3 ]                          || fail "a verb can call itself"
printf '<cl>\nset total to 1\nto typo\n  show totl\nend\ntypo\n</cl>\n' > $t/v2.html
./cl $t/v2.html | grep -q "Did you mean 'total'"                    || fail "inside a verb, a typo suggests the caller's name"

# 3l. Errors for the things beginners mix up
printf '<cl>\nset post to record\n  title: "x"\nend\nfor each x in post\nend\n</cl>\n' > $t/f1.html
./cl $t/f1.html | grep -q "post is one record. Read its columns directly, like: show &quot;\[title of post\]&quot;" || fail "for each over a record says how to read it"
printf '<cl>\nset name to "Ada"\nfor each x in name\nend\n</cl>\n' > $t/f2.html
./cl $t/f2.html | grep -q "for each word in split name by"          || fail "for each over text suggests split"
printf '<cl>\nset n to 3\nfor each x in n\nend\n</cl>\n' > $t/f3.html
./cl $t/f3.html | grep -q "write: repeat n times"                   || fail "for each over a number suggests repeat"
printf '<cl>\nset r to record\n  price: 1\n  stock: 2\nend\nshow priec of r\n</cl>\n' > $t/f4.html
./cl $t/f4.html | grep -q "has no 'priec'. Did you mean 'price'? It has: price, stock" || fail "a misspelled column suggests the right one"
printf '<cl>\nset r to record\n  x: 1\nend\nshow id of r\n</cl>\n' > $t/f5.html
./cl $t/f5.html | grep -q "Did you mean"                            && fail "a short column name is not matched to an unrelated one"

# 3m. A form sent from another site is refused; every response says nosniff and no foreign frames
printf '<cl>\nshow "got [form "a"]"\n</cl>\n' > $t/index.html
port=$((20000 + ($$ + 13) % 10000))
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
u=http://127.0.0.1:$port/
[ "$(curl -s -d a=1 $u)" = "got 1" ]                                || fail "a POST with no Origin still works"
[ "$(curl -s -d a=1 -H "Origin: http://127.0.0.1:$port" $u)" = "got 1" ] || fail "a POST from the same site works"
[ "$(curl -s -o /dev/null -w '%{http_code}' -d a=1 -H 'Origin: https://evil.example' $u)" = 403 ] || fail "a POST from another site is refused"
[ "$(curl -s -o /dev/null -w '%{http_code}' -d a=1 -H 'Origin: null' $u)" = 403 ] || fail "a POST from a sandboxed frame or file is refused"
[ "$(curl -s -d a=1 -H 'Host: internal:1' -H 'X-Forwarded-Host: mysite.com' -H 'Origin: https://mysite.com' $u)" = "got 1" ] || fail "a POST through a proxy that sets X-Forwarded-Host works"
[ "$(curl -s -H 'Origin: https://evil.example' $u)" = "got " ]      || fail "a GET from another site is not refused"
h=$(curl -s -D - -o /dev/null $u)
echo "$h" | grep -q '^X-Content-Type-Options: nosniff'              || fail "responses say nosniff"
echo "$h" | grep -q '^X-Frame-Options: SAMEORIGIN'                  || fail "responses refuse frames on other sites"
pkill -f "cl serve $port"
printf 'a=1' | SCRIPT_FILENAME=$t/index.html REQUEST_METHOD=POST CONTENT_LENGTH=3 HTTP_HOST=site.com HTTP_ORIGIN=https://evil.example ./cl cgi | grep -q '^Status: 403' || fail "cgi refuses a POST from another site"
printf 'a=1' | SCRIPT_FILENAME=$t/index.html REQUEST_METHOD=POST CONTENT_LENGTH=3 HTTP_HOST=site.com HTTP_ORIGIN=https://site.com ./cl cgi | grep -q 'got 1' || fail "cgi takes a POST from the same site"
SCRIPT_FILENAME=$t/index.html ./cl cgi | grep -q '^X-Content-Type-Options: nosniff' || fail "cgi responses say nosniff"
rm -rf $t

# 3n. Text joined with a visitor's value is stored and sent as plain text, so it is escaped once when shown
t=$(mktemp -d)
cat > $t/a.html <<'CL'
<cl>
set note to "From [form "who"]: [form "msg"]"
remember "note" as note
to keep_it
  remember "from_verb" as "yes"
end
keep_it
go to "/find?who=[form "who"]"
</cl>
CL
printf '<cl>\nshow remembered "note"\nshow remembered "from_verb"\n</cl>\n' > $t/b.html
err=$(./cl $t/a.html "" "who=Bo+%26+Al&msg=%3Cb%3Ehi%3C%2Fb%3E" 2>&1 >/dev/null)
echo "$err" | grep -q 'goes to /find?who=Bo%20&%20Al'               || fail "a go-to address joined with a visitor's value is not HTML-escaped"
ck=$(echo "$err" | sed -n 's/.*set-cookie: \(cl_session=[^;]*\).*/\1/p')
b=$(./cl $t/b.html "" "" "" "$ck")
echo "$b" | grep -q '^From Bo &amp; Al: &lt;b&gt;hi&lt;/b&gt;$'    || fail "a remembered joined value is escaped once, not twice"
echo "$b" | grep -q '^yes$'                                          || fail "remember inside a verb sets the cookie"
if [ -n "$has_db" ]; then
  sqlite3 $t/d.db "create table n (body text)"
  printf '<cl>\nopen database "d.db"\ntell database "insert into n (body) values (?)" with "From [form "who"]"\n</cl>\n' > $t/c.html
  ./cl $t/c.html "" "who=Bo+%26+%3CAl%3E" >/dev/null
  [ "$(sqlite3 $t/d.db 'select body from n')" = "From Bo & <Al>" ]  || fail "a joined value goes into the database as plain text"
fi
rm -rf $t

# 3o. Lines are read once and kept: the same results, however often a line runs
t=$(mktemp -d)
cat > $t/c.html <<'CL'
<cl>
to again n
  to again n
    give "new [n]"
  end
  give "old [n]"
end
show again 1
show again 2
set a to 1
set b to 2
set out to ""
repeat 3 times
  set out to "[out][a][b]"
  set a to a + 10
end
show out
show "[a]"
show "[b]"
</cl>
CL
[ "$(./cl $t/c.html | tr '\n' ' ')" = "old 1 new 2 12112212 31 2 " ] || fail "kept lines and holes give the same results on every run, and a verb can be defined again while it runs"
printf '<cl>\nrepeat 3 times\n  show Hello there\nend\n</cl>\n' > $t/q.html
./cl $t/q.html | grep -q 'need to be in double quotes: show &quot;Hello there&quot;' || fail "the missing-quotes hint still works inside a loop"
printf '<cl>\nset items to 1, 2, "x"\nfor each n in items\n  show n * 2\nend\n</cl>\n' > $t/e.html
./cl $t/e.html | tr '\n' ' ' | grep -q "^2 4 .*Common error: Line 4" || fail "an error on a later turn of a loop still names its line"
rm -rf $t

# 3p. Hostile input: crashes, redirects, CPU and private files (from a security review)
t=$(mktemp -d)
printf '<cl>show try form "code"</cl>' > $t/s.html
./cl $t/s.html "" "code=show+%22a%01b%22" | grep -q 'invisible control character'  || fail "a stray hole marker in code is an error, not a crash"
deep=$(python3 -I -c "import urllib.parse; print(urllib.parse.quote_plus('show ' + '('*3000 + '1' + ')'*3000))")
./cl $t/s.html "" "code=$deep" | grep -q 'nests too deeply'          || fail "3000 nested brackets are an error, not a crash"
deep=$(python3 -I -c "import urllib.parse; print(urllib.parse.quote_plus('show ' + 'not '*5000 + 'yes'))")
./cl $t/s.html "" "code=$deep" | grep -q 'nests too deeply'          || fail "5000 nots are an error, not a crash"
printf '<cl>show ((((1 + 2) * 3)))\nshow "[(2 * [3])]"</cl>' > $t/n.html
./cl $t/n.html | grep -q '^9$'                                      || fail "ordinary brackets still work"
./cl $t/s.html "" "code=show+hash+of+%22x%22" | grep -q "hash of&#39; is not allowed in practice code" || fail "hash of is not allowed in practice code"
./cl $t/s.html "" "code=show+%22a%22+matches+%22b%22" | grep -q "matches&#39; is not allowed in practice code" || fail "matches is not allowed in practice code"
printf '<cl>go to query "u"</cl>' > $t/g.html
./cl $t/g.html 'u=/%5Cevil.example' | grep -q 'may only point to a page on this site' || fail "go to refuses /\\evil.example"
printf '<cl>go to "/a\\\\b"</cl>' > $t/g2.html
./cl $t/g2.html 2>&1 >/dev/null | grep -q 'goes to /a%5Cb'           || fail "a backslash in a go-to address is encoded"
printf 'secret' > $t/cl.secret; printf 'x' > $t/site.db; printf '<cl>show "page"</cl>' > $t/p.html
[ "$(SCRIPT_FILENAME=$t/cl.secret ./cl cgi | head -1 | tr -d '\r')" = "Status: 403 Forbidden" ] || fail "cgi never runs cl.secret"
SCRIPT_FILENAME=$t/cl.secret ./cl cgi | grep -q secret              && fail "cgi never prints cl.secret"
[ "$(PATH_TRANSLATED=$t/site.db ./cl cgi | head -1 | tr -d '\r')" = "Status: 403 Forbidden" ] || fail "cgi never runs a database"
SCRIPT_FILENAME=$t/p.html ./cl cgi | grep -q '^page'                 || fail "cgi still runs pages"
printf '<cl>remember "a" as "b"</cl>' > $t/r.html
SCRIPT_FILENAME=$t/r.html HTTPS=on ./cl cgi | grep -q 'SameSite=Lax; Secure' || fail "cookies are Secure over HTTPS"
SCRIPT_FILENAME=$t/r.html ./cl cgi | grep -q 'Secure'                && fail "cookies are not marked Secure over plain HTTP"
printf '<cl>remember "big" as form "v"\nshow "ok"</cl>' > $t/index.html
port=$((20000 + ($$ + 21) % 10000))
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
big=$(python3 -I -c "print('x' * 6000)")
[ "$(curl -s -o /dev/null -w '%{http_code}' -d "v=$big" http://127.0.0.1:$port/)" = 500 ] || fail "a response header too big for the buffer is a 500, not a memory leak"
pkill -f "cl serve $port"
rm -rf $t

# 3q. More hostile input, private files outside the web root, and wrong passwords
t=$(mktemp -d)
printf '<cl>show try form "code"</cl>' > $t/s.html
./cl $t/s.html "" "code=set+5+to%0Aend" | grep -q 'expected a name'           || fail "set 5 to (a text block with a number for a name) is an error, not a crash"
./cl $t/s.html "" "code=set+5+to+record%0Aend" | grep -q 'expected a name'    || fail "set 5 to record is an error, not a crash"
# 5000 deep is far past the limit of 200 and keeps this one argument under 128 KB, the most Linux allows
deep=$(python3 -I -c "import urllib.parse; print(urllib.parse.quote_plus('show ' + 'count of '*5000 + '\"x\"'))")
./cl $t/s.html "" "code=$deep" | grep -q 'nests too deeply'                  || fail "a long chain of count of is an error, not a crash"
python3 -I -c "print('<cl>\n' + 'if yes\n'*20000 + 'show 1\n' + 'end\n'*20000 + '</cl>')" > $t/d.html
./cl $t/d.html | grep -q 'nested too deeply'                                 || fail "20000 nested ifs are an error, not a crash"
# The same with the small stack some hosts give: still an error, never a crash (see stack_check in cl.c)
(ulimit -s 1024; ./cl $t/d.html) | grep -q 'nested too deeply'                 || fail "with a 1 MB stack, 20000 nested ifs are still an error, not a crash"
(ulimit -s 1024; ./cl $t/s.html "" "code=$deep") | grep -q 'too deeply'        || fail "with a 1 MB stack, a long chain of count of is still an error, not a crash"
printf '<cl>\nto countdown n\n  if n is more than 0\n    countdown n - 1\n  end\nend\ncountdown 99\nshow "deep ok"\n</cl>\n' > $t/r.html
./cl $t/r.html | grep -q '^deep ok'                                          || fail "a verb may still call itself 99 deep with an if inside"
mkdir -p $t/home/public_html; d=$t/home/public_html
printf '<cl>remember "a" as "b"\nshow "ok"</cl>' > $d/p.html
SCRIPT_FILENAME=$d/p.html DOCUMENT_ROOT=$d ./cl cgi >/dev/null
[ -f $t/home/.common/public_html/cl.secret ] && [ ! -f $d/cl.secret ]       || fail "cgi keeps cl.secret outside the web root"
[ "$(ls -l $t/home/.common/public_html/cl.secret | cut -c1-10)" = "-rw-------" ] || fail "cl.secret is readable only by its owner"
SCRIPT_FILENAME=$d/p.html DOCUMENT_ROOT=$d CL_PRIVATE_DIR=$t ./cl cgi >/dev/null
[ -f $t/cl.secret ]                                                          || fail "CL_PRIVATE_DIR chooses the private folder"
if [ -n "$has_db" ]; then
  h=$(printf '<cl>show hash of "right"</cl>' > $t/h.html; ./cl $t/h.html)
  sqlite3 $t/u.db "create table users (name text, password text); insert into users values ('ada', '$h')"
  printf '<cl>\nopen database "u.db"\nset u to first of ask database "select password from users where name = ?" with "ada"\nif form "p" matches password of u\n  show "in"\notherwise\n  show "wrong"\nend\n</cl>\n' > $t/login.html
  login() { printf "p=$1" | SCRIPT_FILENAME=$t/login.html DOCUMENT_ROOT=$t REMOTE_ADDR=$2 CL_PRIVATE_DIR=$t CONTENT_LENGTH=${#3} ./cl cgi | tail -1; }
  for i in 1 2 3 4 5 6 7 8 9 10; do login wrong 1.2.3.4 p=wrong >/dev/null; done
  login wrong 1.2.3.4 p=wrong | grep -q 'too many wrong passwords'          || fail "after 10 wrong passwords, an address must wait"
  login right 1.2.3.4 p=right | grep -q 'too many wrong passwords'          || fail "the wait applies to the right password too"
  [ "$(login right 5.6.7.8 p=right)" = "in" ]                                || fail "another address can still sign in"
  [ "$(wc -l < $t/login.log | tr -d ' ')" = 10 ]                             || fail "only wrong passwords are counted"
fi
rm -rf $t

# 3r. include "/x.html" starts from the site's top folder, so pages in subfolders can share one header
t=$(mktemp -d); mkdir -p $t/shop/deep
printf '<cl>set where to "top"</cl>' > $t/header.html
printf '<cl>include "/header.html"\nshow "[where]"</cl>' > $t/shop/index.html
printf '<cl>include "/header.html"\nshow "deep [where]"</cl>' > $t/shop/deep/p.html
[ "$(cd $t && "$OLDPWD/cl" shop/index.html)" = "top" ]               || fail "from the command line, / is the folder cl runs in"
[ "$(SCRIPT_FILENAME=$t/shop/index.html DOCUMENT_ROOT=$t CL_PRIVATE_DIR=$t ./cl cgi | tail -1)" = "top" ] || fail "under cgi, / is the web root"
port=$((20000 + ($$ + 33) % 10000))
(./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
[ "$(curl -s http://127.0.0.1:$port/shop/deep/p)" = "deep top" ]    || fail "under cl serve, / is the folder being served, from any depth"
pkill -f "cl serve $port"
printf '<cl>include "/../x.html"</cl>' > $t/shop/b.html
./cl $t/shop/b.html | grep -q "never a path with .."                || fail "include refuses .. after a leading / too"
rm -rf $t

# 3s. JSON: read json and json of. A visitor's JSON stays a visitor's, and json of is safe in a <script>
t=$(mktemp -d)
printf '<cl>set v to read json form "data"\nshow "[name of v] [count of tags of v] [ok of v] [n of v]"\nshow raw json of v\nshow json of v</cl>' > $t/a.html
out=$(./cl $t/a.html "" 'data=%7B%22name%22%3A%22%3Cb%3EEve%22%2C%22tags%22%3A%5B1%2C2%5D%2C%22ok%22%3Atrue%2C%22n%22%3A-1.5e2%7D')
echo "$out" | sed -n 1p | grep -qx '&lt;b&gt;Eve 2 yes -150'                       || fail "read json: a visitor's text is escaped, numbers and yes/no are values"
echo "$out" | sed -n 2p | grep -qx '{"name":"\\u003cb\\u003eEve","tags":\[1,2\],"ok":true,"n":-150}' || fail "json of writes < > as \\u escapes"
echo "$out" | sed -n 3p | grep -q '^{&quot;name&quot;'                              || fail "json of a visitor's values is escaped when shown"
printf '<cl>set v to read json form "data"</cl>' > $t/b.html
./cl $t/b.html "" 'data=%7B%22a%22%3A%5B' | grep -q "is not JSON: it ends where a value should be (line 1, letter 7)" || fail "read json: a cut-off text says where"
./cl $t/b.html "" "data=$(python3 -c 'print("%5B" * 300)')" | grep -q "more than 200 lists or records inside each other" || fail "read json: deep nesting is an error, not a crash"
printf '<cl>set v to read json form "data"\nshow v\nshow "[v]"\nshow uppercase of v</cl>' > $t/k.html
./cl $t/k.html "" 'data=%7B%22%3Cs%3E%22%3A1%7D' | grep -q '<s>\|<S>'               && fail "read json: a visitor's keys are escaped too"
./cl $t/b.html "" "data=%22%5Cud83d%22" | grep -q "half a letter"                 || fail "read json: half an emoji is an error"
./cl $t/b.html "" "data=%22%5Cud83d%5Cude00%22" | grep -q "Common error"          && fail "read json: an emoji in two halves reads"
cat > $t/c.html <<'CL'
<cl>show try "set v to read json \"\\\[1, 2, 3\\\]\"\nshow count of v"</cl>
CL
[ "$(./cl $t/c.html)" = "3" ]                                                      || fail "read json works in practice code"
rm -rf $t

# 3t. fetch, when this build has the fetch module: the page picks the site, a visitor's value
# is encoded after the host, keys stay with their host, and the reply is another site's text
if ./cl words | grep -q '^  fetch'; then
  t=$(mktemp -d); port=$((20000 + ($$ + 47) % 10000))
  cat > $t/api.py <<'PY'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, body, ctype="application/json"):
        b = body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/posts": self.reply(200, json.dumps({"data": [{"attributes": {"title": "Hello <b>world</b>"}}, {"attributes": {"title": "Second"}}]}), "application/vnd.api+json")
        elif p == "/text": self.reply(200, "plain <i>text</i>", "text/plain")
        elif p == "/bad": self.reply(200, "{nope")
        elif p == "/empty": self.reply(204, "")
        elif p == "/secret": self.reply(401, "{}")
        elif p == "/moved": self.send_response(302); self.send_header("Location", "/posts"); self.send_header("Content-Length", "0"); self.end_headers()
        else: self.do_POST()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        self.reply(200, json.dumps({"method": self.command, "path": self.path, "auth": self.headers.get("Authorization", ""), "type": self.headers.get("Content-Type", ""), "body": self.rfile.read(n).decode()}))
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
  (python3 -I $t/api.py $port >/dev/null 2>&1 &)
  i=0; until curl -s -o /dev/null "http://127.0.0.1:$port/x" || [ $i -ge 100 ]; do sleep 0.1; i=$((i + 1)); done
  a="http://127.0.0.1:$port"
  printf '<cl>set posts to fetch "%s/posts"\nfor each post in data of posts\n  show "<li>[title of attributes of post]</li>"\nend\nshow fetch "%s/text"</cl>' $a $a > $t/a.html
  [ "$(./cl $t/a.html | tr -d '\n')" = "<li>Hello &lt;b&gt;world&lt;/b&gt;</li><li>Second</li>plain &lt;i&gt;text&lt;/i&gt;" ] || fail "fetch: JSON becomes records, text stays text, both escaped"
  printf '<cl>set r to fetch "%s/item/[query "slug"]?q=[query "q"]"\nshow "[path of r] [auth of r]"</cl>' $a > $t/b.html
  [ "$(./cl $t/b.html 'slug=a%2F..%2Fb%3Fc%26d&q=x%20y')" = "/item/a%2F..%2Fb%3Fc%26d?q=x%20y " ] || fail "fetch: a visitor's value is encoded after the host"
  printf '127.0.0.1 Authorization: Bearer s3cret\n' > $t/cl.keys
  [ "$(./cl $t/b.html 'slug=x')" = "/item/x?q= Bearer s3cret" ]                    || fail "fetch: cl.keys adds the key for its host"
  printf '<cl>set r to fetch "http://localhost:%s/x"\nshow raw r</cl>' $port > $t/c.html
  ./cl $t/c.html | grep -q s3cret                                                   && fail "fetch: a key is never sent to another host"
  printf '<cl>set order to record\n  note: form "note"\n  qty: 2\nend\nset r to fetch "%s/orders" with order\nshow "[method of r] [type of r] "\nshow raw body of r</cl>' $a > $t/d.html
  [ "$(./cl $t/d.html '' 'note=%3Cb%3Ehi' | tr -d '\n')" = 'POST application/json {"note":"\u003cb\u003ehi","qty":2}' ] || fail "fetch with a record posts it as JSON"
  printf '<cl>show "[fetch "%s/empty"]."</cl>' $a > $t/e.html
  [ "$(./cl $t/e.html)" = "." ]                                                     || fail "fetch: an empty reply is empty text"
  rm $t/cl.keys
  printf '<cl>set r to fetch "%s/moved"\nshow "[count of data of r]"</cl>' $a > $t/f.html
  [ "$(./cl $t/f.html)" = "2" ]                                                     || fail "fetch follows a redirect when there is no key"
  for c in 'fetch "http://[query "h"]/x"|may only go after the host' "fetch \"$a[query \"h\"]\"|may only go after the host" \
           'fetch query "h"|came from a visitor' 'fetch "ftp://127.0.0.1/x"|starts with https:// or http://' \
           "fetch \"$a/a b\"|cannot be in an address" "fetch \"$a/bad\"|the reply from 127.0.0.1 is not JSON" \
           "fetch \"$a/x\" with 5|sends a record or a list" \
           'fetch "http://127.0.0.1:1/x"|did not take the connection' 'fetch "http"|starts with https:// or http://'; do
    printf '<cl>set r to %s</cl>' "${c%%|*}" > $t/g.html
    ./cl $t/g.html 'h=evil.example' | grep -q "${c#*|}"                            || fail "fetch error: ${c%%|*}"
  done
  printf '<cl>set r to fetch "%s/secret?key=hidden"</cl>' $a > $t/g.html
  ./cl $t/g.html | grep -q 'answered 401: it wants a key'                          || fail "fetch error: 401 asks for a key"
  ./cl $t/g.html | grep -q 'key=hidden'                                             && fail "fetch errors never show the query, where a key may be"
  printf '<cl>show try "set r to fetch \\"%s/x\\""</cl>' $a > $t/h.html
  ./cl $t/h.html | grep -q "not allowed in practice code"                          || fail "fetch is not allowed in practice code"
  printf '<cl>repeat 21 times\n  set r to fetch "%s/x"\nend</cl>' $a > $t/i.html
  ./cl $t/i.html | grep -q "at most 20 addresses"                                  || fail "fetch: at most 20 requests a page"
  pkill -f "api.py $port"
  # the server never sends cl.keys
  printf 'x Authorization: k\n' > $t/cl.keys; port=$((port + 1))
  (./cl serve $port $t >/dev/null 2>&1 &) ; wait_for $port
  [ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$port/cl.keys)" = 403 ] || fail "server must not serve cl.keys"
  pkill -f "cl serve $port"
  rm -rf $t
else
  echo "(this build has no fetch module, fetch tests skipped)"
fi

# 4. Error messages
t=$(mktemp -d)
printf '<cl>show hello</cl>' > $t/a.html
./cl $t/a.html | grep -q "need to be in double quotes: show &quot;hello&quot;" || fail "unknown name gets the quoting hint"
printf '<cl>show "x"' > $t/b.html
./cl $t/b.html | grep -q 'never closed'                   || fail "unclosed cl block"
printf '<cl>\nif yes\nshow "x"\n</cl>' > $t/c.html
./cl $t/c.html | grep -q "Line 2: this 'if' is never closed" || fail "missing end"
printf '<cl>show item 5 of "a"</cl>' > $t/d.html
./cl $t/d.html | grep -q 'needs a list'                   || fail "item of non-list"
printf '<cl>include "nope.html"</cl>' > $t/e.html
./cl $t/e.html | grep -q 'cannot find the file'           || fail "missing include"
printf '<cl>set show to 1</cl>' > $t/f.html
./cl $t/f.html | grep -q "'show' is a Common word"        || fail "reserved word as a name"
printf '<cl>set year to 5\nshow "[year] [year of "2026-10-24"]"</cl>' > $t/f2.html
[ "$(./cl $t/f2.html)" = "5 2026" ]                        || fail "noun-like words work as names and as functions"
printf '<cl>print "x"</cl>' > $t/g.html
./cl $t/g.html | grep -q "Use 'show'"                      || fail "synonym suggestion"
printf '<cl>sey "x"</cl>' > $t/h.html
./cl $t/h.html | grep -q "Did you mean 'set'"              || fail "typo suggestion"
printf '<cl>include "self.html"</cl>' > $t/self.html
./cl $t/self.html | grep -q 'include each other in a loop' || fail "include loop"
./cl words | grep -q 'show raw VALUE'                      || fail "cl words"
rm -rf $t

# 5. The skill for AI agents is up to date, and its examples run cleanly
{ cat skills/SKILL.head.md; ./cl words --markdown; } | cmp -s - skills/common/SKILL.md || fail "skills/common/SKILL.md is out of date: run make agents"
for w in $(./cl words --markdown | sed -n 's/^# \([a-z]*\)$/\1/p'); do
  grep -q "^# $w$" skills/common/SKILL.md                         || fail "the skill is missing the example for $w"
done
t=$(mktemp -d); cp -R skills/common/examples/. $t/
./cl $t/site/index.html | grep -q 'Common error'                   && fail "skill example site/index.html has an error"
./cl $t/contact.html | grep -q 'Common error'                      && fail "skill example contact.html has an error"
./cl $t/contact.html "" "sent=yes&name=Ada&email=ada%40example.com&message=Hello+from+the+skill" 2>&1 >/dev/null | grep -q 'goes to /contact?thanks=yes' || fail "skill example contact.html sends"
grep -q '^Reply-To: ada@example.com$' $t/outbox/*.eml               || fail "skill example contact.html replies to the visitor"
if ./cl words | grep -q 'ask database'; then
  sqlite3 $t/site.db "create table posts (slug text, title text, body text, posted text); insert into posts values ('hello', 'Hello', 'One.', '2026-10-28')"
  ./cl $t/posts.html "" "" "/posts" | grep -q "href='/posts/hello'"  || fail "skill example posts.html lists posts"
  ./cl $t/posts.html "" "" "/posts/hello" | grep -q '<h1>Hello</h1>' || fail "skill example posts.html shows a post"
  ./cl $t/posts.html "" "" "/posts/nope" | grep -q 'no post called'  || fail "skill example posts.html handles a missing post"
fi
rm -rf $t
# Every Common code block in the skill's guide parses (names it never set are fine: blocks are fragments)
python3 -I - skills/SKILL.head.md > /dev/null <<'PY' || fail "a code block in skills/SKILL.head.md is not valid Common"
import re, subprocess, sys, tempfile, os
blocks, cur = [], None
for line in open(sys.argv[1]).read().split("\n"):
    if line.startswith("```"):
        if cur is None: cur = [] if line == "```" else False     # only untagged blocks are Common
        else:
            if cur is not False: blocks.append("\n".join(cur))
            cur = None
    elif cur: cur.append(line)
    elif cur == []: cur.append(line)
bad = 0
d = tempfile.mkdtemp()
for block in blocks:
    if block.lstrip().startswith("<cl>"): block = block.replace("<cl>", "").replace("</cl>", "")
    block = re.sub(r'fetch "https?://[^/"]*', 'fetch "http://127.0.0.1:1', block)   # parse it, but never reach the network
    name = os.path.join(d, "b.html")
    open(name, "w").write("<cl>\n" + block + "\n</cl>\n")
    out = subprocess.run(["./cl", name], capture_output=True, text=True).stdout
    for m in re.findall(r"Common error: Line \d+: ([^<]*)", out):
        if re.search(r"I do not know what|nothing is set to it|no database is open|cannot open the database|has no database support|did not take the connection|has no fetch module", m): continue
        print(m); bad += 1
sys.exit(1 if bad else 0)
PY

# The demo site has its own tests, run when it is here
if [ -d examples/site ]; then sh tests/site.sh; fi

echo "All tests passed"
