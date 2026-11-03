/* cl_module.h - how the Common interpreter and its modules talk to each other.
 *
 * The core (src/cl.c) is the language: it parses every word, keeps the safety
 * rules (no visitor value ever reaches SQL, a file name or an address), and
 * turns anything that goes wrong into a Common error with the line number.
 * A module (src/mod_*.c) is a small piece of C that wraps one library and
 * gives the core one capability it cannot have on its own. Today the only
 * capability is a database driver; SQLite and Postgres each live in a module.
 *
 * Modules are compiled into cl: the Makefile adds src/mod_sqlite.c when
 * sqlite3.h is installed, src/mod_postgres.c when pg_config is found, and the
 * core lists their entry points in modules_load(). Nothing here depends on
 * that. A module sees the core only through the CLHost table it is handed, so
 * the same source could later be built as a shared library and found with
 * dlopen/LoadLibrary: that is what CL_MODULE_SHARED and the fixed entry name
 * cl_module_entry are for. To keep that door open:
 *
 *   - a module never calls a core function by name, only host->something
 *   - a module never sees Env, Toks or any other core struct, only Value
 *   - CL_MODULE_API goes up whenever a struct in this file changes, and the
 *     core refuses a module built against another number
 *   - a module frees whatever it holds before it calls host->fail, because
 *     fail does not return (it is a longjmp to the line being run)
 *
 * The core stays small: it does not include sqlite3.h, libpq-fe.h or
 * curl/curl.h, and it builds and runs with no module at all (the database
 * words and fetch are then off).
 */
#ifndef CL_MODULE_H
#define CL_MODULE_H

#define CL_MODULE_API 2

/* ------------------------------------------------------------------ */
/* Values: the one data type modules and the core share.                */
/* Build them with the host's functions, never by hand, so the memory   */
/* budget and the untrusted mark are kept. Reading the fields is fine.  */
/* ------------------------------------------------------------------ */

typedef enum { V_TEXT, V_NUM, V_BOOL, V_LIST, V_RECORD } VType;

typedef struct Value {
    VType  t;
    char  *s;              /* V_TEXT */
    double n;              /* V_NUM, V_BOOL */
    struct Value *items;   /* V_LIST and V_RECORD */
    char **keys;           /* V_RECORD: one name per item */
    int    count, cap;
    int    untrusted;      /* text typed by a visitor: escaped when shown or joined */
    int    tainted;        /* text with a visitor's value filled into it: fine to show, never SQL or a file name */
} Value;

/* ------------------------------------------------------------------ */
/* The host: what the core offers a module.                             */
/* ------------------------------------------------------------------ */

typedef struct CLHost {
    int         api;                /* CL_MODULE_API of the core */
    const char *version;            /* the core's version, "0.5.0" */

    /* Stop the page with a Common error. Never returns. The core adds the
       line number. Say what went wrong and what to write instead. */
    void (*fail)(const char *fmt, ...);

    /* Values. Text that a visitor may have written (anything read back from a
       store) must be made with untrusted_text, so the page escapes it. */
    Value (*text)(const char *s);
    Value (*untrusted_text)(const char *s);
    Value (*number)(double n);
    Value (*yes_no)(int b);
    Value (*list)(void);
    Value (*record)(void);
    void  (*list_add)(Value *list, Value item);                  /* takes the item */
    void  (*record_add)(Value *rec, const char *name, Value item); /* takes the item, copies the name */
    void  (*free_value)(Value *v);

    /* A value as plain text for a library: a text value with the escapes a page
       added turned back into letters, a number or yes/no spelled out. Malloc'd,
       the caller frees. */
    char *(*plain_text)(const Value *v);
    const char *(*type_name)(VType t);  /* "text", "a number", "yes or no", "a list", "a record" */
} CLHost;

/* ------------------------------------------------------------------ */
/* Capabilities: the kinds of thing a module can provide.               */
/* Each kind has one table type. New kinds (a set of words that give a   */
/* value, for math or JSON; a way to fetch a page, for HTTP) are added  */
/* here with their own table, and CL_MODULE_API goes up.               */
/* ------------------------------------------------------------------ */

typedef enum {
    CL_CAP_DATABASE = 1,    /* a CLDatabase: what 'open database', 'ask database' and 'tell database' talk to */
    CL_CAP_FETCH    = 2     /* a CLFetch: what 'fetch' sends its requests through */
} CLCapKind;

/* A database driver. The core parses the three database words, checks the
   SQL and the address came from the page and not a visitor, and holds the
   open handle on the page's root environment. The driver only runs SQL.
   Parameters are the values after 'with', in order; each ? in the SQL takes
   the next one. Rows come back as a list of records, one key per column. */
typedef struct CLDatabase {
    const char *name;       /* "SQLite" */
    const char *example;    /* an address as a page writes it: "site.db" */
    const char *where;      /* where that address points: "a file next to the page" */

    /* 1 when this driver opens addresses like this one. Every module is asked;
       the first to say yes gets the address. */
    int   (*claims)(const char *address);

    /* Open and return a handle, or fail. dir is the page's folder, where a
       relative file name is looked for; a server address ignores it. */
    void *(*open)(const CLHost *h, const char *address, const char *dir);
    void  (*close)(void *handle);

    /* Run a query and return its rows, or fail. Run a change and return, or fail. */
    Value (*ask)(const CLHost *h, void *handle, const char *sql, const Value *params);
    void  (*tell)(const CLHost *h, void *handle, const char *sql, const Value *params);
} CLDatabase;

/* A way to fetch addresses on the web. The core parses 'fetch', checks that
   no visitor chose the scheme or the host, counts the requests, adds the keys
   from the site's private cl.keys file, turns a JSON reply into values and
   marks every value as another site's. The module only sends the request. */
typedef struct CLFetchRequest {
    const char *url;               /* http:// or https://, checked by the core */
    const char *host;              /* its host alone, for errors: "api.example.com" */
    const char *body;              /* NULL for a GET; otherwise JSON text to POST */
    const char *const *headers;    /* extra "Name: value" lines, from cl.keys */
    int         nheaders;
    long        seconds;           /* give up after this long, connecting included */
    size_t      max_bytes;         /* fail rather than read a longer reply */
} CLFetchRequest;

/* What came back. The module allocates body (with a 0 after it) and
   content_type; the core hands the reply back to free_reply when done. */
typedef struct CLFetchReply {
    long   status;                 /* 200, 404, ... */
    char  *content_type;           /* "application/json; charset=utf-8", or NULL */
    char  *body;
    size_t len;
} CLFetchReply;

typedef struct CLFetch {
    const char *name;              /* "libcurl" */
    /* Send the request and fill the reply, or fail: no answer, too slow, too
       big. Any HTTP status is a reply; the core decides what it means. */
    void (*fetch)(const CLHost *h, const CLFetchRequest *req, CLFetchReply *reply);
    void (*free_reply)(CLFetchReply *reply);
} CLFetch;

typedef struct CLCapability {
    CLCapKind   kind;
    const void *table;      /* the CLDatabase for CL_CAP_DATABASE, the CLFetch for CL_CAP_FETCH */
} CLCapability;

/* What a module's entry point returns. Static storage: the core keeps the pointer. */
typedef struct CLModule {
    int                 api;        /* CL_MODULE_API the module was built against */
    const char         *name;       /* short and lower case: "sqlite" */
    const char         *library;    /* what it wraps, with its version: "SQLite 3.43.2" */
    const CLCapability *caps;
    int                 ncaps;
} CLModule;

/* ------------------------------------------------------------------ */
/* The entry point. A module defines exactly one:                      */
/*                                                                     */
/*     CL_MODULE(sqlite) { ... return &module; }                       */
/*                                                                     */
/* Compiled into cl, that is the function cl_module_sqlite, which       */
/* modules_load() in the core calls. Built as a shared library with     */
/* -DCL_MODULE_SHARED, it is cl_module_entry, the one name a loader     */
/* would look up. The host pointer stays valid for the whole run.       */
/* ------------------------------------------------------------------ */

typedef const CLModule *(*cl_module_entry_fn)(const CLHost *host);

#ifdef CL_MODULE_SHARED
#  define CL_MODULE(name) const CLModule *cl_module_entry(const CLHost *host)
#else
#  define CL_MODULE(name) const CLModule *cl_module_##name(const CLHost *host)
#endif

/* The modules that can be compiled in. The core calls each one the Makefile linked. */
const CLModule *cl_module_sqlite(const CLHost *host);
const CLModule *cl_module_postgres(const CLHost *host);
const CLModule *cl_module_curl(const CLHost *host);

#endif /* CL_MODULE_H */
