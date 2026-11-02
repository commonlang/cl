# Modules

Common's interpreter, `src/cl.c`, is the language: every word, the parser,
the dev server and the safety rules. It includes no library. Anything that
needs one lives in a *module*: a small C file, `src/mod_NAME.c`, that wraps
the library behind the interface in `src/cl_module.h`. Today there are two,
`mod_sqlite.c` and `mod_postgres.c`, and one kind of capability, a database
driver. This page says how they fit and how to add one.

## What a module is

A module gives the core one or more *capabilities*. The core asks for a
capability by kind when a word needs it; today `open database`, `ask database`
and `tell database` ask for a `CL_CAP_DATABASE`, which is a `CLDatabase`
table of five functions:

| Function | Does |
|---|---|
| `claims(address)` | says whether this driver opens addresses like this one (`site.db` is a file; `postgres://...` is a server) |
| `open(host, address, dir)` | opens it and returns a handle, or fails |
| `close(handle)` | closes it |
| `ask(host, handle, sql, params)` | runs a query and returns the rows, a list of records |
| `tell(host, handle, sql, params)` | runs a change |

The core does the rest: it parses the words, refuses SQL or an address that
came from a visitor, keeps the open handle on the page, and closes it when the
page ends. A module never sees the parser, the environment or the page.

A module reaches the core only through the `CLHost` table it is handed:
`host->text()`, `host->number()`, `host->record_add()` and the rest build
values; `host->plain_text()` turns a value into text for the library;
`host->fail()` stops the page with a Common error. Two rules matter:

- **Text read back from a store goes through `host->untrusted_text()`.** A
  visitor may have written it. The page then escapes it when it is shown.
  This is the rule that keeps a stored `<script>` from running.
- **Free what you hold before `host->fail()`.** It does not return; it jumps
  back to the line being run. Finalize the statement, free the buffer, then
  fail.

## How modules are built

Statically, today. The Makefile adds `src/mod_sqlite.c` when `sqlite3.h` is
installed and `src/mod_postgres.c` when `pg_config` is found, and defines
`CL_SQLITE` or `CL_POSTGRES` so that `modules_load()` in the core calls that
module's entry point as `cl` starts. With neither, the core builds alone and
the database words are off.

The interface is written so the same source could be a shared library later.
Built with `-DCL_MODULE_SHARED`, the entry point is the one fixed name
`cl_module_entry`, which a loader would find with `dlopen` and `dlsym`; the
module needs nothing from the core but the `CLHost` pointer. `CL_MODULE_API`
is the version of the interface: it goes up whenever a struct in
`cl_module.h` changes, and the core refuses a module built against another
number. Nothing uses dynamic loading yet, and nothing in a module may assume
it will or will not be.

`cl words` ends with the modules in the build:

```
Modules in this build:
  sqlite     SQLite 3.51.0: open database "site.db", a file next to the page
  postgres   libpq 16.4: open database "postgres://user:password@host/dbname", a Postgres server
```

## Adding a module

1. Write `src/mod_NAME.c`. Include the library and `cl_module.h`, nothing from
   the core. Fill a table for the capability, then the entry point:

   ```c
   static const CLCapability caps[] = { { CL_CAP_DATABASE, &driver } };
   static const CLModule module = { CL_MODULE_API, "name", "Library 1.2", caps, 1 };
   CL_MODULE(name) { (void)host; return &module; }
   ```

2. In the Makefile, probe for the library the way the SQLite block does, and
   when it is there add the file to `SRC`, the define to `CFLAGS` and the
   library to `LDLIBS`. The build must still work without it.

3. In `src/cl.c`, add the entry point to the list in `modules_load()` under
   the same define, and declare it in `cl_module.h` next to the others.

4. Test it in `tests/run.sh` behind a check that the build has it, the way the
   Postgres tests check `cl words` for `postgres://`. Errors are features: test
   that a missing server, bad SQL and a wrong parameter count each give the
   right sentence.

5. Add the library to `NOTICE`. Its license must be compatible with Apache 2.0,
   and a person agrees to it before it goes in.

A new *kind* of capability (words for math, fetching a page over HTTP, reading
JSON) is a bigger step: it adds a table type and a `CLCapKind` to
`cl_module.h`, raises `CL_MODULE_API`, and gives the core the words that use it.
The words themselves, their one form each and their errors, always belong to
the core; a module only does the work behind them.
