/* mod_sqlite.c - the SQLite database module for Common.
 *
 * Compiled in when sqlite3.h is installed (the Makefile checks). Opens a
 * database file next to the page and runs the SQL from 'ask database' and
 * 'tell database' with the values after 'with' bound as parameters, never
 * pasted into the SQL. Text read back is marked untrusted: a visitor may have
 * written it. See cl_module.h for the rules a module follows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "cl_module.h"

static int sq_claims(const char *address) {
    return strstr(address, "://") == NULL;      /* a file name; server addresses belong to other modules */
}

static void *sq_open(const CLHost *h, const char *address, const char *dir) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, address);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        char m[256]; snprintf(m, sizeof m, "%s", db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        h->fail("cannot open the database \"%s\" (looked next to the page): %s", address, m);
    }
    sqlite3_busy_timeout(db, 3000);     /* another request is writing: wait up to 3 s instead of failing */
    return db;
}

static void sq_close(void *handle) { sqlite3_close((sqlite3 *)handle); }

/* Prepare SQL and bind the values after 'with'. */
static sqlite3_stmt *sq_prepare(const CLHost *h, sqlite3 *db, const char *sql, const Value *params) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        char m[256]; snprintf(m, sizeof m, "%s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        h->fail("the database did not understand the query: %s", m);
    }
    int want = sqlite3_bind_parameter_count(st);
    if (want != params->count) {
        sqlite3_finalize(st);
        h->fail("the query has %d question mark%s but %d value%s came after 'with'", want, want == 1 ? "" : "s",
                params->count, params->count == 1 ? "" : "s");
    }
    for (int i = 0; i < params->count; i++) {
        const Value *p = &params->items[i];
        if (p->t == V_TEXT)      { char *t = h->plain_text(p); sqlite3_bind_text(st, i + 1, t, -1, SQLITE_TRANSIENT); free(t); }
        else if (p->t == V_NUM)  { if (p->n == (double)(long long)p->n) sqlite3_bind_int64(st, i + 1, (long long)p->n); else sqlite3_bind_double(st, i + 1, p->n); }
        else if (p->t == V_BOOL) sqlite3_bind_int(st, i + 1, p->n != 0);
        else { sqlite3_finalize(st); h->fail("can only send text, numbers or yes/no to the database, not %s", h->type_name(p->t)); }
    }
    return st;
}

static Value sq_ask(const CLHost *h, void *handle, const char *sql, const Value *params) {
    sqlite3 *db = (sqlite3 *)handle;
    sqlite3_stmt *st = sq_prepare(h, db, sql, params);
    Value rows = h->list();
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        Value r = h->record();
        int nc = sqlite3_column_count(st);
        for (int c = 0; c < nc; c++) {
            Value cv;
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER: cv = h->number((double)sqlite3_column_int64(st, c)); break;
                case SQLITE_FLOAT:   cv = h->number(sqlite3_column_double(st, c)); break;
                case SQLITE_NULL:    cv = h->text(""); break;
                default: {
                    const unsigned char *t = sqlite3_column_text(st, c);
                    cv = h->untrusted_text(t ? (const char *)t : "");   /* stored text may have come from a visitor */
                }
            }
            h->record_add(&r, sqlite3_column_name(st, c), cv);
        }
        h->list_add(&rows, r);
    }
    if (rc != SQLITE_DONE) {
        char m[256]; snprintf(m, sizeof m, "%s", sqlite3_errmsg(db));
        sqlite3_finalize(st); h->free_value(&rows);
        h->fail("the database stopped: %s", m);
    }
    sqlite3_finalize(st);
    return rows;
}

static void sq_tell(const CLHost *h, void *handle, const char *sql, const Value *params) {
    sqlite3 *db = (sqlite3 *)handle;
    sqlite3_stmt *st = sq_prepare(h, db, sql, params);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { }
    if (rc != SQLITE_DONE) {
        char m[256]; snprintf(m, sizeof m, "%s", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        h->fail("the database refused: %s", m);
    }
    sqlite3_finalize(st);
}

static const CLDatabase sq_driver = {
    "SQLite", "site.db", "a file next to the page",
    sq_claims, sq_open, sq_close, sq_ask, sq_tell
};
static const CLCapability sq_caps[] = { { CL_CAP_DATABASE, &sq_driver } };
static CLModule sq_module = { CL_MODULE_API, "sqlite", "SQLite", sq_caps, 1 };
static char sq_library[64];

CL_MODULE(sqlite) {
    (void)host;
    snprintf(sq_library, sizeof sq_library, "SQLite %s", sqlite3_libversion());
    sq_module.library = sq_library;
    return &sq_module;
}
