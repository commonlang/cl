/* mod_postgres.c - the Postgres database module for Common.
 *
 * Compiled in when libpq is installed (the Makefile checks for pg_config).
 * Claims addresses that start with postgres:// or postgresql://. Common
 * writes ? for each parameter like SQLite; Postgres numbers them $1, $2 ...,
 * so the SQL is converted outside quotes before it is sent. Values are bound,
 * never pasted. Text read back is marked untrusted. See cl_module.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include "cl_module.h"

static int pg_claims(const char *address) {
    return !strncmp(address, "postgres://", 11) || !strncmp(address, "postgresql://", 13);
}

static void pg_one_line(char *m) { for (; *m; m++) if (*m == '\n') *m = ' '; }

static void *pg_open(const CLHost *h, const char *address, const char *dir) {
    (void)dir;
    PGconn *c = PQconnectdb(address);
    if (PQstatus(c) != CONNECTION_OK) {
        char m[300]; snprintf(m, sizeof m, "%s", PQerrorMessage(c));
        pg_one_line(m);
        PQfinish(c);
        h->fail("cannot reach the Postgres database: %s", m);
    }
    return c;
}

static void pg_close(void *handle) { PQfinish((PGconn *)handle); }

/* Postgres numbers its parameters $1, $2 ...; Common writes ? like SQLite. Convert outside quotes. */
static char *pg_sql(const char *sql, int *count) {
    size_t len = strlen(sql), marks = 0;
    for (const char *p = sql; *p; p++) if (*p == '?') marks++;
    char *out = malloc(len + marks * 11 + 1), *w = out;   /* "$" and up to ten digits for each ? */
    int inq = 0, n = 0;
    for (const char *p = sql; *p; p++) {
        if (*p == '\'') inq = !inq;
        if (*p == '?' && !inq) w += sprintf(w, "$%d", ++n);
        else *w++ = *p;
    }
    *w = 0; *count = n;
    return out;
}

/* Run SQL with the values after 'with' bound as text. Returns the result or fails. */
static PGresult *pg_run(const CLHost *h, PGconn *c, const char *sql, const Value *params) {
    int want; char *q = pg_sql(sql, &want);
    if (want != params->count) {
        free(q);
        h->fail("the query has %d question mark%s but %d value%s came after 'with'", want, want == 1 ? "" : "s",
                params->count, params->count == 1 ? "" : "s");
    }
    const char **vals = malloc(sizeof(char *) * (size_t)(params->count + 1));
    for (int i = 0; i < params->count; i++) {
        const Value *p = &params->items[i];
        if (p->t == V_LIST || p->t == V_RECORD) {
            for (int k = 0; k < i; k++) if (params->items[k].t != V_BOOL) free((char *)vals[k]);
            free(vals); free(q);
            h->fail("can only send text, numbers or yes/no to the database, not %s", h->type_name(p->t));
        }
        vals[i] = p->t == V_BOOL ? (p->n ? "true" : "false") : h->plain_text(p);
    }
    PGresult *r = PQexecParams(c, q, params->count, NULL, vals, NULL, NULL, 0);
    for (int i = 0; i < params->count; i++) if (params->items[i].t != V_BOOL) free((char *)vals[i]);
    free(vals); free(q);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        char m[300]; snprintf(m, sizeof m, "%s", PQresultErrorMessage(r));
        pg_one_line(m);
        PQclear(r);
        h->fail("the database did not understand the query: %s", m);
    }
    return r;
}

static Value pg_ask(const CLHost *h, void *handle, const char *sql, const Value *params) {
    PGresult *r = pg_run(h, (PGconn *)handle, sql, params);
    Value rows = h->list();
    int nr = PQntuples(r), nf = PQnfields(r);
    for (int i = 0; i < nr; i++) {
        Value rec = h->record();
        for (int f = 0; f < nf; f++) {
            Value cv; Oid t = PQftype(r, f);
            if (PQgetisnull(r, i, f)) cv = h->text("");
            else if (t == 20 || t == 21 || t == 23 || t == 700 || t == 701 || t == 1700) cv = h->number(strtod(PQgetvalue(r, i, f), NULL));  /* int8, int2, int4, float4, float8, numeric */
            else if (t == 16) cv = h->yes_no(PQgetvalue(r, i, f)[0] == 't');   /* bool */
            else cv = h->untrusted_text(PQgetvalue(r, i, f));
            h->record_add(&rec, PQfname(r, f), cv);
        }
        h->list_add(&rows, rec);
    }
    PQclear(r);
    return rows;
}

static void pg_tell(const CLHost *h, void *handle, const char *sql, const Value *params) {
    PGresult *r = pg_run(h, (PGconn *)handle, sql, params);
    PQclear(r);
}

static const CLDatabase pg_driver = {
    "Postgres", "postgres://user:password@host/dbname", "a Postgres server",
    pg_claims, pg_open, pg_close, pg_ask, pg_tell
};
static const CLCapability pg_caps[] = { { CL_CAP_DATABASE, &pg_driver } };
static CLModule pg_module = { CL_MODULE_API, "postgres", "libpq", pg_caps, 1 };
static char pg_library[64];

CL_MODULE(postgres) {
    (void)host;
    int v = PQlibVersion();      /* 160004 is 16.4; before 10 it was 90605 for 9.6.5 */
    if (v >= 100000) snprintf(pg_library, sizeof pg_library, "libpq %d.%d", v / 10000, v % 10000);
    else snprintf(pg_library, sizeof pg_library, "libpq %d.%d.%d", v / 10000, (v / 100) % 100, v % 100);
    pg_module.library = pg_library;
    return &pg_module;
}
