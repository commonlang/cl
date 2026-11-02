/*
 * Common (CL): a programming language for the web, written in plain English.
 * https://commonlang.org  ·  Apache License 2.0 (see LICENSE and NOTICE)
 *
 * The reference interpreter, development server and CGI program, in one C11
 * file. It works like PHP: Common code lives in HTML pages inside <cl> ... </cl>
 * tags, and each block is replaced by its output on the server.
 *
 *   cl page.html [query] [form] [path] [cookie]   run a page, print the HTML
 *   cl serve [port] [dir]                         serve a folder over HTTP
 *   cl cgi                                        run behind a web server
 *   cl words [--markdown]                         every word of the language
 *   cl version
 *
 * Build: make (SQLite and Postgres are added when installed), or by hand:
 *   cc -O2 -o cl src/cl.c   (add -lws2_32 on Windows; no database words)
 *   cc -O2 -DCL_SQLITE -o cl src/cl.c src/mod_sqlite.c -lsqlite3
 *
 * Libraries live in modules, src/mod_*.c, behind the small interface in
 * src/cl_module.h. This file is the language: it never includes a library.
 *
 * The vocabulary is the DICTIONARY table below: one accepted form for every
 * word. docs/dictionary.md explains each one, and SYNONYMS turns words from
 * other languages into errors that name the right word.
 *
 * How it runs: a page is cut into lines; each line is split into tokens the
 * first time it runs and kept; a recursive-descent parser reads the tokens
 * and evaluates as it goes (there is no syntax tree). Text holes, "Hello
 * [name]", are stored between HOLE_OPEN and HOLE_CLOSE and filled when used.
 *
 * Safety: values from a visitor (query, form, path, remembered, database
 * text) are marked untrusted, and text built from them tainted. They are
 * HTML-escaped when shown, and refused in SQL text, file names, off-site
 * go-to addresses and email recipients. Sessions are an HMAC-SHA256 signed
 * cookie; passwords are PBKDF2-HMAC-SHA256.
 *
 * Errors are a longjmp (fail). Anything a frame holds across a fail is noted
 * in small registries (g_live, g_held, g_bufs, g_frames) so run_block can
 * free it: see "Checking for leaks" in CONTRIBUTING.md.
 */

#define CL_VERSION "0.5.0"
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE             /* glibc: realpath; gcc 14 refuses implicit declarations */
#define _CRT_RAND_S                 /* Windows: rand_s, the system's secure random numbers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "cl_module.h"      /* Value, and the interface the database modules are behind */

/* ------------------------------------------------------------------ */
/* Platform: the server needs sockets, which differ on Windows.        */
/* Everything else is plain C11.                                       */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#  include <winsock2.h>
#  include <direct.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET sock_t;
#  define sock_close(s)        closesocket(s)
#  define sock_read(s, b, n)   recv((s), (b), (int)(n), 0)
#  define sock_write(s, b, n)  send((s), (b), (int)(n), 0)
#  define sock_valid(s)        ((s) != INVALID_SOCKET)
#  define sock_opt_ptr(p)      ((const char *)(p))
#  ifndef S_ISREG
#    define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#    define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#  endif
#else
#  include <unistd.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <sys/resource.h>      /* getrlimit: how much C stack a page may use, see stack_check */
   typedef int sock_t;
#  define sock_close(s)        close(s)
#  define sock_read(s, b, n)   read((s), (b), (n))
#  define sock_write(s, b, n)  write((s), (b), (n))
#  define sock_valid(s)        ((s) >= 0)
#  define sock_opt_ptr(p)      (p)
#endif
/* Helpers with big buffers that the block runner calls once. If the compiler folded
   them into exec_lines_inner, every nested block would pay for their stack space. */
#if defined(__GNUC__) || defined(__clang__)
#  define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#else
#  define NOINLINE
#endif

/* Practice code from 'try' gets a memory budget; every allocation of text and
   lists counts against it while it runs (see budget(), after fail()). */
static void budget(size_t n);

/* Our own copies of strdup and strndup, so no platform has to provide them. */
static char *xstrndup(const char *s, size_t n) {
    size_t len = 0; while (len < n && s[len]) len++;
    budget(len + 1);
    char *d = malloc(len + 1); memcpy(d, s, len); d[len] = 0; return d;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

/* The folder part of a path, with / or \ separators. "." if there is none. */
static void dir_of(const char *path, char *out, size_t n) {
    size_t len = strlen(path), i = len;
    while (i > 0 && path[i-1] != '/' && path[i-1] != '\\') i--;
    if (i == 0) { snprintf(out, n, "."); return; }
    if (i > 1) i--;                          /* drop the separator, keep a lone root "/" */
    if (i >= n) i = n - 1;
    memcpy(out, path, i); out[i] = 0;
}

/* ================================================================== */
/* Text buffer                                                         */
/* ================================================================== */

typedef struct { char *data; size_t len, cap; } Buf;

static Buf *buf_new(void) {
    Buf *b = malloc(sizeof *b);
    budget(256); b->cap = 256; b->len = 0;
    b->data = malloc(b->cap); b->data[0] = 0;
    return b;
}
static void buf_free(Buf *b) { free(b->data); free(b); }
static void buf_putn(Buf *b, const char *s, size_t n) {
    while (b->len + n + 1 > b->cap) { budget(b->cap); b->cap *= 2; b->data = realloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
static void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_putc(Buf *b, char c)        { buf_putn(b, &c, 1); }
/* Escape text for HTML. A visitor's value may land inside an attribute in
   either kind of quote, so apos escapes ' too; text we write between tags
   ourselves (errors, code samples) does not need it. */
static void buf_puts_esc(Buf *b, const char *s, int apos) {
    for (; *s; s++) {
        switch (*s) {
            case '<':  buf_puts(b, "&lt;");   break;
            case '>':  buf_puts(b, "&gt;");   break;
            case '&':  buf_puts(b, "&amp;");  break;
            case '"':  buf_puts(b, "&quot;"); break;
            case '\'': if (apos) buf_puts(b, "&#39;"); else buf_putc(b, *s); break;
            default:   buf_putc(b, *s);
        }
    }
}
static void buf_puts_html(Buf *b, const char *s) { buf_puts_esc(b, s, 1); }
static void buf_puts_text(Buf *b, const char *s) { buf_puts_esc(b, s, 0); }

/* ================================================================== */
/* Errors                                                              */
/* ================================================================== */

static jmp_buf err_jmp;
static char    err_msg[1024];
static int     cur_line;

__attribute__((noreturn)) static void fail(const char *fmt, ...) {
    char m[900];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    snprintf(err_msg, sizeof err_msg, "Line %d: %s", cur_line, m);
    longjmp(err_jmp, 1);
}

static int    g_budget_on;      /* set while practice code runs */
static size_t g_budget_left;
static void budget(size_t n) {
    if (!g_budget_on) return;
    if (n > g_budget_left) {
        g_budget_on = 0;        /* the cleanup after the error may allocate */
        fail("the practice code used too much memory (more than 64 MB). Is a text or list growing in a loop?");
    }
    g_budget_left -= n;
}

/* ================================================================== */
/* The dictionary: every word Common knows                             */
/* ================================================================== */

typedef struct { const char *word; const char *form; const char *meaning; const char *example; int cmd; } Word;

static const Word DICTIONARY[] = {
    /* the one piece of syntax: brackets put a value into text. Not a word, so it can never clash with a name */
    { "[ ]", "\"text with [a value] inside\"", "put a value into text. A value is anything you could write after 'set x to': a name, math, count of, query. Write \\[ for a real bracket", "set name to \"Ada\"\nshow \"Hello, [name]! You have [count of messages] messages.\"", 0 },
    /* commands: the first word of a line */
    { "show", "show VALUE  /  show ... end", "show a value on the page (visitor text is made safe). On its own, show opens a block of text that runs until a line saying end", "show \"Hello, [name]!\"", 1 },
    { "raw", "show raw VALUE", "show text exactly as it is, with no safety", "show raw \"<b>bold</b>\"", 0 },
    { "code", "show code TEXT", "show Common code highlighted, the way a cl-code block does, but from a value", "show code example of entry", 0 },
    { "set", "set NAME to VALUE", "create or replace a variable; VALUE, VALUE makes a list. 'set COLUMN of RECORD to VALUE' changes one column", "set price to 4.5", 1 },
    { "add", "add VALUE to LIST", "put a value at the end of a list", "add \"cherry\" to fruits", 1 },
    { "if", "if CONDITION", "run the lines below when the condition is yes", "if age is at least 18\n  show \"Welcome\"\nend", 1 },
    { "otherwise", "otherwise [if CONDITION]", "what to run when the condition above was no", "if paid\n  show \"Thanks\"\notherwise\n  show \"Pay first\"\nend", 1 },
    { "end", "end", "close an if, for each, repeat, to, send email, record or text block", "repeat 3 times\n  show \"hip\"\nend", 1 },
    { "for", "for each NAME in LIST", "run the lines below once for every item", "for each fruit in fruits\n  show \"<li>[fruit]</li>\"\nend", 1 },
    { "repeat", "repeat NUMBER times [until CONDITION]", "run the lines below that many times, or fewer with 'until CONDITION': stop as soon as it is yes", "repeat 5 times\n  show \"★\"\nend", 1 },
    { "include", "include \"file.html\"", "insert another page here. A name starting with / starts from the site's top folder: include \"/header.html\"", "include \"header.html\"", 1 },
    { "open", "open database \"file.db\"", "open a SQLite file next to the page, or a Postgres server with postgres://user:password@host/dbname", "open database \"site.db\"", 1 },
    { "tell", "tell database \"SQL\" with VALUE, ...", "run SQL that changes the database (insert, update, delete)", "tell database \"insert into notes (text) values (?)\" with note", 1 },
    { "go", "go to \"/page.html\"", "send the visitor to another page and stop", "go to \"/thanks\"", 1 },
    { "remember", "remember \"name\" as VALUE", "keep a value for this visitor until they forget it (a signed cookie)", "remember \"user\" as name", 1 },
    { "forget", "forget \"name\"  /  forget everything", "drop a remembered value, or all of them (sign out)", "forget everything", 1 },
    { "send", "send email to \"you@site.com\"  then subject:, body:, reply to:, from: lines, then end", "send an email to an address you wrote yourself (never a visitor's). While developing it is saved in the private outbox/ folder; behind a web server it goes through sendmail. At most 3 per page and 20 an hour", "send email to \"owner@example.com\"\n  subject: \"Message from [name]\"\n  reply to: email\n  body: message\nend", 1 },
    /* values */
    { "yes", "yes", "the answer yes", "set paid to yes", 0 },
    { "no", "no", "the answer no", "set done to no", 0 },
    { "count", "count of LIST", "how many items a list has, or letters in text", "show \"[count of fruits] fruits\"", 0 },
    { "first", "first of LIST  /  first N of LIST", "the first item, or the first N items (or letters of text)", "show first of fruits\nshow \"[first 100 of body]…\"", 0 },
    { "last", "last of LIST  /  last N of LIST", "the last item, or the last N items (or letters of text)", "show last of fruits\nfor each post in last 3 of posts", 0 },
    { "item", "item N of LIST", "one item by position, counting from 1; or a column by name: item \"price\" of lamp", "show item 2 of fruits", 0 },
    { "query", "query \"name\"", "what the visitor typed in the URL after ?name=", "set name to query \"name\"", 0 },
    { "path", "path", "the URL path, like /about. The visitor typed it, so it is made safe like query", "set slug to last of split path by \"/\"", 0 },
    { "try", "try TEXT", "run text as Common in a sandbox and give back what it said. No files, database, cookies, redirects, email or password hashing; loops stop after 20000 steps and memory at 64 MB. For practice pages", "set result to try form \"code\"", 0 },
    { "dictionary", "dictionary", "every word of the language as records: word, form, meaning, example", "for each entry in dictionary\n  show \"<dt>[word of entry]</dt><dd>[meaning of entry]</dd>\"\nend", 0 },
    { "form", "form \"field\"", "what the visitor typed in a submitted form (POST)", "set message to trim of form \"message\"", 0 },
    { "remembered", "remembered \"name\"", "a value kept for this visitor, or empty text", "if remembered \"user\" is empty\n  go to \"/login\"\nend", 0 },
    { "hash", "hash of TEXT", "a one-way scramble of a password, safe to store (PBKDF2-HMAC-SHA256)", "tell database \"insert into users (name, password) values (?, ?)\" with name, hash of form \"password\"", 0 },
    { "matches", "TEXT matches HASH", "yes if the text is the password behind the hash", "if form \"password\" matches password of user\n  remember \"user\" as name\nend", 0 },
    { "email", "TEXT is an email", "yes if the text is one email address, like ada@example.com. Check a visitor's address with it before you use it", "if form \"email\" is not an email\n  set problem to \"Please check your email address.\"\nend", 0 },
    { "contains", "A contains B", "yes if text A has B inside it, or list A has item B", "if email contains \"@\"", 0 },
    { "sort", "sort of LIST", "the list in order (numbers by size, text A to Z)", "for each name in sort of names", 0 },
    { "reverse", "reverse of LIST", "the list back to front", "for each post in reverse of posts", 0 },
    { "date", "date of TEXT", "a date like 2026-10-24 written out: 24 October 2026", "show \"[date of today]\"", 0 },
    { "year", "year of TEXT", "the year of a date as a number", "show \"[year of today]\"", 0 },
    { "month", "month of TEXT", "the month of a date as a name", "show \"[month of today]\"", 0 },
    { "day", "day of TEXT", "the day of a date as a number", "show \"[day of today]\"", 0 },
    { "today", "today", "the date, like 2026-10-24", "show \"Today is [today]\"", 0 },
    { "now", "now", "the date and time, like 2026-10-24 18:30", "tell database \"insert into log (at) values (?)\" with now", 0 },
    { "number", "number of TEXT", "the text as a number, so math works on it", "set qty to number of query \"qty\"", 0 },
    { "list", "empty list", "a list with nothing in it yet", "set names to empty list", 0 },
    { "uppercase", "uppercase of TEXT", "the text in capitals", "show \"[uppercase of name]\"", 0 },
    { "lowercase", "lowercase of TEXT", "the text in small letters", "set slug to lowercase of title", 0 },
    { "trim", "trim of TEXT", "the text without spaces at the ends", "set name to trim of form \"name\"", 0 },
    { "paragraphs", "paragraphs of TEXT", "a list of the paragraphs (separated by blank lines)", "for each paragraph in paragraphs of body\n  show \"<p>[paragraph]</p>\"\nend", 0 },
    { "split", "split TEXT by \"/\"", "a list of the pieces between each separator", "set parts to split \"2026-10-24\" by \"-\"", 0 },
    { "replace", "replace \"a\" with \"b\" in TEXT", "the text with every a changed to b", "set slug to replace \" \" with \"-\" in title", 0 },
    { "ask", "ask database \"SQL\" with VALUE, ...", "run a query; each ? in the SQL takes the next value safely. Gives a list of records", "set posts to ask database \"select * from posts where slug = ?\" with slug", 0 },
    { "of", "COLUMN of RECORD", "one column of a record, like name of recipe. Any column name works", "show \"[title of recipe]\"", 0 },
    /* conditions */
    { "is", "A is B", "equal. Also: is not, is more than, is less than, is at least, is at most, is between A and B, is empty", "if colour is \"blue\"\nif age is more than 12\nif name is empty", 0 },
    { "and", "A and B", "both must be yes", "if name is not empty and age is at least 18", 0 },
    { "or", "A or B", "either can be yes", "if paid or member", 0 },
    { "not", "not A", "flip yes and no", "if not done", 0 },
    /* words that only appear inside the forms above */
    { "to", "to NAME ARG ...  then lines, then end", "define a verb: your own command. Call it by name with one value per ARG. Inside, 'give VALUE' hands a value back", "to greet someone\n  show \"<p>Hello, [someone]!</p>\"\nend\n\ngreet \"Ada\"", 1 },
    { "give", "give VALUE", "inside a verb: hand this value back and finish. A verb that gives can be used wherever a value goes", "to slug_of heading\n  give replace \" \" with \"-\" in lowercase of heading\nend\n\nshow \"[slug_of title]\"", 1 },
    { "record", "set NAME to record  then column: VALUE lines, then end", "a record of your own, like a row you made up. Read a column with 'price of lamp', change one with 'set price of lamp to 15'", "set lamp to record\n  name: \"Desk lamp\"\n  price: 19\nend\nshow \"[name of lamp] costs $[price of lamp]\"", 0 },
    { "until", 0, 0, 0, 0 }, { "each", 0, 0, 0, 0 }, { "in", 0, 0, 0, 0 }, { "times", 0, 0, 0, 0 },
    { "database", 0, 0, 0, 0 }, { "with", 0, 0, 0, 0 }, { "by", 0, 0, 0, 0 }, { "as", 0, 0, 0, 0 }, { "everything", 0, 0, 0, 0 },
    { "the", 0, 0, 0, 0 }, { "between", 0, 0, 0, 0 }, { "an", 0, 0, 0, 0 },
    { "than", 0, 0, 0, 0 }, { "at", 0, 0, 0, 0 }, { "least", 0, 0, 0, 0 }, { "most", 0, 0, 0, 0 },
    { "less", 0, 0, 0, 0 }, { "more", 0, 0, 0, 0 }, { "empty", 0, 0, 0, 0 },
    /* reserved for the future */
    { "page", 0, 0, 0, 0 }, { "text", 0, 0, 0, 0 },
};
#define NWORDS (sizeof DICTIONARY / sizeof DICTIONARY[0])

/* Common guesses from other languages, and the Common word to use instead. */
static const char *SYNONYMS[][2] = {
    { "print", "show" }, { "echo", "show" }, { "say", "show" }, { "write", "show" }, { "display", "show" },
    { "let", "set" }, { "make", "set" }, { "var", "set" }, { "define", "set" },
    { "else", "otherwise" }, { "elif", "otherwise if" }, { "elseif", "otherwise if" },
    { "then", "nothing: just start the next line" },
    { "while", "repeat N times, or for each" }, { "loop", "repeat N times, or for each" }, { "foreach", "for each" },
    { "length", "count" }, { "size", "count" }, { "len", "count" },
    { "push", "add VALUE to LIST" }, { "append", "add VALUE to LIST" },
    { "bigger", "more" }, { "greater", "more" }, { "larger", "more" }, { "smaller", "less" }, { "lower", "less" },
    { "equals", "is" }, { "equal", "is" }, { "says", "is" }, { "eq", "is" }, { "isnt", "is not" }, { "true", "yes" }, { "false", "no" }, { "null", "empty" }, { "nil", "empty" },
    { "require", "include" }, { "import", "include" }, { "get", "query" }, { "param", "query" }, { "post", "form" },
    { "redirect", "go to" }, { "header", "go to" }, { "int", "number of" }, { "float", "number of" }, { "parse", "number of" },
    { "upper", "uppercase of" }, { "lower", "lowercase of" }, { "strip", "trim of" }, { "explode", "split ... by" }, { "join", "[a] [b] inside text" },
    { "insert", "tell database" }, { "update", "tell database" }, { "delete", "tell database" }, { "execute", "tell database" },
    { "session", "remember / remembered" }, { "cookie", "remember / remembered" }, { "login", "remember \"user\" as name" }, { "logout", "forget everything" },
    { "password_hash", "hash of" }, { "bcrypt", "hash of" }, { "md5", "hash of" }, { "password_verify", "matches" },
    { "sorted", "sort of" }, { "reversed", "reverse of" }, { "substr", "first N of" }, { "substring", "first N of" }, { "slice", "first N of" },
    { "mail", "send email" }, { "sendmail", "send email" }, { "smtp", "send email" },
    { "includes", "contains" }, { "in_array", "contains" }, { "strpos", "contains" }, { "strftime", "date of" },
};
#define NSYN (sizeof SYNONYMS / sizeof SYNONYMS[0])

/* Words that are only ever used as "WORD of X" (or split X by, replace A with B in X).
   They may also be plain variable names, because the shape tells them apart. */
static int is_soft_word(const char *w) {
    static const char *soft[] = { "count", "first", "last", "item", "number", "date", "year", "month", "day", "hash",
                                  "sort", "reverse", "uppercase", "lowercase", "trim", "paragraphs", "split", "replace", "email", NULL };
    for (int i = 0; soft[i]; i++) if (strcmp(soft[i], w) == 0) return 1;
    return 0;
}

/* Dictionary lookups happen for every name a page uses, so they go through a
   small hash table (open addressing, built the first time it is needed). */
static unsigned word_hash(const char *w) { unsigned h = 2166136261u; while (*w) h = (h ^ (unsigned char)*w++) * 16777619u; return h; }
static const Word *dict_find(const char *w) {
    static const Word *table[512]; static int built;
    if (!built) {
        for (size_t i = 0; i < NWORDS; i++) {
            unsigned h = word_hash(DICTIONARY[i].word) & 511;
            while (table[h]) h = (h + 1) & 511;
            table[h] = &DICTIONARY[i];
        }
        built = 1;
    }
    for (unsigned h = word_hash(w) & 511; table[h]; h = (h + 1) & 511)
        if (strcmp(table[h]->word, w) == 0) return table[h];
    return NULL;
}
static const char *synonym(const char *w) {
    for (size_t i = 0; i < NSYN; i++) if (strcmp(SYNONYMS[i][0], w) == 0) return SYNONYMS[i][1];
    return NULL;
}

static int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (size_t i = 0; i <= la; i++) d[i][0] = (int)i;
    for (size_t j = 0; j <= lb; j++) d[0][j] = (int)j;
    for (size_t i = 1; i <= la; i++)
        for (size_t j = 1; j <= lb; j++) {
            int c = a[i-1] == b[j-1] ? 0 : 1;
            int m = d[i-1][j] + 1;
            if (d[i][j-1] + 1 < m) m = d[i][j-1] + 1;
            if (d[i-1][j-1] + c < m) m = d[i-1][j-1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}

/* The closest dictionary word (commands only if cmd_only) within 2 edits, or NULL. */
static const char *nearest_word(const char *w, int cmd_only) {
    const char *best = NULL; int bd = 3;
    for (size_t i = 0; i < NWORDS; i++) {
        if (!DICTIONARY[i].form || (cmd_only && !DICTIONARY[i].cmd)) continue;
        int d = edit_distance(w, DICTIONARY[i].word);
        if (d < bd) { bd = d; best = DICTIONARY[i].word; }
    }
    return best;
}

static void lowercase(const char *s, char *out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

/* Markdown for one table cell: | would end the cell, a new line would end the row. */
static void md_cell(const char *s) {
    for (; *s; s++) {
        if (*s == '|') fputs("\\|", stdout);
        else if (*s == '\n') fputs(" ", stdout);
        else putchar(*s);
    }
}

/* `cl words --markdown`: the whole vocabulary as Markdown, for AI agents and
   docs. Every word is listed whatever this build has, so the output is the
   same on every machine. */
static void print_words_markdown(void) {
    printf("## Every word\n\n");
    printf("Generated by `cl words --markdown` from the interpreter's own dictionary (Common %s). "
           "Each word has exactly one form. Anything not listed here is an error.\n\n", CL_VERSION);
    for (int pass = 0; pass < 2; pass++) {
        printf(pass == 0 ? "### Commands: the first word of a line\n\n" : "### Values, conditions and the rest\n\n");
        printf("| Form | Meaning |\n|---|---|\n");
        for (size_t i = 0; i < NWORDS; i++) {
            const Word *w = &DICTIONARY[i];
            if (!w->form || (pass == 0) != (w->cmd == 1)) continue;
            printf("| `"); md_cell(w->form); printf("` | "); md_cell(w->meaning); printf(" |\n");
        }
        printf("\n");
    }
    printf("Also: `+ - * / ( )` for math (numbers only), `# comment` to the end of a line, and `the` is silent.\n\n");
    printf("### Words that cannot be names\n\nA name cannot be one of these. Any other word made of letters, digits and `_` is fine, "
           "including the words only used before `of` (`count`, `first`, `date`, `email`, ...).\n\n");
    int n = 0;
    for (size_t i = 0; i < NWORDS; i++) {
        if (is_soft_word(DICTIONARY[i].word) || !isalpha((unsigned char)DICTIONARY[i].word[0])) continue;
        printf("%s`%s`", n++ ? " " : "", DICTIONARY[i].word);
    }
    printf("\n\n");
    printf("### Examples, one per word\n\n```\n");
    for (size_t i = 0; i < NWORDS; i++) {
        const Word *w = &DICTIONARY[i];
        if (!w->example) continue;
        printf("# %s\n%s\n\n", w->word, w->example);
    }
    printf("```\n\n");
    printf("### Words from other languages\n\nCommon refuses these and names the word to use instead.\n\n");
    printf("| You might write | Common |\n|---|---|\n");
    for (size_t i = 0; i < NSYN; i++) {
        printf("| `"); md_cell(SYNONYMS[i][0]); printf("` | "); md_cell(SYNONYMS[i][1]); printf(" |\n");
    }
}

static int db_available(void);
static void print_modules(void);
static void print_words(void) {
    printf("Common (CL) %s. One form each. Details: docs/dictionary.md\n\n", CL_VERSION);
    int has_db = db_available();
    for (size_t i = 0; i < NWORDS; i++) {
        const Word *w = &DICTIONARY[i];
        if (!w->form) continue;
        if (!has_db && (!strcmp(w->word, "open") || !strcmp(w->word, "ask") || !strcmp(w->word, "tell") || !strcmp(w->word, "of"))) continue;
        printf("  %-28s %s\n", w->form, w->meaning);
    }
    if (!has_db) printf("  (database words are off: this build has no database module. Install SQLite and run make again)\n");
    printf("\nAlso: \"text in quotes with [values] inside\", + - * / ( ) for math, # for comments.\n");
    print_modules();
}

/* ================================================================== */
/* Values: text, numbers, yes/no, lists                                */
/* The Value struct itself is in cl_module.h, shared with the modules. */
/* ================================================================== */

static Value v_text(const char *s) { Value v = { V_TEXT, xstrdup(s), 0, NULL, NULL, 0, 0, 0, 0 }; return v; }
static Value v_num(double d)       { Value v = { V_NUM,  NULL, d, NULL, NULL, 0, 0, 0, 0 };     return v; }
static Value v_bool(int b)         { Value v = { V_BOOL, NULL, b ? 1 : 0, NULL, NULL, 0, 0, 0, 0 }; return v; }
static Value v_list(void)          { Value v = { V_LIST, NULL, 0, NULL, NULL, 0, 0, 0, 0 };   return v; }
static Value v_record(void)        { Value v = { V_RECORD, NULL, 0, NULL, NULL, 0, 0, 0, 0 }; return v; }

static void v_free(Value *v) {
    free(v->s);
    for (int i = 0; i < v->count; i++) { v_free(&v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items); free(v->keys);
    memset(v, 0, sizeof *v);
}

static Value v_copy(const Value *v) {
    Value c = *v;
    if (v->s) c.s = xstrdup(v->s);
    if (v->items) {
        budget(sizeof(Value) * (size_t)v->cap);
        c.items = malloc(sizeof(Value) * (size_t)v->cap);
        for (int i = 0; i < v->count; i++) c.items[i] = v_copy(&v->items[i]);
    }
    if (v->keys) {
        budget(sizeof(char *) * (size_t)v->cap);
        c.keys = malloc(sizeof(char *) * (size_t)v->cap);
        for (int i = 0; i < v->count; i++) c.keys[i] = xstrdup(v->keys[i]);
    }
    return c;
}

static void list_add(Value *l, Value item) {
    if (l->count == l->cap) {
        budget(sizeof(Value) * (size_t)(l->cap ? l->cap : 4));
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = realloc(l->items, sizeof(Value) * (size_t)l->cap);
    }
    l->items[l->count++] = item;
}

static void record_add(Value *r, const char *key, Value item) {
    if (r->count == r->cap) {
        budget((sizeof(Value) + sizeof(char *)) * (size_t)(r->cap ? r->cap : 4));
        r->cap = r->cap ? r->cap * 2 : 4;
        r->items = realloc(r->items, sizeof(Value) * (size_t)r->cap);
        r->keys  = realloc(r->keys,  sizeof(char *) * (size_t)r->cap);
    }
    r->keys[r->count] = xstrdup(key);
    r->items[r->count++] = item;
}

static const char *type_name(VType t) {
    switch (t) {
        case V_TEXT:   return "text";
        case V_NUM:    return "a number";
        case V_BOOL:   return "yes or no";
        case V_RECORD: return "a record";
        default:       return "a list";
    }
}

/* Text form of any value. Caller frees. */
static char *v_to_text(const Value *v) {
    char tmp[64];
    switch (v->t) {
        case V_TEXT: return xstrdup(v->s);
        case V_NUM:  snprintf(tmp, sizeof tmp, "%.15g", v->n); return xstrdup(tmp);
        case V_BOOL: return xstrdup(v->n ? "yes" : "no");
        default: {
            Buf *b = buf_new();
            for (int i = 0; i < v->count; i++) {
                if (i) buf_puts(b, ", ");
                if (v->t == V_RECORD) { buf_puts(b, v->keys[i]); buf_puts(b, ": "); }
                char *s = v_to_text(&v->items[i]); buf_puts(b, s); free(s);
            }
            char *r = xstrdup(b->data); buf_free(b); return r;
        }
    }
}

/* Text form for showing on a page: untrusted text is HTML-escaped. Caller frees. */
static char *v_to_safe_text(const Value *v) {
    if (v->t == V_LIST || v->t == V_RECORD) {
        Buf *b = buf_new();
        for (int i = 0; i < v->count; i++) {
            if (i) buf_puts(b, ", ");
            if (v->t == V_RECORD) { buf_puts(b, v->keys[i]); buf_puts(b, ": "); }
            char *s = v_to_safe_text(&v->items[i]); buf_puts(b, s); free(s);
        }
        char *r = xstrdup(b->data); buf_free(b); return r;
    }
    if (v->t == V_TEXT && v->untrusted) {
        Buf *b = buf_new(); buf_puts_html(b, v->s);
        char *r = xstrdup(b->data); buf_free(b); return r;
    }
    return v_to_text(v);
}

/* Whether any part of a value came from a visitor: raw visitor text (untrusted)
   or text with a visitor's value filled in (tainted). Lists are searched too. */
static void v_taint_of(const Value *v, int *untrusted, int *tainted) {
    if (v->untrusted) *untrusted = 1;
    if (v->tainted)   *tainted = 1;
    for (int i = 0; i < v->count; i++) v_taint_of(&v->items[i], untrusted, tainted);
}
/* Give dst everything src knows about the visitor. Every word that builds a
   value out of another one calls this, so the safety rules survive it. */
static void v_carry(Value *dst, const Value *src) {
    int u = 0, t = 0; v_taint_of(src, &u, &t);
    dst->untrusted |= u; dst->tainted |= t;
}
/* Turn v into text in place, keeping what it knows about the visitor. */
static void v_make_text(Value *v) {
    if (v->t == V_TEXT) return;
    char *s = v_to_text(v); Value t = v_text(s); free(s);
    v_carry(&t, v); v_free(v); *v = t;
}

static int v_truthy(const Value *v) {
    switch (v->t) {
        case V_TEXT: return v->s[0] != 0;
        case V_LIST: case V_RECORD: return v->count > 0;
        default:     return v->n != 0;
    }
}

/* Text that is written as a number, like "12" or " 4.5 ". Visitors only ever send text. */
static int text_number(const Value *v, double *out) {
    if (v->t != V_TEXT) return 0;
    const char *q = v->s; while (isspace((unsigned char)*q)) q++;
    if (!*q || !strchr("0123456789+-.", *q)) return 0;
    for (const char *c = q; *c; c++) if (!strchr("0123456789+-.eE \t\r\n", *c)) return 0;   /* no hex, inf or nan */
    char *end; double d = strtod(q, &end);
    if (end == q) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    *out = d; return 1;
}
static int as_number(const Value *v, double *out) {
    if (v->t == V_NUM) { *out = v->n; return 1; }
    return text_number(v, out);
}

static int v_equal(const Value *a, const Value *b) {
    if (a->t == V_LIST || b->t == V_LIST || a->t == V_RECORD || b->t == V_RECORD) {
        if (a->t != b->t || a->count != b->count) return 0;
        for (int i = 0; i < a->count; i++) if (!v_equal(&a->items[i], &b->items[i])) return 0;
        return 1;
    }
    if (a->t == b->t && a->t != V_TEXT) return a->n == b->n;
    double x1, y1;                                           /* query "n" is 3, when n=3.0 */
    if ((a->t == V_NUM || b->t == V_NUM) && as_number(a, &x1) && as_number(b, &y1)) return x1 == y1;
    char *x = v_to_text(a), *y = v_to_text(b);
    int r = strcmp(x, y) == 0;
    free(x); free(y);
    return r;
}

/* -1, 0, 1 like strcmp. Numbers compare as numbers, everything else as text. */
static int v_compare(const Value *a, const Value *b) {
    if (a->t == V_NUM && b->t == V_NUM) return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
    char *x = v_to_text(a), *y = v_to_text(b);
    int r = strcmp(x, y);
    free(x); free(y);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

/* ================================================================== */
/* Environment: variables and request information                      */
/* ================================================================== */

typedef struct { char *name; Value val; } Var;
typedef struct { char *name; char **args; int argc; char *body; int first_line; void *lines; } Verb;   /* lines: the body, split once (Lines*) */
static void lines_free(void *lines);
static void lines_retire(void *lines, char *body);

typedef struct Env {
    Var *vars; int n, cap;
    const char *query;     /* "a=1&b=2" or NULL */
    const char *body;      /* submitted form data, same shape, or NULL */
    const char *cookie;    /* the Cookie header, or NULL */
    char **skeys, **svals; int sn, scap;   /* the session: remembered values */
    int session_loaded, session_changed, session_cleared;
    int in_hole;           /* evaluating [a hole] inside text: errors mention \[ */
    int plain_holes;   /* inside a send email block: holes are filled as plain text, not HTML */
    int sandbox;           /* running practice code from 'try': no files, database, cookies, redirects */
    const char *line_text; size_t line_len;   /* the raw line being run, for forgiving hints */
    Verb *verbs; int vn, vcap;                /* verbs defined on the page (kept on the root env) */
    struct Env *parent, *root;                /* a verb runs in a child env; sessions and redirects live on the root */
    Buf *out;                                 /* where 'show' writes right now */
    Value gave; int has_gave;                 /* set by 'give' inside a verb */
    int depth;
    const char *hint_prefix; int hint_skip;   /* "show" / "set NAME to": how to quote the rest of the line */
    const char *hint_name;                    /* for "set NAME to": the name, so the hint is built only on an error */
    long steps;            /* statements run, for the sandbox limit */
    const char *path;      /* "/about" or NULL */
    char redirect[1024];   /* set by 'go to' */
    int stopped;           /* 'go to' ran: stop the page */
    char dir[2048];        /* folder of the page, for include */
    int include_depth;
    const CLDatabase *dbdrv;  /* the module that opened the database: on the root env only */
    void *db;                 /* its handle, while a database is open */
} Env;

static void env_init(Env *e, const char *page_path, const char *query, const char *body, const char *url_path, const char *cookie) {
    memset(e, 0, sizeof *e);
    e->query = query; e->body = body; e->path = url_path; e->cookie = cookie; e->root = e;
    dir_of(page_path, e->dir, sizeof e->dir);
}
static void env_free(Env *e) {
    for (int i = 0; i < e->n; i++) { free(e->vars[i].name); v_free(&e->vars[i].val); }
    free(e->vars);
    for (int i = 0; i < e->sn; i++) { free(e->skeys[i]); free(e->svals[i]); }
    free(e->skeys); free(e->svals);
    for (int i = 0; i < e->vn; i++) { free(e->verbs[i].name); for (int a = 0; a < e->verbs[i].argc; a++) free(e->verbs[i].args[a]); free(e->verbs[i].args); free(e->verbs[i].body); lines_free(e->verbs[i].lines); }
    free(e->verbs);
    if (e->db && e->dbdrv) e->dbdrv->close(e->db);
}

static Verb *verb_find(Env *e, const char *name) {
    Env *r = e->root ? e->root : e;
    for (int i = 0; i < r->vn; i++) if (strcmp(r->verbs[i].name, name) == 0) return &r->verbs[i];
    return NULL;
}
static void verb_define(Env *e, const char *name, char **args, int argc, char *body, int first_line) {
    Env *r = e->root ? e->root : e;
    Verb *v = verb_find(e, name);
    if (v) {
        /* A verb defined again, maybe while it is running: keep the old body
           until the page ends, chained to the new lines, since a running call
           may still be reading it. */
        for (int a = 0; a < v->argc; a++) free(v->args[a]);
        free(v->args);
        if (!v->lines) free(v->body);
        else { lines_retire(v->lines, v->body); v->lines = NULL; }
    }
    else {
        if (r->vn == r->vcap) { r->vcap = r->vcap ? r->vcap * 2 : 8; r->verbs = realloc(r->verbs, sizeof(Verb) * (size_t)r->vcap); }
        v = &r->verbs[r->vn++]; v->name = xstrdup(name); v->lines = NULL;
    }
    v->args = args; v->argc = argc; v->body = body; v->first_line = first_line;
}
/* A name set in this environment itself. */
static Value *env_get_here(Env *e, const char *name) {
    for (int i = 0; i < e->n; i++) if (strcmp(e->vars[i].name, name) == 0) return &e->vars[i].val;
    return NULL;
}
/* A name as a verb sees it: its own, or else its caller's, and so on up.
   Verbs read the caller's names in place instead of copying them all on every
   call; whatever a verb sets goes into its own environment (env_set), so the
   caller's values never change. Read only: to change one, use env_get_own. */
static Value *env_get(Env *e, const char *name) {
    for (Env *x = e; x; x = x->parent) { Value *v = env_get_here(x, name); if (v) return v; }
    return NULL;
}
static void env_set(Env *e, const char *name, Value v);
/* A name about to be changed in place (add to a list, set a column): if it
   belongs to a caller, this verb gets its own copy first. */
static Value *env_get_own(Env *e, const char *name) {
    Value *v = env_get_here(e, name);
    if (v) return v;
    Value *up = env_get(e, name);
    if (!up) return NULL;
    env_set(e, name, v_copy(up));
    return env_get_here(e, name);
}
static void env_set(Env *e, const char *name, Value v) {   /* takes ownership of v */
    Value *old = env_get_here(e, name);
    if (old) { v_free(old); *old = v; return; }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 8; e->vars = realloc(e->vars, sizeof(Var) * (size_t)e->cap); }
    e->vars[e->n].name = xstrdup(name);
    e->vars[e->n].val  = v;
    e->n++;
}

static char *url_decode(const char *s, size_t n) {
    char *out = malloc(n + 1), *o = out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') *o++ = ' ';
        else if (s[i] == '%' && i + 2 < n + 0 && isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2])) {
            char h[3] = { s[i+1], s[i+2], 0 }; *o++ = (char)strtol(h, NULL, 16); i += 2;
        } else *o++ = s[i];
    }
    *o = 0;
    return out;
}

static Value query_lookup(const char *query, const char *key) {
    if (!query) return v_text("");
    const char *p = query; size_t klen = strlen(key);
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        size_t k = eq ? (size_t)(eq - p) : seg;
        if (k == klen && strncmp(p, key, k) == 0) {
            char *val = url_decode(eq ? eq + 1 : p + seg, eq ? seg - k - 1 : 0);
            Value v = v_text(val); v.untrusted = 1; free(val); return v;
        }
        p += seg; if (*p == '&') p++;
    }
    return v_text("");
}

static void url_encode(const char *s, Buf *b) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') buf_putc(b, (char)c);
        else { char t[4]; snprintf(t, sizeof t, "%%%02X", c); buf_puts(b, t); }
    }
}

/* ================================================================== */
/* SHA-256 and HMAC, for session cookies and password hashes           */
/* ================================================================== */

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } Sha256;
static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(Sha256 *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha256_init(Sha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha256_update(Sha256 *c, const void *data, size_t n) {
    const unsigned char *p = data; c->len += n;
    while (n) {
        size_t take = 64 - c->n; if (take > n) take = n;
        memcpy(c->buf + c->n, p, take); c->n += take; p += take; n -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
static void sha256_final(Sha256 *c, unsigned char out[32]) {
    uint64_t bits = c->len * 8; unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    while (c->n != 56) { unsigned char z = 0; sha256_update(c, &z, 1); }
    unsigned char lb[8]; for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) { out[i*4] = (unsigned char)(c->h[i] >> 24); out[i*4+1] = (unsigned char)(c->h[i] >> 16); out[i*4+2] = (unsigned char)(c->h[i] >> 8); out[i*4+3] = (unsigned char)c->h[i]; }
}
static void sha256(const void *a, size_t an, const void *b, size_t bn, unsigned char out[32]) {
    Sha256 c; sha256_init(&c); sha256_update(&c, a, an); if (b) sha256_update(&c, b, bn); sha256_final(&c, out);
}
static void hmac_sha256(const char *key, const void *msg, size_t n, unsigned char out[32]) {
    unsigned char k[64] = {0}, ipad[64], opad[64], inner[32];
    size_t kl = strlen(key);
    if (kl > 64) sha256(key, kl, NULL, 0, k); else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 c; sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, n); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}
static void to_hex(const unsigned char *b, size_t n, char *out) {
    static const char *hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = hx[b[i] >> 4]; out[i*2+1] = hx[b[i] & 15]; }
    out[n*2] = 0;
}
static int from_hex(const char *h, size_t n, unsigned char *out) {     /* n = number of bytes wanted */
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = h[i*2+j]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else return 0;
        }
        out[i] = (unsigned char)v;
    }
    return 1;
}
/* Secure random bytes for salts and the site secret. There is no safe
   fallback: guessable bytes would let anyone forge a sign-in, so stop instead. */
static void random_bytes(unsigned char *b, size_t n) {
#ifdef _WIN32
    for (size_t i = 0; i < n; i++) {
        unsigned int r;
        if (rand_s(&r) != 0) { fprintf(stderr, "cl: Windows gave no secure random numbers\n"); exit(1); }
        b[i] = (unsigned char)r;
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t got = fread(b, 1, n, f); fclose(f); if (got == n) return; }
    fprintf(stderr, "cl: cannot read /dev/urandom, so there are no secure random numbers\n");
    exit(1);
#endif
}

/* ------------------------------------------------------------------ */
/* Email                                                               */
/* ------------------------------------------------------------------ */

/* One plain email address: local@domain.tld, no spaces, commas or angle
   brackets, so it can never smuggle a second recipient or a header. */
static int is_email(const char *s) {
    size_t n = strlen(s);
    if (n < 6 || n > 254) return 0;
    const char *at = strchr(s, '@');
    if (!at || strchr(at + 1, '@') || at == s || at - s > 64) return 0;
    for (const char *p = s; p < at; p++)
        if (!(isalnum((unsigned char)*p) || strchr(".!#$%&'*+/=?^_`{|}~-", *p))) return 0;
    if (s[0] == '.' || at[-1] == '.' || strstr(s, "..")) return 0;
    const char *d = at + 1, *dot = strrchr(d, '.');
    if (!dot || dot == d || !dot[1] || d[0] == '-' || d[0] == '.') return 0;
    for (const char *p = d; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.')) return 0;
    for (const char *p = dot + 1; *p; p++) if (!isalpha((unsigned char)*p)) return 0;
    return 1;
}

static int g_mail_live;      /* 1: hand emails to sendmail (cl cgi, or CL_MAIL=sendmail); 0: save them in outbox/ */
static int g_mail_sent;      /* emails sent by this page run */
#define MAIL_PER_PAGE 3
#define MAIL_PER_HOUR 20

/* The site secret signs session cookies. It lives in cl.secret in the site
   folder and is created on first use. */
static char g_site_root[2048];   /* the folder cl serve serves, or the web root under cl cgi */
static char g_page_dir[2048];    /* the folder of the page being run: the top folder when there is no site root */
static char g_include_root[2048]; /* where include "/x.html" starts, when it is not g_site_root */
static char g_secret[129];
/* Where cl.secret, mail.log and outbox/ live. Under cl cgi this is a folder
   outside the web root (see private_dir_for_cgi), so a missing or broken
   .htaccess cannot expose them. Otherwise it is the site's own folder. */
static char g_private_dir[2048];
static const char *private_base(const char *fallback_dir) {
    return g_private_dir[0] ? g_private_dir : g_site_root[0] ? g_site_root : fallback_dir;
}

static const char *site_secret(const char *fallback_dir) {
    if (g_secret[0]) return g_secret;
    char path[4096]; snprintf(path, sizeof path, "%s/cl.secret", private_base(fallback_dir));
    FILE *f = fopen(path, "rb");
    if (f) { size_t n = fread(g_secret, 1, 128, f); fclose(f); g_secret[n] = 0; while (n && isspace((unsigned char)g_secret[n-1])) g_secret[--n] = 0; if (n >= 32) return g_secret; }
    unsigned char raw[32]; random_bytes(raw, 32); to_hex(raw, 32, g_secret);
    f = fopen(path, "wb");
    if (f) { fputs(g_secret, f); fputc('\n', f); fclose(f);
#ifndef _WIN32
        chmod(path, 0600);                                        /* only this account may read it */
#endif
    }
    else fprintf(stderr, "cl: cannot write %s, so remembered values will not survive a restart\n", path);
    return g_secret;
}

/* ---- wrong passwords: at most 10 from one address in 10 minutes ---- */
/* Each check costs about 0.3 s of work on purpose, so guessing is slow; this
   also stops one address from guessing at all, and from using the server's
   time for it. Wrong attempts are noted in login.log in the private folder
   (see private_base): one line per attempt, "<time> <address>". */
#define LOGIN_TRIES 10
#define LOGIN_WINDOW 600
static char g_remote[64];            /* the visitor's address, under cl serve and cl cgi */
static int login_recent(const char *log, time_t now) {
    FILE *f = fopen(log, "r"); if (!f) return 0;
    int n = 0; char line[128], who[80];
    while (fgets(line, sizeof line, f)) {
        long long t; if (sscanf(line, "%lld %79s", &t, who) != 2) continue;
        if (now - (time_t)t < LOGIN_WINDOW && !strcmp(who, g_remote)) n++;
    }
    fclose(f); return n;
}

/* ---- sessions: a signed cookie, cl_session=<hex payload>.<hmac> ---- */

#define SESSION_SECONDS 2592000      /* 30 days */
static int g_https;                  /* the request came over HTTPS (cl cgi): cookies get Secure */

static void session_set(Env *e, const char *k, const char *v) {
    if (e->root && e->root != e) e = e->root;
    for (int i = 0; i < e->sn; i++) if (strcmp(e->skeys[i], k) == 0) { free(e->svals[i]); e->svals[i] = xstrdup(v); return; }
    if (e->sn == e->scap) { e->scap = e->scap ? e->scap * 2 : 8; e->skeys = realloc(e->skeys, sizeof(char *) * (size_t)e->scap); e->svals = realloc(e->svals, sizeof(char *) * (size_t)e->scap); }
    e->skeys[e->sn] = xstrdup(k); e->svals[e->sn] = xstrdup(v); e->sn++;
}
static void session_load(Env *e) {
    if (e->root && e->root != e) e = e->root;
    if (e->session_loaded) return;
    e->session_loaded = 1;
    if (!e->cookie) return;
    const char *c = NULL;                     /* exactly cl_session=, not xcl_session= */
    for (const char *p = e->cookie; *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, "cl_session=", 11)) { c = p + 11; break; }
        while (*p && *p != ';') p++;
    }
    if (!c) return;
    const char *end = c; while (*end && *end != ';') end++;
    const char *dot = memchr(c, '.', (size_t)(end - c));
    if (!dot || end - dot - 1 != 64) return;
    unsigned char mac[32]; char machex[65];
    hmac_sha256(site_secret(e->dir), c, (size_t)(dot - c), mac); to_hex(mac, 32, machex);
    unsigned char diff = 0;                                     /* compare every byte, whatever differs */
    for (int i = 0; i < 64; i++) diff |= (unsigned char)(machex[i] ^ dot[1 + i]);
    if (diff) return;                                           /* forged or stale: ignore */
    size_t plen = (size_t)(dot - c) / 2;
    unsigned char *payload = malloc(plen + 1);
    if (!from_hex(c, plen, payload)) { free(payload); return; }
    payload[plen] = 0;
    /* <expiry>|k=v&k=v: the expiry is signed too, so a copied cookie stops working after 30 days */
    char *bar = strchr((char *)payload, '|');
    if (!bar || strtoll((char *)payload, NULL, 10) < (long long)time(NULL)) { free(payload); return; }
    const char *p = bar + 1;
    while (*p) {
        const char *amp = strchr(p, '&'); size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        if (eq) {
            char *k = url_decode(p, (size_t)(eq - p)), *v = url_decode(eq + 1, seg - (size_t)(eq - p) - 1);
            session_set(e, k, v); free(k); free(v);
        }
        p += seg; if (*p == '&') p++;
    }
    free(payload);
}
static const char *session_get(Env *e, const char *k) {
    if (e->root && e->root != e) e = e->root;
    session_load(e);
    for (int i = 0; i < e->sn; i++) if (strcmp(e->skeys[i], k) == 0) return e->svals[i];
    return NULL;
}
static void session_forget(Env *e, const char *k) {
    if (e->root && e->root != e) e = e->root;
    session_load(e);
    for (int i = 0; i < e->sn; i++) if (strcmp(e->skeys[i], k) == 0) {
        free(e->skeys[i]); free(e->svals[i]);
        e->skeys[i] = e->skeys[e->sn - 1]; e->svals[i] = e->svals[e->sn - 1]; e->sn--;
        e->session_changed = 1; return;
    }
}
/* The Set-Cookie value for this session, or "" if unchanged. Caller frees. */
static char *session_cookie(Env *e) {
    if (e->root && e->root != e) e = e->root;
    if (!e->session_changed) return xstrdup("");
    if (e->sn == 0) return xstrdup("cl_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    Buf *pl = buf_new();
    char exp[32]; snprintf(exp, sizeof exp, "%lld|", (long long)time(NULL) + SESSION_SECONDS); buf_puts(pl, exp);
    for (int i = 0; i < e->sn; i++) { if (i) buf_putc(pl, '&'); url_encode(e->skeys[i], pl); buf_putc(pl, '='); url_encode(e->svals[i], pl); }
    char *hex = malloc(pl->len * 2 + 1); to_hex((const unsigned char *)pl->data, pl->len, hex);
    unsigned char mac[32]; char machex[65];
    hmac_sha256(site_secret(e->dir), hex, strlen(hex), mac); to_hex(mac, 32, machex);
    char *out = malloc(strlen(hex) + 200);
    sprintf(out, "cl_session=%s.%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax%s", hex, machex, SESSION_SECONDS, g_https ? "; Secure" : "");
    free(hex); buf_free(pl);
    return out;
}

/* Password hashes: cl2$<rounds>$<salt>$<hex>, PBKDF2-HMAC-SHA256. The rounds
   are written into the hash so they can be raised later without breaking old
   passwords. cl1$<salt>$<hex> (salted, iterated SHA-256 from 0.1) still checks. */
#define PASSWORD_ROUNDS 200000      /* about 0.3 s on a laptop */

static void pbkdf2_sha256(const char *pw, const unsigned char *salt, size_t saltn, long rounds, unsigned char out[32]) {
    unsigned char k[64] = {0}, pad[64], u[32];
    size_t kl = strlen(pw);
    if (kl > 64) sha256(pw, kl, NULL, 0, k); else memcpy(k, pw, kl);
    Sha256 in, ou;                                             /* the keyed halves of HMAC, made once */
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&in); sha256_update(&in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&ou); sha256_update(&ou, pad, 64);
    Sha256 c = in; unsigned char one[4] = { 0, 0, 0, 1 };
    sha256_update(&c, salt, saltn); sha256_update(&c, one, 4); sha256_final(&c, u);
    c = ou; sha256_update(&c, u, 32); sha256_final(&c, u);
    memcpy(out, u, 32);
    for (long r = 1; r < rounds; r++) {
        c = in; sha256_update(&c, u, 32); sha256_final(&c, u);
        c = ou; sha256_update(&c, u, 32); sha256_final(&c, u);
        for (int i = 0; i < 32; i++) out[i] ^= u[i];
    }
}

static char *password_hash(const char *text) {
    unsigned char salt[16], h[32]; char salt_hex[33], hex[65];
    random_bytes(salt, 16); to_hex(salt, 16, salt_hex);
    pbkdf2_sha256(text, salt, 16, PASSWORD_ROUNDS, h); to_hex(h, 32, hex);
    char *out = malloc(128);
    snprintf(out, 128, "cl2$%d$%s$%s", PASSWORD_ROUNDS, salt_hex, hex);
    return out;
}
static int same_text(const char *a, const char *b) {          /* constant time for equal lengths */
    size_t n = strlen(a); if (n != strlen(b)) return 0;
    int same = 1; for (size_t i = 0; i < n; i++) same &= (a[i] == b[i]);
    return same;
}
static int password_matches(const char *text, const char *stored) {
    unsigned char salt[16], h[32]; char hex[65];
    if (!strncmp(stored, "cl2$", 4)) {
        char *end; long rounds = strtol(stored + 4, &end, 10);
        if (rounds < 1 || rounds > 10000000 || *end != '$' || strlen(end + 1) != 32 + 1 + 64 || end[33] != '$') return 0;
        if (!from_hex(end + 1, 16, salt)) return 0;
        pbkdf2_sha256(text, salt, 16, rounds, h); to_hex(h, 32, hex);
        return same_text(hex, end + 34);
    }
    if (!strncmp(stored, "cl1$", 4) && strlen(stored) == 4 + 32 + 1 + 64 && stored[36] == '$') {
        if (!from_hex(stored + 4, 16, salt)) return 0;
        sha256(salt, 16, text, strlen(text), h);
        for (int i = 0; i < 50000; i++) sha256(h, 32, salt, 16, h);
        to_hex(h, 32, hex);
        return same_text(hex, stored + 37);
    }
    return 0;
}

/* ================================================================== */
/* Tokenizer: one line of Common into words, text, numbers, symbols    */
/* ================================================================== */

typedef enum { T_WORD, T_STR, T_NUM, T_OP } TokType;

/* Markers around a [value] hole inside a text token. */
#define HOLE_OPEN  '\x01'
#define HOLE_CLOSE '\x02'
typedef struct { TokType t; char *text; double num; int name_ok; } Tok;   /* name_ok: expect_name passed it already */
typedef struct { Tok *t; int n, cap, pos; } Toks;

static void toks_add(Toks *T, TokType t, char *text, double num) {
    if (T->n == T->cap) { T->cap = T->cap ? T->cap * 2 : 16; T->t = realloc(T->t, sizeof(Tok) * (size_t)T->cap); }
    T->t[T->n].t = t; T->t[T->n].text = text; T->t[T->n].num = num; T->t[T->n].name_ok = 0; T->n++;
}
/* Every live token list is also noted here, so that when an error unwinds
   (fail() is a longjmp) run_block can free the lists the dead frames held. */
static Toks g_live[256]; static int g_nlive;
static void live_push(const Toks *T) { if (T->t && g_nlive < 256) g_live[g_nlive++] = *T; }
static void live_drop(const Toks *T) {
    for (int i = g_nlive - 1; i >= 0; i--) if (g_live[i].t == T->t) {
        memmove(&g_live[i], &g_live[i + 1], sizeof(Toks) * (size_t)(g_nlive - i - 1)); g_nlive--; return;
    }
}
/* Values and buffers an expression holds while it parses the rest of the line.
   fail() unwinds past those frames, so run_block frees whatever is still here. */
static Value g_held[256]; static int g_nheld;
struct Env;
static struct { struct Env *env; void *lines; } g_frames[128]; static int g_nframes;   /* lines is a Lines* */
static Buf *g_bufs[64]; static int g_nbufs;
static void held_push(Value v) { if (g_nheld < 256) g_held[g_nheld++] = v; }
static void held_pop(void)     { if (g_nheld > 0) g_nheld--; }
static void toks_free(Toks *T) {
    if (T->t) live_drop(T);
    for (int i = 0; i < T->n; i++) free(T->t[i].text);
    free(T->t); memset(T, 0, sizeof *T);
}

static void tokenize(const char *s, size_t len, Toks *T) {
    memset(T, 0, sizeof *T);
    /* The bytes that mark a [hole] inside a text token can never come from source,
       or a stray one would start a hole with no end. */
    for (size_t k = 0; k < len; k++)
        if (s[k] == HOLE_OPEN || s[k] == HOLE_CLOSE) fail("the line has an invisible control character in it (byte %d). Delete it and type the line again", s[k]);
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '#') break;                                   /* comment */
        if (c == '"') {
            Buf *b = buf_new(); i++;
            if (g_nbufs < 64) g_bufs[g_nbufs++] = b;              /* freed by run_block if the text never closes */
            while (i < len && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < len) {              /* \" \\ \n \[ */
                    i++;
                    buf_putc(b, s[i] == 'n' ? '\n' : s[i]);
                    i++;
                } else if (s[i] == '[') {
                    /* a hole: [value] inside text. Kept raw between markers, filled when evaluated. */
                    i++;
                    buf_putc(b, HOLE_OPEN);
                    int depth = 0, closed = 0;
                    while (i < len) {
                        if (s[i] == '"') {                       /* quoted text inside the hole */
                            buf_putc(b, s[i++]);
                            while (i < len && s[i] != '"') {
                                if (s[i] == '\\' && i + 1 < len) buf_putc(b, s[i++]);
                                buf_putc(b, s[i++]);
                            }
                            if (i < len) buf_putc(b, s[i++]);
                        } else if (s[i] == '[') { depth++; buf_putc(b, s[i++]); }
                        else if (s[i] == ']') {
                            if (depth == 0) { closed = 1; i++; break; }
                            depth--; buf_putc(b, s[i++]);
                        }
                        else buf_putc(b, s[i++]);
                    }
                    if (!closed) { live_push(T); fail("the text has a [ with no closing ]"); }
                    buf_putc(b, HOLE_CLOSE);
                } else buf_putc(b, s[i++]);
            }
            if (i >= len) { live_push(T); fail("text is missing its closing quote"); }
            i++;
            if (g_nbufs > 0 && g_bufs[g_nbufs - 1] == b) g_nbufs--;
            toks_add(T, T_STR, xstrdup(b->data), 0); buf_free(b);
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i+1]))) {
            char *end; double d = strtod(s + i, &end);
            toks_add(T, T_NUM, NULL, d); i = (size_t)(end - s);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (j - i == 3 && strncmp(s + i, "the", 3) == 0) { i = j; continue; }   /* 'the' is silent: show the count of fruits */
            toks_add(T, T_WORD, xstrndup(s + i, j - i), 0); i = j;
            continue;
        }
        if (c == '.') {                                        /* a full stop at the end of the line is fine */
            size_t j = i + 1; while (j < len && isspace((unsigned char)s[j])) j++;
            if (j >= len) break;
        }
        if (strchr("+-*/(),", c)) {
            char op[2] = { c, 0 };
            toks_add(T, T_OP, xstrdup(op), 0); i++;
            continue;
        }
        if (c == '=') { live_push(T); fail("Common has no '='. Write 'set NAME to VALUE' to store, or 'is' to compare"); }
        if (c == '>') { live_push(T); fail("Common has no '>'. Write 'is more than'"); }
        if (c == '<') { live_push(T); fail("Common has no '<'. Write 'is less than'"); }
        if (c == '!') { live_push(T); fail("Common has no '!'. Write 'is not' or 'not'"); }
        if (c == '{' || c == '}') { live_push(T); fail("Common has no braces. Blocks end with the word 'end', and values go inside text like \"Hello [name]\""); }
        if (c == '[' || c == ']') { live_push(T); fail("brackets only work inside text, like \"Hello [name]\""); }
        if (c == ';') { live_push(T); fail("Common has no ';'. Put each command on its own line"); }
        if (c == '\'') { live_push(T); fail("text goes in double quotes, like \"this\""); }
        { live_push(T); fail("I do not understand the character '%c'", c); }
    }
    live_push(T);
}

static int at_end(Toks *T)  { return T->pos >= T->n; }
static int at_word(Toks *T, const char *w) {
    if (at_end(T) || T->t[T->pos].t != T_WORD) return 0;
    const char *t = T->t[T->pos].text;
    return t[0] == w[0] && strcmp(t, w) == 0;       /* most words differ in the first letter */
}
static int at_op(Toks *T, char c)          { return !at_end(T) && T->t[T->pos].t == T_OP && T->t[T->pos].text[0] == c; }
static int accept_word(Toks *T, const char *w) { if (at_word(T, w)) { T->pos++; return 1; } return 0; }
static void expect_word(Toks *T, const char *w) {
    if (!accept_word(T, w)) {
        if (at_end(T)) fail("expected '%s' but the line ended", w);
        fail("expected '%s' but found '%s'", w, T->t[T->pos].text ? T->t[T->pos].text : "a number");
    }
}
static const char *expect_name(Toks *T) {
    if (at_end(T) || T->t[T->pos].t != T_WORD) fail("expected a name");
    if (T->t[T->pos].name_ok) return T->t[T->pos++].text;   /* checked on an earlier run of this line */
    const char *w = T->t[T->pos].text;
    const Word *d = dict_find(w);
    if (d && !is_soft_word(w)) fail("'%s' is a Common word%s%s, so it cannot be a name. Pick another, like my_%s", w,
                d->form ? ": " : "", d->form ? d->form : "", w);
    char lc[64]; lowercase(w, lc, sizeof lc);
    if (dict_find(lc) && !is_soft_word(lc)) fail("'%s' is too close to the Common word '%s'. Pick another name", w, lc);
    T->t[T->pos].name_ok = 1;
    return T->t[T->pos++].text;
}
static const char *synonym(const char *w);
static void expect_line_end(Toks *T) {
    if (!at_end(T)) {
        Tok *k = &T->t[T->pos];
        if (k->t == T_NUM) fail("unexpected number %.15g", k->num);
        if (k->t == T_STR) fail("unexpected text \"%s\"", k->text);
        const char *syn = k->t == T_WORD ? synonym(k->text) : NULL;
        if (syn) fail("unexpected '%s'. In Common that is written '%s'", k->text, syn);
        fail("unexpected '%s'", k->text);
    }
}

/* ================================================================== */
/* Expressions (parsed and evaluated in one pass)                      */
/* ================================================================== */

/* A value as plain text, for where it leaves the page as data: a database
   parameter, a remembered value, an email, a go-to address. Text joined with
   a visitor's value ("[form "x"] ...") had that value HTML-escaped so it could
   be shown on a page; stored or sent like that, it would be escaped a second
   time when shown again. So the five escapes are turned back into letters.
   Text that is not marked (tainted) is returned as it is. Caller frees. */
static char *plain_text(const Value *v) {
    char *t = v_to_text(v);
    if (v->t != V_TEXT || !v->tainted) return t;
    static const char *from[] = { "&amp;", "&lt;", "&gt;", "&quot;", "&#39;" }, to[] = "&<>\"'";
    char *w = t;
    for (const char *p = t; *p; ) {
        int hit = 0;
        for (int i = 0; i < 5; i++) { size_t n = strlen(from[i]); if (!strncmp(p, from[i], n)) { *w++ = to[i]; p += n; hit = 1; break; } }
        if (!hit) *w++ = *p++;
    }
    *w = 0; return t;
}

/* ------------------------------------------------------------------ */
/* Modules: optional capabilities compiled in behind cl_module.h.       */
/* The core knows the words; a module knows a library. The Makefile    */
/* links src/mod_*.c for each library it finds and defines CL_SQLITE   */
/* or CL_POSTGRES so modules_load() calls that module's entry point.   */
/* ------------------------------------------------------------------ */

static Value v_untrusted_text(const char *s) { Value v = v_text(s); v.untrusted = 1; return v; }

/* What a module may call. One table for the whole run; see CLHost in cl_module.h. */
static const CLHost g_host = {
    CL_MODULE_API, CL_VERSION,
    fail,
    v_text, v_untrusted_text, v_num, v_bool, v_list, v_record, list_add, record_add, v_free,
    plain_text, type_name
};

#define MAX_MODULES 16
static const CLModule *g_modules[MAX_MODULES]; static int g_nmodules;

/* Called once as cl starts. The list is every module the build linked in; a
   loader for shared libraries would add what it finds to g_modules here. */
static void modules_load(void) {
    static const cl_module_entry_fn builtin[] = {
#ifdef CL_SQLITE
        cl_module_sqlite,
#endif
#ifdef CL_POSTGRES
        cl_module_postgres,
#endif
        NULL
    };
    for (int i = 0; builtin[i]; i++) {
        const CLModule *m = builtin[i](&g_host);
        if (!m || m->api != CL_MODULE_API) {
            fprintf(stderr, "cl: the module %s was built for another version of the module interface (%d; this cl has %d). Run make clean, then make\n",
                    m ? m->name : "?", m ? m->api : 0, CL_MODULE_API);
            exit(2);
        }
        if (g_nmodules < MAX_MODULES) g_modules[g_nmodules++] = m;
    }
}

/* The n-th capability of a kind, or NULL: for (i = 0; (d = capability(kind, i)); i++) */
static const void *capability(CLCapKind kind, int n) {
    for (int i = 0; i < g_nmodules; i++)
        for (int c = 0; c < g_modules[i]->ncaps; c++)
            if (g_modules[i]->caps[c].kind == kind && n-- == 0) return g_modules[i]->caps[c].table;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Databases: the words are here, the drivers are modules.              */
/* ------------------------------------------------------------------ */

static int db_available(void) { return capability(CL_CAP_DATABASE, 0) != NULL; }

/* The open database lives on the page's root environment, so a verb sees the
   database its page opened, and a database a verb opens stays open for the page. */
static Env *db_env(Env *e) { return e->root ? e->root : e; }

static void db_close(Env *r) {
    if (r->db && r->dbdrv) r->dbdrv->close(r->db);
    r->db = NULL; r->dbdrv = NULL;
}

/* Every kind of address this build opens, for an error: 'a file next to the page
   (open database "site.db") or a Postgres server (open database "postgres://...")' */
static void db_kinds(char *out, size_t cap) {
    out[0] = 0;
    const CLDatabase *d;
    for (int i = 0; (d = capability(CL_CAP_DATABASE, i)); i++) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s (open database \"%s\")", i ? " or " : "", d->where, d->example);
    }
}

static NOINLINE void db_open(Env *e, const char *address) {
    Env *r = db_env(e);
    const CLDatabase *d = NULL, *x;
    for (int i = 0; (x = capability(CL_CAP_DATABASE, i)); i++) if (x->claims(address)) { d = x; break; }
    if (!d) {
        if (!db_available()) fail("this build of Common has no database support, so \"%s\" cannot be opened. Install SQLite and run make again", address);
        if (!strncmp(address, "postgres://", 11) || !strncmp(address, "postgresql://", 13))
            fail("this build of Common has no Postgres support. Install libpq (the Postgres client library) and run make again");
        char kinds[512]; db_kinds(kinds, sizeof kinds);
        fail("cannot open the database \"%s\": this build of Common opens %s", address, kinds);
    }
    db_close(r);                                   /* one database at a time: the last one opened */
    r->db = d->open(&g_host, address, r->dir);
    r->dbdrv = d;
}

/* The open database, or the error for having none. */
static const CLDatabase *db_need(Env *e, void **handle) {
    Env *r = db_env(e);
    if (!r->db) {
        if (!db_available()) fail("this build of Common has no database support. Install SQLite and run make again");
        fail("no database is open. Write: open database \"file.db\" first");
    }
    *handle = r->db;
    return r->dbdrv;
}

/* Run SQL with bound parameters. Returns a list of records, one per row. */
static Value db_ask(Env *e, const char *sql, Value *params) {
    void *h; const CLDatabase *d = db_need(e, &h);
    return d->ask(&g_host, h, sql, params);
}

/* Run SQL that changes the database. */
static void db_tell(Env *e, const char *sql, Value *params) {
    void *h; const CLDatabase *d = db_need(e, &h);
    d->tell(&g_host, h, sql, params);
}

/* For `cl words`: the modules in this build and what each one opens. */
static void print_modules(void) {
    if (!g_nmodules) return;
    printf("\nModules in this build:\n");
    for (int i = 0; i < g_nmodules; i++) {
        const CLModule *m = g_modules[i];
        printf("  %-10s %s", m->name, m->library);
        for (int c = 0; c < m->ncaps; c++) if (m->caps[c].kind == CL_CAP_DATABASE) {
            const CLDatabase *d = m->caps[c].table;
            printf(": open database \"%s\", %s", d->example, d->where);
        }
        printf("\n");
    }
}

static Value parse_expr(Env *e, Toks *T);
/* How much C stack the page may use. Blocks, brackets and verbs inside each other
   each take a frame; the counts below cap them, but a frame's size depends on the
   compiler and the stack on the computer (8 MB on Linux and macOS, 1 MB on Windows,
   less on some hosts), so the page is also stopped before the stack itself runs out. */
static uintptr_t g_stack_base;        /* where the stack began, recorded as main() starts */
static size_t g_stack_budget;         /* how far below it the page may go */
/* Where the C stack is right now: the frame itself, which is the real stack even when
   a sanitizer keeps copies of local variables elsewhere, so &local would mislead. */
static uintptr_t stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#else
    char here; return (uintptr_t)&here;
#endif
}
static void stack_setup(void) {
    size_t limit = (size_t)1 << 20;                            /* Windows: 1 MB unless the linker was told otherwise */
#ifndef _WIN32
    struct rlimit rl;
    limit = (size_t)8 << 20;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && (size_t)rl.rlim_cur < limit) limit = (size_t)rl.rlim_cur;
#endif
    g_stack_base = stack_here();
    g_stack_budget = limit - limit / 4;                       /* a quarter is kept for the deepest leaf calls: SQLite, email, files */
}
static void stack_check(void) {
    uintptr_t now = stack_here();                              /* the stack grows down on every supported machine */
    if (g_stack_base && now < g_stack_base && g_stack_base - now > g_stack_budget)
        fail("the page is nested too deeply for this computer: too many blocks, brackets or verbs inside each other. Make it flatter");
}

/* How deeply expressions are nested right now: brackets, holes, 'not', '-'.
   Each level is a C stack frame, so a limit keeps a hostile line from crashing. */
static int g_expr_depth;
#define MAX_EXPR_DEPTH 200
static void expr_enter(void) {
    if (++g_expr_depth > MAX_EXPR_DEPTH) { g_expr_depth = 0; fail("the line nests too deeply (more than %d brackets, holes or 'not's inside each other)", MAX_EXPR_DEPTH); }
    if (g_expr_depth > 8) stack_check();                       /* deep brackets: watch the stack itself too */
}
static Value parse_add(Env *e, Toks *T);

/* Parse the optional "with VALUE, VALUE" after a database command. Each value
   may be a name, text, or math, but not a comparison: in
   'count of ask database "..." with slug is more than 0' the comparison belongs
   to the count, not to the parameter. */
static Value parse_with(Env *e, Toks *T) {
    Value params = v_list();
    if (accept_word(T, "with")) {
        held_push(params);
        list_add(&params, parse_add(e, T)); held_pop(); held_push(params);
        while (at_op(T, ',')) { T->pos++; list_add(&params, parse_add(e, T)); held_pop(); held_push(params); }
        held_pop();
    }
    return params;
}

/* Does the text so far end where a link address starts, like <a href=" ?
   A visitor's value there could be javascript:..., which escaping does not stop. */
static int starts_url_attr(const Buf *b) {
    static const char *attrs[] = { "href", "src", "action", "formaction", "poster", "srcset", NULL };
    size_t n = b->len;
    if (n && (b->data[n-1] == '"' || b->data[n-1] == '\'')) n--;
    while (n && isspace((unsigned char)b->data[n-1])) n--;
    if (!n || b->data[n-1] != '=') return 0;
    n--; while (n && isspace((unsigned char)b->data[n-1])) n--;
    for (int i = 0; attrs[i]; i++) {
        size_t al = strlen(attrs[i]);
        if (n < al || (n > al && (isalnum((unsigned char)b->data[n-al-1]) || b->data[n-al-1] == '-'))) continue;
        int same = 1; for (size_t k = 0; k < al; k++) if (tolower((unsigned char)b->data[n-al+k]) != attrs[i][k]) same = 0;
        if (same) return 1;
    }
    return 0;
}
/* A scheme that runs code: javascript:, vbscript:, data:. Browsers skip spaces,
   control characters and case, so this does too. */
static int unsafe_scheme(const char *s) {
    char w[16]; size_t j = 0;
    for (; *s && j + 1 < sizeof w; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= ' ') continue;
        if (c == ':') { w[j] = 0; return !strcmp(w, "javascript") || !strcmp(w, "vbscript") || !strcmp(w, "data"); }
        if (!isalpha(c)) return 0;
        w[j++] = (char)tolower(c);
    }
    return 0;
}

/* The tokens of each [hole], kept by its text, so text that is built again and
   again (in a loop) does not re-read its holes. Keyed by content, never by
   address: text can be temporary. When the table is full, holes are simply
   read each time, as before. */
#define HOLE_CACHE 1024
static struct { char *src; size_t len; Tok *t; int n; } g_holes[HOLE_CACHE];
static void hole_tokens(const char *src, size_t len, Toks *U) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)src[i]) * 16777619u;
    for (unsigned k = h % HOLE_CACHE, tries = 0; tries < 8; k = (k + 1) % HOLE_CACHE, tries++) {
        if (g_holes[k].src && g_holes[k].len == len && memcmp(g_holes[k].src, src, len) == 0) {
            memset(U, 0, sizeof *U); U->t = g_holes[k].t; U->n = g_holes[k].n; return;
        }
        if (!g_holes[k].src) {
            tokenize(src, len, U);
            live_drop(U);                                   /* kept for the life of the program */
            g_holes[k].src = xstrndup(src, len); g_holes[k].len = len; g_holes[k].t = U->t; g_holes[k].n = U->n;
            U->cap = 0; return;
        }
    }
    tokenize(src, len, U);                                  /* no room: an ordinary, owned token list */
    U->cap = -1;
}

/* Text with holes: "Hello [name]". Each hole is evaluated as a value and
   placed into the text, made safe if it came from the visitor. */
static Value fill_text(Env *e, const char *t) {
    if (!strchr(t, HOLE_OPEN)) return v_text(t);
    Buf *b = buf_new(); int tainted = 0;
    if (g_nbufs < 64) g_bufs[g_nbufs++] = b;
    const char *p = t;
    while (*p) {
        if (*p != HOLE_OPEN) { buf_putc(b, *p++); continue; }
        const char *end = strchr(p + 1, HOLE_CLOSE);
        if (!end) fail("the text has a [ with no closing ]");      /* cannot happen from source: tokenize refuses stray markers */
        Toks U; hole_tokens(p + 1, (size_t)(end - p - 1), &U);
        int owned = U.cap == -1;
        if (U.n == 0) fail("the brackets in the text are empty. Put a value inside, like [name]");
        e->in_hole++;
        Value v = parse_expr(e, &U);
        e->in_hole--;
        held_push(v);
        if (!at_end(&U)) fail("unexpected '%s' inside the brackets", U.t[U.pos].text ? U.t[U.pos].text : "number");
        if (v.untrusted || v.tainted) tainted = 1;
        /* Both the value and its text stay held until they are in the buffer:
           growing the text can hit the memory limit of practice code. */
        char *s = e->plain_holes ? v_to_text(&v) : v_to_safe_text(&v);
        Value sv = { V_TEXT, s, 0, NULL, NULL, 0, 0, 0, 0 };
        held_push(sv);
        if (!e->plain_holes && (v.untrusted || v.tainted) && starts_url_attr(b) && unsafe_scheme(s)) buf_puts(b, "#unsafe-link");
        else buf_puts(b, s);
        held_pop(); held_pop();
        free(s);
        v_free(&v); if (owned) { U.cap = 0; toks_free(&U); }
        p = end + 1;
    }
    if (g_nbufs > 0 && g_bufs[g_nbufs - 1] == b) g_nbufs--;
    Value v = v_text(b->data); buf_free(b); v.tainted = tainted;
    return v;
}

/* One column of a record. Frees the record. */
static Value record_field(Value *r, const char *field) {
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->keys[i], field) == 0) { Value c = v_copy(&r->items[i]); v_free(r); return c; }
    char have[512] = "";
    for (int i = 0; i < r->count; i++) {
        if (i) strncat(have, ", ", sizeof have - strlen(have) - 1);
        strncat(have, r->keys[i], sizeof have - strlen(have) - 1);
    }
    const char *closest = NULL; int best = 3;           /* not "near": Windows headers define near as a macro */
    for (int i = 0; i < r->count; i++) { int d = edit_distance(field, r->keys[i]); if (d < best && d < (int)strlen(field)) { best = d; closest = r->keys[i]; } }
    char hint[160] = "";
    if (closest) snprintf(hint, sizeof hint, " Did you mean '%s'?", closest);
    char closest_copy[128] = ""; if (closest) snprintf(closest_copy, sizeof closest_copy, "%s", closest);
    v_free(r);
    if (closest_copy[0]) fail("the record has no '%s'.%s It has: %s", field, hint, have);
    fail("the record has no '%s'. It has: %s", field, have);
}

static Value run_sandbox(Env *parent, const char *code);
static Value verb_call(Env *e, Verb *vb, Value *args, Buf *out, int as_value);
static const char *verb_form(Verb *vb);

/* The raw line after `skip` words, trimmed, with double quotes escaped when asked. */
static void rest_of_line_x(Env *e, int skip, char *out, size_t n, int escape) {
    const char *p = e->line_text, *end = e->line_text + e->line_len;
    for (int i = 0; i < skip && p < end; i++) {
        while (p < end && isspace((unsigned char)*p)) p++;
        while (p < end && !isspace((unsigned char)*p)) p++;
    }
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    size_t j = 0;
    for (; p < end && j + 2 < n; p++) { if (escape && *p == '"') out[j++] = '\\'; out[j++] = *p; }
    out[j] = 0;
}
static void rest_of_line(Env *e, int skip, char *out, size_t n) { rest_of_line_x(e, skip, out, n, 1); }

/* Before a line is even tokenized: if show / set ... to / print is followed by
   plain words, the writer probably forgot the quotes. Say so, with the fixed line. */
static NOINLINE void forgive_unquoted(Env *e) {
    const char *p = e->line_text, *end = p + e->line_len;
    while (p < end && isspace((unsigned char)*p)) p++;
    const char *w = p; while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
    char cmd[32]; size_t cl = (size_t)(p - w); if (cl == 0 || cl >= sizeof cmd) return;
    memcpy(cmd, w, cl); cmd[cl] = 0;
    int skip = 1; char prefix[96];
    const char *syn = synonym(cmd);
    if (!strcmp(cmd, "show")) {
        snprintf(prefix, sizeof prefix, "show");
        const char *q = p; while (q < end && isspace((unsigned char)*q)) q++;
        if (end - q >= 4 && !strncmp(q, "raw ", 4))  { snprintf(prefix, sizeof prefix, "show raw");  skip = 2; }
        if (end - q >= 5 && !strncmp(q, "code ", 5)) { snprintf(prefix, sizeof prefix, "show code"); skip = 2; }
    } else if (!strcmp(cmd, "set")) {
        const char *q = p; while (q < end && isspace((unsigned char)*q)) q++;
        const char *nm = q; while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
        size_t nl = (size_t)(q - nm); if (nl == 0 || nl > 60) return;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (end - q < 3 || strncmp(q, "to ", 3)) return;
        snprintf(prefix, sizeof prefix, "set %.*s to", (int)nl, nm); skip = 3;
    } else if (syn && !strcmp(syn, "show")) {
        snprintf(prefix, sizeof prefix, "show");
    } else return;

    char rest[512]; rest_of_line_x(e, skip, rest, sizeof rest, 0);
    if (!rest[0] || rest[0] == '"' ) return;                        /* quoted already, or nothing to quote */
    if (!(isalpha((unsigned char)rest[0]) || rest[0] == '<')) return;   /* a number, a bracket, math: a real value */
    if (isalpha((unsigned char)rest[0])) {
        char first[64]; size_t k = 0;
        while (rest[k] && (isalnum((unsigned char)rest[k]) || rest[k] == '_') && k + 1 < sizeof first) { first[k] = rest[k]; k++; }
        first[k] = 0;
        if (env_get(e, first) || dict_find(first) || verb_find(e, first)) return;   /* a variable, a Common word or a verb: let it parse */
        const char *after = rest + k; while (*after == ' ') after++;
        if (!strncmp(after, "of ", 3)) return;                         /* a column or a function: name of post, year of today */
        for (Env *x = e; x; x = x->parent)
            for (int i = 0; i < x->n; i++) if (edit_distance(first, x->vars[i].name) <= 2) return;   /* probably a typo of a name */
    }
    char quoted[600]; rest_of_line_x(e, skip, quoted, sizeof quoted, 1);
    if (syn) fail("'%s' is not a Common word. Use 'show', and put the words in double quotes: show \"%s\"", cmd, quoted);
    fail("the words after '%s' need to be in double quotes: %s \"%s\"", prefix, prefix, quoted);
}

static Value parse_primary_inner(Env *e, Toks *T);
static Value parse_primary(Env *e, Toks *T) {     /* "count of count of ..." nests here */
    expr_enter();
    Value v = parse_primary_inner(e, T);
    g_expr_depth--;
    return v;
}
static Value parse_primary_inner(Env *e, Toks *T) {
    if (at_end(T)) fail("expected a value but the line ended");
    if (at_word(T, "try") && !(T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0)) {
        T->pos++;
        Value code = parse_primary(e, T); held_push(code);
        if (code.t != V_TEXT) fail("'try' needs Common code as text");
        Value r = run_sandbox(e, code.s);          /* still held: run_sandbox can fail before it runs */
        held_pop();
        v_free(&code);
        return r;
    }
    Tok *k = &T->t[T->pos];

    if (k->t == T_STR) { T->pos++; return fill_text(e, k->text); }
    if (k->t == T_NUM) { T->pos++; return v_num(k->num); }

    if (k->t == T_OP) {
        if (k->text[0] == '(') {
            T->pos++;
            Value v = parse_expr(e, T);
            if (!at_op(T, ')')) fail("missing closing parenthesis");
            T->pos++;
            return v;
        }
        fail("unexpected '%s'", k->text);
    }

    /* words */
    if (accept_word(T, "yes")) return v_bool(1);
    if (accept_word(T, "no"))  return v_bool(0);

#define NEXT_IS_OF (T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0)
#define NEXT_IS_VALUE (T->pos + 1 < T->n && (T->t[T->pos+1].t != T_OP || T->t[T->pos+1].text[0] == '('))
    if (at_word(T, "count") && NEXT_IS_OF) {
        T->pos++; expect_word(T, "of");
        Value v = parse_primary(e, T);
        if (v.t == V_RECORD) return record_field(&v, "count");       /* a column called count */
        double n = -1;
        if (v.t == V_LIST) n = v.count;
        else if (v.t == V_TEXT) { n = 0; for (const char *c = v.s; *c; c++) if (((unsigned char)*c & 0xC0) != 0x80) n++; }   /* letters, not bytes */
        if (n < 0) { held_push(v); fail("'count of' needs a list or text, not %s", type_name(v.t)); }
        v_free(&v);
        return v_num(n);
    }
    if ((at_word(T, "first") || at_word(T, "last")) && NEXT_IS_VALUE) {
        int first = at_word(T, "first"); T->pos++;
        /* first N of X / last N of X: a number, or a variable name, before 'of' */
        int howmany = -1;
        if (T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0 &&
            (T->t[T->pos].t == T_NUM || (T->t[T->pos].t == T_WORD && strcmp(T->t[T->pos].text, "of") != 0))) {
            Value n;
            if (T->t[T->pos].t == T_NUM) n = v_num(T->t[T->pos].num);
            else { Value *var = env_get(e, T->t[T->pos].text); if (!var) fail("I do not know what '%s' is", T->t[T->pos].text); n = v_copy(var); }
            if (n.t != V_NUM || n.n < 0) fail("'%s N of' needs a number", first ? "first" : "last");
            howmany = n.n > 2e9 ? 2000000000 : (int)n.n; v_free(&n); T->pos++;
        }
        expect_word(T, "of");
        Value v = parse_primary(e, T), r;
        if (howmany >= 0) {
            if (v.t == V_RECORD) fail("'%s N of' needs a list or text, not a record", first ? "first" : "last");
            if (v.t == V_LIST) {
                r = v_list();
                int take = howmany < v.count ? howmany : v.count;
                for (int i = 0; i < take; i++) list_add(&r, v_copy(&v.items[first ? i : v.count - take + i]));
                v_free(&v); return r;
            }
            v_make_text(&v);
            char *t = xstrdup(v.s);
            /* count letters, not bytes, so accents and emoji stay whole */
            size_t len = strlen(t), total = 0; for (size_t i = 0; i < len; i++) if (((unsigned char)t[i] & 0xC0) != 0x80) total++;
            size_t take = (size_t)howmany < total ? (size_t)howmany : total, start = 0, stop = len;
            if (first) { size_t seen = 0; for (stop = 0; stop < len; stop++) { if (((unsigned char)t[stop] & 0xC0) != 0x80) { if (seen == take) break; seen++; } } }
            else { size_t seen = 0; for (start = len; start > 0; start--) { if (((unsigned char)t[start-1] & 0xC0) != 0x80) { seen++; if (seen == take) { start--; break; } } } }
            char *piece = xstrndup(t + start, stop - start);
            r = v_text(piece); v_carry(&r, &v);
            free(piece); free(t); v_free(&v); return r;
        }
        if (v.t == V_RECORD) return record_field(&v, first ? "first" : "last");   /* a column called first or last */
        if (v.t == V_LIST) {
            if (v.count == 0) fail("the list is empty");
            r = v_copy(&v.items[first ? 0 : v.count - 1]);
        } else if (v.t == V_TEXT) {
            size_t n = strlen(v.s);
            if (n == 0) fail("the text is empty");
            size_t a = 0, b = n;                                     /* one letter, not one byte */
            if (first) { b = 1; while (b < n && ((unsigned char)v.s[b] & 0xC0) == 0x80) b++; }
            else { a = n - 1; while (a > 0 && ((unsigned char)v.s[a] & 0xC0) == 0x80) a--; }
            char *c = xstrndup(v.s + a, b - a);
            r = v_text(c); free(c); v_carry(&r, &v);
        } else fail("'%s of' needs a list or text, not %s", first ? "first" : "last", type_name(v.t));
        v_free(&v);
        return r;
    }
    /* item N of LIST */
    /* item N of LIST  (but "item of RECORD" with no number is a column called item) */
    if (at_word(T, "item") && NEXT_IS_VALUE && !NEXT_IS_OF) {
        T->pos++;
        Value n;
        /* "item position of list": the word before 'of' is the number, not a column */
        if (T->pos + 1 < T->n && T->t[T->pos].t == T_WORD && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0) {
            Value *var = env_get(e, T->t[T->pos].text);
            if (!var) fail("I do not know what '%s' is", T->t[T->pos].text);
            n = v_copy(var); T->pos++;
        } else n = parse_primary(e, T);
        expect_word(T, "of");
        held_push(n);
        Value v = parse_primary(e, T); held_push(v);
        if (n.t == V_TEXT && v.t == V_RECORD) { held_pop(); held_pop(); char key[128]; snprintf(key, sizeof key, "%s", n.s); v_free(&n); return record_field(&v, key); }
        if (n.t != V_NUM) fail("'item' needs a number (or, for a record, a column name in quotes), not %s", type_name(n.t));
        if (v.t != V_LIST) fail("'item ... of' needs a list, not %s", type_name(v.t));
        int i = n.n < 1 || n.n > v.count ? 0 : (int)n.n;
        if (i < 1 || i > v.count) fail("there is no item %.15g. The list has %d item%s", n.n, v.count, v.count == 1 ? "" : "s");
        held_pop(); held_pop();
        Value r = v_copy(&v.items[i - 1]);
        v_free(&n); v_free(&v);
        return r;
    }
    /* query of RECORD / path of RECORD: columns with those names */
    if ((at_word(T, "query") || at_word(T, "path")) && T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0) {
        const char *field = k->text; T->pos += 2;
        Value r = parse_primary(e, T);
        if (r.t != V_RECORD) { held_push(r); fail("'%s of' needs a record from the database, not %s", field, type_name(r.t)); }
        return record_field(&r, field);
    }
    if (at_word(T, "query")) { T->pos++;
        Value key = parse_primary(e, T);
        if (key.t != V_TEXT) fail("'query' needs the name of a field in quotes");
        Value r = query_lookup(e->query, key.s);
        v_free(&key);
        return r;
    }
    if (accept_word(T, "path")) { Value r = v_text(e->path ? e->path : ""); r.untrusted = 1; return r; }   /* the visitor typed the address */

    /* ask database "SQL" with VALUE, VALUE ... */
    if (accept_word(T, "ask")) {
        if (e->sandbox) fail("'ask database' is not allowed in practice code. Everything else is");
        expect_word(T, "database");
        Value sql = parse_primary(e, T); held_push(sql);
        if (sql.t != V_TEXT) fail("'ask database' needs the SQL in quotes");
        if (sql.untrusted || sql.tainted) fail("the SQL has a visitor's value inside it, and a visitor could change the query. Put the value after 'with' and write ? in its place: ask database \"select * from posts where slug = ?\" with slug");
        Value params = parse_with(e, T); held_push(params);
        Value rows = db_ask(e, sql.s, &params);
        held_pop(); held_pop();
        v_free(&sql); v_free(&params);
        return rows;
    }

    /* form "field": submitted form data, untrusted like query */
    if (at_word(T, "form") && !(T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0)) {
        T->pos++;
        Value key = parse_primary(e, T);
        if (key.t != V_TEXT) fail("'form' needs the name of a field in quotes");
        Value r = query_lookup(e->body, key.s);
        v_free(&key);
        return r;
    }
    if (at_word(T, "remembered") && !(T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0)) {
        T->pos++;
        Value key = parse_primary(e, T);
        if (key.t != V_TEXT) fail("'remembered' needs a name in quotes, like remembered \"user\"");
        const char *v = session_get(e, key.s);
        Value r = v_text(v ? v : ""); r.untrusted = 1;
        v_free(&key);
        return r;
    }
    if (accept_word(T, "dictionary")) {
        Value list = v_list();
        const Word *sorted[NWORDS]; int n = 0;                 /* alphabetical, only words with a form */
        for (size_t i = 0; i < NWORDS; i++) if (DICTIONARY[i].form) sorted[n++] = &DICTIONARY[i];
        for (int i = 1; i < n; i++) { const Word *k = sorted[i]; int j = i - 1; while (j >= 0 && strcmp(sorted[j]->word, k->word) > 0) { sorted[j+1] = sorted[j]; j--; } sorted[j+1] = k; }
        for (int i = 0; i < n; i++) {
            Value r = v_record();
            record_add(&r, "word", v_text(sorted[i]->word));
            record_add(&r, "form", v_text(sorted[i]->form));
            record_add(&r, "meaning", v_text(sorted[i]->meaning));
            record_add(&r, "example", v_text(sorted[i]->example ? sorted[i]->example : ""));
            record_add(&r, "command", v_bool(sorted[i]->cmd));
            list_add(&list, r);
        }
        return list;
    }
    if (accept_word(T, "today")) { char b[32]; time_t t = time(NULL); strftime(b, sizeof b, "%Y-%m-%d", localtime(&t)); return v_text(b); }
    if (accept_word(T, "now"))   { char b[32]; time_t t = time(NULL); strftime(b, sizeof b, "%Y-%m-%d %H:%M", localtime(&t)); return v_text(b); }
    if (at_word(T, "empty") && T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "list") == 0) {
        T->pos += 2; return v_list();
    }

    /* number of X, uppercase of X, lowercase of X, trim of X, paragraphs of X.
       If X is a record, these read the column of that name instead. */
    if (NEXT_IS_OF &&
        (at_word(T, "number") || at_word(T, "uppercase") || at_word(T, "lowercase") || at_word(T, "trim") || at_word(T, "paragraphs") ||
         at_word(T, "hash") || at_word(T, "sort") || at_word(T, "reverse") || at_word(T, "date") || at_word(T, "year") || at_word(T, "month") || at_word(T, "day"))) {
        const char *fn = k->text; T->pos += 2;
        Value v = parse_primary(e, T);
        if (v.t == V_RECORD) return record_field(&v, fn);
        if (!strcmp(fn, "sort") || !strcmp(fn, "reverse")) {
            if (v.t != V_LIST) { held_push(v); fail("'%s of' needs a list, not %s", fn, type_name(v.t)); }
            if (!strcmp(fn, "reverse")) { for (int i = 0, j = v.count - 1; i < j; i++, j--) { Value t = v.items[i]; v.items[i] = v.items[j]; v.items[j] = t; } return v; }
            for (int i = 1; i < v.count; i++) {                     /* insertion sort: lists on a page are small */
                Value key = v.items[i]; int j = i - 1;
                while (j >= 0 && v_compare(&v.items[j], &key) > 0) { v.items[j + 1] = v.items[j]; j--; }
                v.items[j + 1] = key;
            }
            return v;
        }
        if (!strcmp(fn, "hash")) {
            if (e->sandbox) { held_push(v); fail("'hash of' is not allowed in practice code: it is slow on purpose, to protect passwords"); }
            if (v.t != V_TEXT) { held_push(v); fail("'hash of' needs text, not %s", type_name(v.t)); }
            char *h = password_hash(v.s); v_free(&v); Value r = v_text(h); free(h); return r;
        }
        if (!strcmp(fn, "date") || !strcmp(fn, "year") || !strcmp(fn, "month") || !strcmp(fn, "day")) {
            if (v.t != V_TEXT) { held_push(v); fail("'%s of' needs a date in text, like \"2026-10-24\"", fn); }
            int y, m, d;
            static const int days_in[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            if (sscanf(v.s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > days_in[m - 1] ||
                (m == 2 && d == 29 && !(y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))))
                { held_push(v); fail("\"%s\" is not a date like 2026-10-24", v.s); }
            static const char *months[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
            v_free(&v);
            if (!strcmp(fn, "year")) return v_num(y);
            if (!strcmp(fn, "day"))  return v_num(d);
            if (!strcmp(fn, "month")) return v_text(months[m - 1]);
            char out[64]; snprintf(out, sizeof out, "%d %s %d", d, months[m - 1], y);
            return v_text(out);
        }
        if (!strcmp(fn, "number")) {
            if (v.t == V_NUM) return v;
            if (v.t == V_BOOL) { v.t = V_NUM; return v; }
            if (v.t != V_TEXT) { held_push(v); fail("'number of' needs text, not %s", type_name(v.t)); }
            const char *q = v.s; while (isspace((unsigned char)*q)) q++;
            char *end; double d = strtod(q, &end);
            while (isspace((unsigned char)*end)) end++;
            if (end == q || *end) { held_push(v); fail("\"%s\" is not a number", v.s); }
            v_free(&v); return v_num(d);
        }
        v_make_text(&v);                          /* numbers and lists become text first */
        if (!strcmp(fn, "uppercase")) { for (char *c = v.s; *c; c++) *c = (char)toupper((unsigned char)*c); return v; }
        if (!strcmp(fn, "lowercase")) { for (char *c = v.s; *c; c++) *c = (char)tolower((unsigned char)*c); return v; }
        if (!strcmp(fn, "trim")) {
            char *a = v.s; while (isspace((unsigned char)*a)) a++;
            size_t n = strlen(a); while (n && isspace((unsigned char)a[n-1])) n--;
            char *t = xstrndup(a, n); Value r = v_text(t); free(t); v_carry(&r, &v); v_free(&v); v = r;
            return v;
        }
        /* paragraphs: split on blank lines, trim each, skip empties */
        Value list = v_list();
        const char *p = v.s;
        while (*p) {
            const char *q = strstr(p, "\n\n");
            size_t n = q ? (size_t)(q - p) : strlen(p);
            const char *a = p; size_t m = n;
            while (m && isspace((unsigned char)*a)) { a++; m--; }
            while (m && isspace((unsigned char)a[m-1])) m--;
            if (m) { char *t = xstrndup(a, m); Value item = v_text(t); v_carry(&item, &v); list_add(&list, item); free(t); }
            p = q ? q + 2 : p + n;
        }
        v_free(&v);
        return list;
    }

    /* split TEXT by SEP */
    if (at_word(T, "split") && NEXT_IS_VALUE && !NEXT_IS_OF) {
        T->pos++;
        Value v = parse_primary(e, T); held_push(v);
        if (!accept_word(T, "by")) fail("'split' is written: split TEXT by \"/\"");
        Value sep = parse_primary(e, T); held_push(sep);
        if (sep.t != V_TEXT || !sep.s[0]) fail("'split ... by' needs a separator in quotes, like \"/\" or \",\"");
        held_pop(); held_pop();
        v_make_text(&v);
        Value list = v_list();
        const char *p = v.s; size_t sl = strlen(sep.s);
        while (*p) {
            const char *q = strstr(p, sep.s);
            size_t n = q ? (size_t)(q - p) : strlen(p);
            if (n) { char *t = xstrndup(p, n); Value item = v_text(t); v_carry(&item, &v); list_add(&list, item); free(t); }
            p = q ? q + sl : p + n;
        }
        v_free(&v); v_free(&sep);
        return list;
    }

    /* replace A with B in TEXT */
    if (at_word(T, "replace") && NEXT_IS_VALUE && !NEXT_IS_OF) {
        T->pos++;
        Value a = parse_primary(e, T); held_push(a);
        if (!accept_word(T, "with")) fail("'replace' is written: replace \"old\" with \"new\" in TEXT");
        Value b = parse_primary(e, T); held_push(b);
        if (!accept_word(T, "in")) fail("'replace' is written: replace \"old\" with \"new\" in TEXT");
        Value v = parse_primary(e, T); held_push(v);
        if (a.t != V_TEXT || !a.s[0]) fail("'replace' needs the text to look for in quotes");
        held_pop(); held_pop(); held_pop();
        char *bt = v_to_text(&b);
        v_make_text(&v);
        Buf *out = buf_new();
        const char *p = v.s; size_t al = strlen(a.s);
        while (*p) {
            const char *q = strstr(p, a.s);
            if (!q) { buf_puts(out, p); break; }
            buf_putn(out, p, (size_t)(q - p)); buf_puts(out, bt); p = q + al;
        }
        Value r = v_text(out->data); v_carry(&r, &v); v_carry(&r, &b);
        buf_free(out); free(bt); v_free(&a); v_free(&b); v_free(&v);
        return r;
    }

    /* COLUMN of RECORD, e.g. name of recipe */
    if (T->pos + 1 < T->n && T->t[T->pos+1].t == T_WORD && strcmp(T->t[T->pos+1].text, "of") == 0) {
        const char *field = k->text; T->pos += 2;
        Value r = parse_primary(e, T);
        if (r.t != V_RECORD) {
            held_push(r);
            if (r.t == V_LIST) fail("'%s of' needs one record, but this is a list of %d. Use 'first of' or 'for each' to pick one", field, r.count);
            fail("'%s of' needs a record from the database, not %s", field, type_name(r.t));
        }
        return record_field(&r, field);
    }

    /* a verb used as a value: slug_of title */
    {
        Verb *vb = verb_find(e, k->text);
        if (vb) {
            T->pos++;
            Value args = v_list(); held_push(args);
            for (int a = 0; a < vb->argc; a++) {
                if (at_end(T)) fail("'%s' takes %d value%s. It is written: %s", vb->name, vb->argc, vb->argc == 1 ? "" : "s", verb_form(vb));
                list_add(&args, parse_primary(e, T)); held_pop(); held_push(args);
            }
            Value r = verb_call(e, vb, &args, NULL, 1);
            held_pop(); v_free(&args);
            return r;
        }
    }

    /* a variable */
    T->pos++;
    Value *v = env_get(e, k->text);
    if (!v) {
        if (verb_find(e, k->text)) fail("'%s' is a verb. Call it with its values: %s", k->text, verb_form(verb_find(e, k->text)));
        const char *w = k->text, *syn = synonym(w);
        char lc[64]; lowercase(w, lc, sizeof lc);
        if (syn) fail("'%s' is not a Common word. Use '%s'", w, syn);
        if (dict_find(w)) fail("'%s' on its own is a name, and nothing is set to it yet. As a Common word it is written: %s", w, dict_find(w)->form ? dict_find(w)->form : "inside another command");
        if (dict_find(lc)) fail("Common words are always lowercase. Write '%s'", lc);
        const char *best = NULL; int bd = 3; int best_is_var = 0;
        for (Env *x = e; x; x = x->parent)
            for (int i = 0; i < x->n; i++) { int d = edit_distance(w, x->vars[i].name); if (d < bd) { bd = d; best = x->vars[i].name; best_is_var = 1; } }
        if (!best) best = nearest_word(w, 0);
        const char *hint = e->in_hole ? " If you meant a real bracket in the text, write \\[" : "";
        if (best && best_is_var) fail("I do not know what '%s' is. Did you mean '%s'?%s", w, best, hint);
        if (best && !e->in_hole) fail("I do not know what '%s' is. Did you mean '%s'? If you meant the word itself, write it in double quotes: \"%s\"", w, best, w);
        if (best) fail("I do not know what '%s' is. Did you mean '%s'?%s", w, best, hint);
        if (e->hint_prefix && e->line_text && !e->in_hole) {        /* inside [brackets] the words are already in quotes */
            char rest[512]; rest_of_line(e, e->hint_skip, rest, sizeof rest);
            char prefix[96];
            if (e->hint_name) snprintf(prefix, sizeof prefix, "set %s to", e->hint_name); else snprintf(prefix, sizeof prefix, "%s", e->hint_prefix);
            fail("I do not know what '%s' is. If you meant the words themselves, put them in double quotes: %s \"%s\"", w, prefix, rest);
        }
        if (!e->in_hole) fail("I do not know what '%s' is. If you meant the word itself, write it in double quotes: \"%s\". If it is a name, set it first with: set %s to ...", w, w, w);
        fail("I do not know what '%s' is. Set it first with: set %s to ...%s", w, w, hint);
    }
    return v_copy(v);
}

static Value parse_unary(Env *e, Toks *T) {
    expr_enter();
    if (at_op(T, '-')) {
        T->pos++;
        Value v = parse_unary(e, T);
        if (v.t != V_NUM) fail("can only negate a number, not %s", type_name(v.t));
        v.n = -v.n;
        { Value ret_ = v; g_expr_depth--; return ret_; }
    }
    { Value ret_ = parse_primary(e, T); g_expr_depth--; return ret_; }
}

static Value parse_mul(Env *e, Toks *T) {
    Value left = parse_unary(e, T);
    while (at_op(T, '*') || at_op(T, '/')) {
        char op = T->t[T->pos++].text[0];
        held_push(left);
        Value right = parse_unary(e, T);
        held_push(right);
        if (left.t != V_NUM || right.t != V_NUM)
            fail("can only %s numbers, not %s and %s", op == '*' ? "multiply" : "divide", type_name(left.t), type_name(right.t));
        if (op == '/' && right.n == 0) fail("cannot divide by zero");
        held_pop(); held_pop();
        left.n = op == '*' ? left.n * right.n : left.n / right.n;
        v_free(&right);
    }
    return left;
}

static Value parse_add(Env *e, Toks *T) {
    Value left = parse_mul(e, T);
    while (at_op(T, '+') || at_op(T, '-')) {
        char op = T->t[T->pos++].text[0];
        held_push(left);
        Value right = parse_mul(e, T);
        held_push(right);
        if (op == '-') {
            if (left.t != V_NUM || right.t != V_NUM)
                fail("can only subtract numbers, not %s and %s", type_name(left.t), type_name(right.t));
            left.n -= right.n;
        } else if (left.t == V_NUM && right.t == V_NUM) {
            left.n += right.n;
        } else {
            fail("'+' adds numbers, but got %s and %s. To put a value in text, write it in brackets: \"Hello [name]\"",
                 type_name(left.t), type_name(right.t));
        }
        held_pop(); held_pop();
        v_free(&right);
    }
    return left;
}

/* Order two values for is more than / is less than / is at least / is at most /
   is between. A number against text written as a number compares as numbers,
   so query "age" is more than 12 works; against other text it is an error,
   because "9" is more than "12" as text, which is never what was meant. */
static int compare_ordered(const Value *a, const Value *b, const char *how) {
    double x, y;
    if (a->t == V_NUM || b->t == V_NUM) {
        if (as_number(a, &x) && as_number(b, &y)) return x < y ? -1 : x > y ? 1 : 0;
        const Value *t = a->t == V_NUM ? b : a, *n = a->t == V_NUM ? a : b;
        char *nt = v_to_text(n), buf[200];
        if (t->t == V_TEXT && !t->s[0]) snprintf(buf, sizeof buf, "'%s' needs a number to compare with %s, but got empty text. A form field left blank is empty: check first with 'if NAME is empty'", how, nt);
        else if (t->t == V_TEXT) snprintf(buf, sizeof buf, "'%s' cannot compare \"%.40s\" with the number %s, because it is not a number", how, t->s, nt);
        else snprintf(buf, sizeof buf, "'%s' cannot compare %s with the number %s", how, type_name(t->t), nt);
        free(nt); fail("%s", buf);
    }
    return v_compare(a, b);
}

static Value parse_compare(Env *e, Toks *T) {
    int start = T->pos;
    Value left = parse_add(e, T);
    held_push(left);
    /* the left side's name, when it is a single word, for teaching errors */
    const char *left_name = (T->pos == start + 1 && T->t[start].t == T_WORD) ? T->t[start].text : NULL;
    if (accept_word(T, "contains")) {
        Value r = parse_add(e, T); int found = 0; held_pop();
        if (left.t == V_LIST) { for (int i = 0; i < left.count; i++) if (v_equal(&left.items[i], &r)) found = 1; }
        else { char *a = v_to_text(&left), *b = v_to_text(&r); found = b[0] == 0 || strstr(a, b) != NULL; free(a); free(b); }
        v_free(&left); v_free(&r); return v_bool(found);
    }
    if (accept_word(T, "matches")) {
        if (e->sandbox) fail("'matches' is not allowed in practice code: it is slow on purpose, to protect passwords");
        Value r = parse_add(e, T); held_push(r);
        if (left.t != V_TEXT || r.t != V_TEXT) fail("'matches' compares text with a hash made by 'hash of'");
        char log[4096]; time_t now = time(NULL);
        if (g_remote[0]) {                                         /* a real visitor: limit wrong guesses */
            snprintf(log, sizeof log, "%s/login.log", private_base(e->dir));
            if (login_recent(log, now) >= LOGIN_TRIES)
                fail("too many wrong passwords from this address. Wait %d minutes and try again", LOGIN_WINDOW / 60);
        }
        held_pop(); held_pop();
        int ok = password_matches(left.s, r.s);
        if (!ok && g_remote[0]) { FILE *lf = fopen(log, "a"); if (lf) { fprintf(lf, "%lld %s\n", (long long)now, g_remote); fclose(lf); } }
        v_free(&left); v_free(&r); return v_bool(ok);
    }
    if (!accept_word(T, "is")) { held_pop(); return left; }

    int result, negate = 0;
    if (accept_word(T, "not")) negate = 1;

    if (accept_word(T, "empty")) {
        result = !v_truthy(&left);
    } else if (accept_word(T, "an")) {
        expect_word(T, "email");
        char *t = v_to_text(&left); result = left.t == V_TEXT && is_email(t); free(t);
    } else if (accept_word(T, "between")) {
        Value lo = parse_add(e, T); held_push(lo);
        if (!accept_word(T, "and")) fail("'is between' is written: X is between LOW and HIGH");
        Value hi = parse_add(e, T); held_push(hi);
        result = compare_ordered(&left, &lo, "is between") >= 0 && compare_ordered(&left, &hi, "is between") <= 0;
        held_pop(); held_pop();
        v_free(&lo); v_free(&hi);
    } else if (accept_word(T, "more")) {
        expect_word(T, "than");
        Value r = parse_add(e, T); held_push(r); result = compare_ordered(&left, &r, "is more than") > 0; held_pop(); v_free(&r);
    } else if (accept_word(T, "less")) {
        expect_word(T, "than");
        Value r = parse_add(e, T); held_push(r); result = compare_ordered(&left, &r, "is less than") < 0; held_pop(); v_free(&r);
    } else if (accept_word(T, "at")) {
        int least = accept_word(T, "least");
        if (!least) expect_word(T, "most");
        Value r = parse_add(e, T); held_push(r);
        int c = compare_ordered(&left, &r, least ? "is at least" : "is at most"); result = least ? c >= 0 : c <= 0; held_pop(); v_free(&r);
    } else {
        Value r = parse_add(e, T); held_push(r);
        if (left.t == V_LIST && r.t != V_LIST && r.t != V_RECORD) {
            char *want = v_to_text(&r); char hint[400];
            snprintf(hint, sizeof hint, "%s is a list of %d item%s, so it cannot be one %s. To ask whether it holds %s%s%s, write: %s%s contains %s%s%s",
                     left_name ? left_name : "the left side", left.count, left.count == 1 ? "" : "s", r.t == V_NUM ? "number" : "value",
                     r.t == V_TEXT ? "\"" : "", want, r.t == V_TEXT ? "\"" : "",
                     negate ? "not " : "", left_name ? left_name : "LIST", r.t == V_TEXT ? "\"" : "", want, r.t == V_TEXT ? "\"" : "");
            free(want); fail("%s", hint);
        }
        held_pop();
        result = v_equal(&left, &r); v_free(&r);
    }
    held_pop();
    v_free(&left);
    return v_bool(negate ? !result : result);
}

static Value parse_not(Env *e, Toks *T) {
    expr_enter();
    if (accept_word(T, "not")) {
        Value v = parse_not(e, T);
        int r = !v_truthy(&v); v_free(&v);
        { Value ret_ = v_bool(r); g_expr_depth--; return ret_; }
    }
    { Value ret_ = parse_compare(e, T); g_expr_depth--; return ret_; }
}

static Value parse_and(Env *e, Toks *T) {
    Value left = parse_not(e, T);
    while (accept_word(T, "and")) {
        held_push(left);
        Value right = parse_not(e, T);
        held_pop();
        int r = v_truthy(&left) && v_truthy(&right);
        v_free(&left); v_free(&right); left = v_bool(r);
    }
    return left;
}

static Value parse_expr(Env *e, Toks *T) {
    expr_enter();
    Value left = parse_and(e, T);
    while (accept_word(T, "or")) {
        held_push(left);
        Value right = parse_and(e, T);
        held_pop();
        int r = v_truthy(&left) || v_truthy(&right);
        v_free(&left); v_free(&right); left = v_bool(r);
    }
    g_expr_depth--;
    return left;
}

/* ================================================================== */
/* Statements                                                          */
/* ================================================================== */

/* One line of code. The line is split into tokens the first time it runs and
   the tokens are kept (toks, ntoks), so a loop does not re-read its own lines;
   kind caches line_kind. Parsing only reads tokens, so one copy is shared by
   every run of the line, even by a verb that calls itself. */
typedef struct { const char *text; size_t len; int lineno; const char *raw; size_t rawlen; Tok *toks; int ntoks; int kind; } SrcLine;
typedef struct Lines { SrcLine *l; int n, cap; struct Lines *retired; char *retired_body; } Lines;

/* Bodies of verbs that were defined again. Freed when the page is done. */
static Lines *g_retired;
static void lines_retire(void *lines, char *body) {
    Lines *L = lines; L->retired_body = body;
    Lines *last = L; while (last->retired) last = last->retired;
    last->retired = g_retired; g_retired = L;
}
static void lines_free(void *lines) {
    Lines *L = lines;
    while (L) {
        for (int i = 0; i < L->n; i++) if (L->l[i].toks) {
            for (int k = 0; k < L->l[i].ntoks; k++) free(L->l[i].toks[k].text);
            free(L->l[i].toks);
        }
        free(L->l); free(L->retired_body);
        Lines *next = L->retired; free(L); L = next;
    }
}

static void split_lines(const char *src, int first_line, Lines *L) {
    memset(L, 0, sizeof *L);
    const char *p = src; int lineno = first_line;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *s = p; size_t n = len;
        while (n && isspace((unsigned char)*s)) { s++; n--; }
        while (n && isspace((unsigned char)s[n - 1])) n--;
        if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 32; L->l = realloc(L->l, sizeof(SrcLine) * (size_t)L->cap); }
        L->l[L->n].text = s; L->l[L->n].len = n; L->l[L->n].lineno = lineno; L->l[L->n].raw = p; L->l[L->n].rawlen = len;
        L->l[L->n].toks = NULL; L->l[L->n].ntoks = -1; L->l[L->n].kind = -1; L->n++;
        p = nl ? nl + 1 : p + len; lineno++;
    }
}

/* First word of a line, lowercase assumed. Returns "" if none. */
static void first_word(const SrcLine *L, char *out, size_t outsz) {
    size_t i = 0, j = 0;
    while (i < L->len && j + 1 < outsz && (isalnum((unsigned char)L->text[i]) || L->text[i] == '_')) out[j++] = L->text[i++];
    out[j] = 0;
}

enum { K_OTHER, K_IF, K_FOR, K_REPEAT, K_OTHERWISE, K_END, K_TEXT, K_TO, K_RECORD, K_EMAIL };

/* Is this line a bare "show", "show raw", or "set NAME to" with nothing after it? */
static int opens_text_block(const SrcLine *L) {
    const char *t = L->text; size_t n = L->len;
    while (n && (t[n-1] == '.' || isspace((unsigned char)t[n-1]))) n--;     /* "show." is fine too */
    if (n == 4 && !strncmp(t, "show", 4)) return 1;
    if (n == 8 && !strncmp(t, "show raw", 8)) return 1;
    if (n > 7 && !strncmp(t, "set ", 4) && (isalpha((unsigned char)t[4]) || t[4] == '_') && !strncmp(t + n - 3, " to", 3)) {
        for (size_t i = 4; i < n - 3; i++) if (!(isalnum((unsigned char)t[i]) || t[i] == '_')) return 0;
        return 1;
    }
    return 0;
}
/* "set NAME to record" with nothing after it */
static int opens_record_block(const SrcLine *L) {
    const char *t = L->text; size_t n = L->len;
    while (n && (t[n-1] == '.' || isspace((unsigned char)t[n-1]))) n--;
    if (n > 14 && !strncmp(t, "set ", 4) && (isalpha((unsigned char)t[4]) || t[4] == '_') && !strncmp(t + n - 10, " to record", 10)) {
        for (size_t i = 4; i < n - 10; i++) if (!(isalnum((unsigned char)t[i]) || t[i] == '_')) return 0;
        return 1;
    }
    return 0;
}

static int line_kind_uncached(const SrcLine *L);
static int line_kind(const SrcLine *L) {
    if (L->kind < 0) ((SrcLine *)L)->kind = line_kind_uncached(L);
    return L->kind;
}
static int line_kind_uncached(const SrcLine *L) {
    char w[32]; first_word(L, w, sizeof w);
    if (opens_record_block(L))   return K_RECORD;
    if (!strcmp(w, "send"))      return K_EMAIL;      /* send email to ... then lines, then end */
    if (opens_text_block(L))     return K_TEXT;
    if (!strcmp(w, "to"))        return K_TO;
    if (!strcmp(w, "if"))        return K_IF;
    if (!strcmp(w, "for"))       return K_FOR;
    if (!strcmp(w, "repeat"))    return K_REPEAT;
    if (!strcmp(w, "otherwise")) return K_OTHERWISE;
    if (!strcmp(w, "end"))       return K_END;
    return K_OTHER;
}
static int is_opener(int k) { return k == K_IF || k == K_FOR || k == K_REPEAT || k == K_TO; }

/* Is the line just 'end' (or 'end.')? Inside a text block, "end of story" is text. */
static int is_bare_end(const SrcLine *L) {
    size_t n = L->len;
    while (n && (L->text[n-1] == '.' || isspace((unsigned char)L->text[n-1]))) n--;
    return n == 3 && !strncmp(L->text, "end", 3);
}

/* The 'end' that closes a text block opened at line i: the next line that is just 'end'. */
static int find_text_end(Lines *L, int i) {
    for (int j = i + 1; j < L->n; j++) if (is_bare_end(&L->l[j])) return j;
    cur_line = L->l[i].lineno;
    fail("this text block is never closed. Add a line with just 'end' after the text");
    return -1;
}

/* Index of the line that closes the block opened at line i. */
static int find_block_end(Lines *L, int i) {
    int depth = 0;
    for (int j = i + 1; j < L->n; j++) {
        int k = line_kind(&L->l[j]);
        if (k == K_TEXT || k == K_RECORD || k == K_EMAIL) { j = find_text_end(L, j); continue; }     /* text inside a show or record block is not code */
        if (is_opener(k)) depth++;
        else if (k == K_END) { if (depth == 0) return j; depth--; }
    }
    cur_line = L->l[i].lineno;
    int k = line_kind(&L->l[i]);
    if (k == K_TO) fail("this verb is never closed. Add a line with just 'end' after its lines");
    fail("this '%s' is never closed. Add a line with just 'end' after the lines it should %s",
         k == K_FOR ? "for each" : "repeat", k == K_FOR ? "run for each item" : "repeat");
    return -1;
}

/* Next 'otherwise' or 'end' at this nesting level, starting at line j. */
static int find_clause(Lines *L, int j, int open_line) {
    int depth = 0;
    for (; j < L->n; j++) {
        int k = line_kind(&L->l[j]);
        if (k == K_TEXT || k == K_RECORD || k == K_EMAIL) { j = find_text_end(L, j); continue; }
        if (is_opener(k)) depth++;
        else if (k == K_END) { if (depth == 0) return j; depth--; }
        else if (k == K_OTHERWISE && depth == 0) return j;
    }
    cur_line = L->l[open_line].lineno;
    fail("this 'if' is never closed. Add a line with just 'end' after the lines it decides about");
    return -1;
}

static void cl_process_env(const char *html, Env *env, Buf *out);
static void exec_lines(Env *e, Lines *L, int from, int to, Buf *out);

static const char *hl_code(const char *p, const char *end, Buf *out, int stop_at_bracket);

/* The text of a block: lines i+1 .. end-1, as typed, with the common indentation
   removed and [holes] filled. *tainted says whether a visitor's value was
   filled in. Caller frees. */
static char *text_block(Env *e, Lines *L, int i, int end, int *tainted) {
    *tainted = 0;
    size_t indent = (size_t)-1;
    for (int j = i + 1; j < end; j++) {
        if (L->l[j].len == 0) continue;
        size_t k = 0; while (k < L->l[j].rawlen && (L->l[j].raw[k] == ' ' || L->l[j].raw[k] == '\t')) k++;
        if (k < indent) indent = k;
    }
    if (indent == (size_t)-1) indent = 0;
    Buf *b = buf_new();
    for (int j = i + 1; j < end; j++) {
        const char *raw = L->l[j].raw; size_t n = L->l[j].rawlen;
        size_t k = 0; while (k < n && k < indent && (raw[k] == ' ' || raw[k] == '\t')) k++;
        /* wrap the line in quotes so the usual text scanner handles \[ and holes; a real quote stays a quote */
        Buf *q = buf_new(); buf_putc(q, '"');
        int depth = 0;                                           /* quotes inside a [hole] belong to the hole */
        for (size_t m = k; m < n; m++) {
            if (raw[m] == '\\' && m + 1 < n) { buf_putc(q, raw[m]); buf_putc(q, raw[m+1]); m++; continue; }
            if (raw[m] == '[') depth++;
            else if (raw[m] == ']' && depth > 0) depth--;
            else if (raw[m] == '"' && depth == 0) buf_putc(q, '\\');
            buf_putc(q, raw[m]);
        }
        buf_putc(q, '"');
        cur_line = L->l[j].lineno;
        Toks U; tokenize(q->data, q->len, &U);
        Value v = fill_text(e, U.t[0].text);
        if (v.untrusted || v.tainted) *tainted = 1;
        buf_puts(b, v.s); buf_putc(b, '\n');
        v_free(&v); toks_free(&U); buf_free(q);
    }
    if (b->len) b->len--;                                      /* no newline after the last line */
    b->data[b->len] = 0;
    char *t = xstrdup(b->data); buf_free(b);
    return t;
}

static const char *verb_form(Verb *vb) {
    static char form[256];
    size_t n = (size_t)snprintf(form, sizeof form, "%s", vb->name);
    for (int a = 0; a < vb->argc && n < sizeof form - 2; a++) n += (size_t)snprintf(form + n, sizeof form - n, " %s", vb->args[a]);
    return form;
}

/* Run a verb. The verb sees a copy of the caller's variables plus its arguments;
   what it sets stays inside. 'show' goes to the caller's output. */
static Value verb_call(Env *e, Verb *vb, Value *args, Buf *out, int as_value) {
    if (e->depth >= 100) fail("'%s' calls itself too deeply (more than 100 levels)", vb->name);
    /* The verb's own environment lives on the heap and is noted in g_frames, so
       that an error inside the verb (a longjmp past this frame) can still free it. */
    Env *c = calloc(1, sizeof *c);
    if (!vb->lines) { Lines *nl = calloc(1, sizeof *nl); split_lines(vb->body, vb->first_line, nl); vb->lines = nl; }
    Lines *L = vb->lines;                           /* split once, kept with the verb */
    if (g_nframes < 128) { g_frames[g_nframes].env = c; g_frames[g_nframes].lines = NULL; g_nframes++; }
    c->query = e->query; c->body = e->body; c->path = e->path; c->cookie = e->cookie;
    snprintf(c->dir, sizeof c->dir, "%s", e->dir);
    c->include_depth = e->include_depth; c->sandbox = e->sandbox; c->steps = e->steps;
    c->parent = e; c->root = e->root; c->depth = e->depth + 1;
    c->out = out ? out : e->out;
    for (int a = 0; a < vb->argc; a++) env_set(c, vb->args[a], v_copy(&args->items[a]));

    int saved_line = cur_line;
    exec_lines(c, L, 0, L->n, c->out);
    cur_line = saved_line;
    e->steps = c->steps;                            /* the database, if the verb opened one, is on the root already */

    Value r; int gave = c->has_gave;
    if (gave) { r = c->gave; memset(&c->gave, 0, sizeof c->gave); c->has_gave = 0; }
    else r = v_bool(0);
    if (g_nframes > 0 && g_frames[g_nframes - 1].env == c) g_nframes--;
    env_free(c); free(c);
    if (!gave && as_value) fail("'%s' does not give a value. Add 'give VALUE' inside it, or call it on its own line", vb->name);
    return r;
}

/* Build a record from "column: VALUE" lines i+1 .. end-1. */
static Value record_block(Env *e, Lines *L, int i, int end, int email) {
    Value r = v_record();
    for (int j = i + 1; j < end; j++) {
        const char *t = L->l[j].text; size_t n = L->l[j].len;
        if (n == 0 || t[0] == '#') continue;
        cur_line = L->l[j].lineno;
        const char *colon = memchr(t, ':', n);
        if (!colon) fail("each line of a record is written: column: VALUE");
        size_t kl = (size_t)(colon - t); while (kl && isspace((unsigned char)t[kl-1])) kl--;
        char key[128]; if (kl == 0 || kl >= sizeof key) fail("each line of a record is written: column: VALUE");
        memcpy(key, t, kl); key[kl] = 0;
        if (email && !strcmp(key, "reply to")) strcpy(key, "reply_to");
        if (email && strcmp(key, "subject") && strcmp(key, "body") && strcmp(key, "reply_to") && strcmp(key, "from"))
            fail("an email is written with subject:, body:, reply to: and from: lines, not '%s:'", key);
        for (size_t m = 0; m < kl; m++) if (!(isalnum((unsigned char)key[m]) || key[m] == '_')) fail("a column name is one word, like price or first_name, not '%s'", key);
        Toks U; tokenize(colon + 1, n - kl - 1 - (size_t)(colon - t - (long)kl), &U);
        if (U.n == 0) fail("the column '%s' has no value after the colon", key);
        Value v = parse_expr(e, &U);
        if (at_op(&U, ',')) {                                      /* a list in a column: tags: "a", "b" */
            Value list = v_list(); list_add(&list, v);
            while (at_op(&U, ',')) { U.pos++; list_add(&list, parse_expr(e, &U)); }
            v = list;
        }
        expect_line_end(&U); toks_free(&U);
        int found = 0;
        for (int k = 0; k < r.count; k++) if (strcmp(r.keys[k], key) == 0) { v_free(&r.items[k]); r.items[k] = v; found = 1; }
        if (!found) record_add(&r, key, v);
    }
    return r;
}

/* Subject text for a header: line breaks become spaces, and anything
   beyond plain ASCII is wrapped as =?UTF-8?B?...?= so mail programs read it. */
static void mail_subject(Buf *b, const char *s) {
    char clean[400]; size_t n = 0; int ascii = 1;
    for (const char *p = s; *p && n < 200; p++) {
        unsigned char c = (unsigned char)*p;
        clean[n++] = c < 0x20 ? ' ' : (char)c;              /* no line breaks or control characters in a header */
        if (c >= 0x80) ascii = 0;
    }
    clean[n] = 0;
    if (ascii) { buf_puts(b, clean); return; }
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    buf_puts(b, "=?UTF-8?B?");
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)clean[i] << 16 | (i + 1 < n ? (unsigned char)clean[i+1] << 8 : 0) | (i + 2 < n ? (unsigned char)clean[i+2] : 0);
        buf_putc(b, b64[v >> 18 & 63]); buf_putc(b, b64[v >> 12 & 63]);
        buf_putc(b, i + 1 < n ? b64[v >> 6 & 63] : '='); buf_putc(b, i + 2 < n ? b64[v & 63] : '=');
    }
    buf_puts(b, "?=");
}

static const Value *email_field(const Value *r, const char *key) {
    for (int k = 0; k < r->count; k++) if (!strcmp(r->keys[k], key)) return &r->items[k];
    return NULL;
}

/* An address the page wrote itself: never a visitor's value, and one real address. */
static char *fixed_address(const Value *v, const char *what, const char *example) {
    if (v->t != V_TEXT) fail("'%s' needs an email address in quotes, like \"%s\"", what, example);
    if (v->untrusted || v->tainted)
        fail("'%s' cannot take an address that came from a visitor, or anyone could use your form to send email to anybody. Write the address yourself, like \"%s\". A visitor's address goes in 'reply to:'", what, example);
    if (!is_email(v->s)) fail("'%s' needs one email address, like \"%s\", not \"%s\"", what, example, v->s);
    return v->s;
}

/* How many emails this site sent in the last hour, from its private mail.log. */
static int mail_recent(const char *log, time_t now) {
    FILE *f = fopen(log, "r"); if (!f) return 0;
    int n = 0; char line[64];
    while (fgets(line, sizeof line, f)) if (now - (time_t)atoll(line) < 3600) n++;
    fclose(f); return n;
}

static NOINLINE void send_email(Env *e, const Value *to_v, const Value *r) {
    if (e->sandbox) fail("'send' is not allowed in practice code. Everything else is");
    const char *to = fixed_address(to_v, "send email to", "you@yoursite.com");

    const Value *subject = email_field(r, "subject"), *body = email_field(r, "body");
    const Value *reply = email_field(r, "reply_to"), *from_v = email_field(r, "from");
    if (!subject) fail("the email has no subject. Add a line like: subject: \"New message\"");
    if (!body)    fail("the email has no body. Add a line like: body: message");
    const char *from = NULL;
    if (from_v) from = fixed_address(from_v, "from:", "site@yoursite.com");
    else if (getenv("CL_MAIL_FROM") && is_email(getenv("CL_MAIL_FROM"))) from = getenv("CL_MAIL_FROM");
    else fail("say who the email is from: add a line like  from: \"site@yoursite.com\"  (an address on your own domain, so it is not marked as spam)");

    if (++g_mail_sent > MAIL_PER_PAGE) fail("a page can send at most %d emails each time it runs", MAIL_PER_PAGE);
    const char *base = private_base(e->dir);
    char log[4096]; snprintf(log, sizeof log, "%s/mail.log", base);
    time_t now = time(NULL);
    int per_hour = getenv("CL_MAIL_PER_HOUR") ? atoi(getenv("CL_MAIL_PER_HOUR")) : MAIL_PER_HOUR;
    if (mail_recent(log, now) >= per_hour)
        fail("this site already sent %d emails in the last hour, so this one was not sent. That stops a robot from using your form to flood an inbox", per_hour);

    char *subj = plain_text(subject), *text = plain_text(body);
    if (strlen(text) > 50000) { free(subj); free(text); fail("the email body is longer than 50000 letters"); }
    char *reply_s = reply ? plain_text(reply) : NULL;
    if (reply_s && !is_email(reply_s)) { free(reply_s); reply_s = NULL; }   /* not one address: leave Reply-To off rather than trust it */

    unsigned char rnd[12]; char id[25]; random_bytes(rnd, sizeof rnd); to_hex(rnd, sizeof rnd, id);
    char date[64]; strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
    const char *domain = strchr(from, '@') + 1;

    Buf *m = buf_new();
    buf_puts(m, "To: "); buf_puts(m, to); buf_puts(m, "\n");
    buf_puts(m, "From: "); buf_puts(m, from); buf_puts(m, "\n");
    if (reply_s) { buf_puts(m, "Reply-To: "); buf_puts(m, reply_s); buf_puts(m, "\n"); }
    buf_puts(m, "Subject: "); mail_subject(m, subj); buf_puts(m, "\n");
    buf_puts(m, "Date: "); buf_puts(m, date); buf_puts(m, "\n");
    buf_puts(m, "Message-ID: <"); buf_puts(m, id); buf_puts(m, "@"); buf_puts(m, domain); buf_puts(m, ">\n");
    buf_puts(m, "MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\nContent-Transfer-Encoding: 8bit\nX-Mailer: Common " CL_VERSION "\n\n");
    for (const char *p = text; *p; p++) if (*p != '\r') buf_putc(m, *p);    /* plain text only, one kind of line ending */
    if (m->len && m->data[m->len - 1] != '\n') buf_putc(m, '\n');
    free(subj); free(text); free(reply_s);

    const char *mode = getenv("CL_MAIL");
    int live = mode ? !strcmp(mode, "sendmail") : g_mail_live;
    if (live) {
#ifdef _WIN32
        buf_free(m); fail("this build cannot reach sendmail on Windows. Run with CL_MAIL=outbox to save emails as files instead");
#else
        const char *prog = getenv("CL_SENDMAIL") ? getenv("CL_SENDMAIL") : "/usr/sbin/sendmail";
        int fd[2]; if (pipe(fd) != 0) { buf_free(m); fail("could not start sendmail"); }
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) { dup2(fd[0], 0); close(fd[0]); close(fd[1]); execl(prog, "sendmail", "-t", "-i", (char *)NULL); _exit(127); }
        close(fd[0]);
        size_t off = 0; while (pid > 0 && off < m->len) { ssize_t w = write(fd[1], m->data + off, m->len - off); if (w <= 0) break; off += (size_t)w; }
        close(fd[1]);
        int status = 0; if (pid > 0) waitpid(pid, &status, 0);
        buf_free(m);
        if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fail("sendmail (%s) did not take the email. Ask your host whether sendmail is available, or set CL_SENDMAIL to its path", prog);
#endif
    } else {
        char dir[4096]; snprintf(dir, sizeof dir, "%s/outbox", base);
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0700);
#endif
        struct tm *tm = localtime(&now); char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
        char file[4200]; snprintf(file, sizeof file, "%s/%s-%.6s.eml", dir, stamp, id);
        FILE *f = fopen(file, "wb");
        if (!f) { buf_free(m); fail("could not save the email in %s", dir); }
        fwrite(m->data, 1, m->len, f); fclose(f); buf_free(m);
        fprintf(stderr, "cl: email to %s saved in outbox/%s-%.6s.eml (not sent: this is the development server)\n", to, stamp, id);
    }
    FILE *lf = fopen(log, "a"); if (lf) { fprintf(lf, "%lld %s\n", (long long)now, to); fclose(lf); }
}

static void exec_say(Env *e, Toks *T, Buf *out) {
    int raw = accept_word(T, "raw");
    int code = !raw && accept_word(T, "code");
    e->hint_prefix = raw ? "show raw" : code ? "show code" : "show"; e->hint_name = NULL; e->hint_skip = (raw || code) ? 2 : 1;
    Value v = parse_expr(e, T);
    e->hint_prefix = NULL;
    expect_line_end(T);
    char *s = raw ? v_to_text(&v) : code ? v_to_text(&v) : v_to_safe_text(&v);
    if (code) hl_code(s, s + strlen(s), out, 0);
    else buf_puts(out, s);
    buf_putc(out, '\n');
    free(s); v_free(&v);
}

static void expect_form(Toks *T, const char *w, const char *form) {
    if (!accept_word(T, w)) fail("expected '%s'. The whole line is written: %s", w, form);
}

static void exec_set(Env *e, Toks *T) {
    const char *name = expect_name(T);
    if (accept_word(T, "of")) {                                  /* set price of lamp to 15 */
        const char *recname = expect_name(T);
        expect_form(T, "to", "set COLUMN of RECORD to VALUE");
        Value v = parse_expr(e, T); expect_line_end(T);
        Value *r = env_get_own(e, recname);
        if (!r) fail("I do not know what '%s' is", recname);
        if (r->t != V_RECORD) fail("'%s' is %s, not a record, so it has no '%s' to set", recname, type_name(r->t), name);
        for (int i = 0; i < r->count; i++) if (strcmp(r->keys[i], name) == 0) { v_free(&r->items[i]); r->items[i] = v; return; }
        record_add(r, name, v);
        return;
    }
    expect_form(T, "to", "set NAME to VALUE");
    e->hint_prefix = "set"; e->hint_name = name; e->hint_skip = 3;
    Value v = parse_expr(e, T);
    e->hint_prefix = NULL; e->hint_name = NULL;
    if (at_op(T, ',')) {
        Value list = v_list(); list_add(&list, v);
        while (at_op(T, ',')) { T->pos++; list_add(&list, parse_expr(e, T)); }
        v = list;
    }
    expect_line_end(T);
    env_set(e, name, v);
}

static void exec_add(Env *e, Toks *T) {
    Value v = parse_expr(e, T);
    held_push(v);                                   /* freed if the rest of the line is wrong */
    expect_form(T, "to", "add VALUE to LIST");
    const char *name = expect_name(T);
    expect_line_end(T);
    Value *list = env_get_own(e, name);
    if (!list) fail("I do not know what '%s' is", name);
    if (list->t != V_LIST) fail("'%s' is %s, not a list", name, type_name(list->t));
    list_add(list, v);
    held_pop();                                     /* the list owns it now */
}

static NOINLINE void exec_include(Env *e, Toks *T, Buf *out) {
    Value v = parse_expr(e, T);
    held_push(v);
    expect_line_end(T);
    if (v.t != V_TEXT) fail("'include' needs a file name in quotes");
    if (v.untrusted || v.tainted) fail("'include' cannot take a name that came from a visitor, who could then read any file. Write the file name yourself: include \"header.html\"");
    if (strstr(v.s, "..") || strchr(v.s, '\\') || strchr(v.s, ':'))
        fail("'include' names a file in the site, like \"header.html\", \"parts/menu.html\", or \"/header.html\" for the site's top folder, never a path with ..");
    if (e->include_depth > 10) fail("files include each other in a loop");

    /* "header.html" is next to this page; "/header.html" is in the site's top
       folder, so a page in shop/ can use the same header as the home page. */
    char path[4096];
    if (v.s[0] == '/') {
        const char *rest = v.s; while (*rest == '/') rest++;
        if (!*rest) fail("'include \"/\"' needs a file name after the /, like \"/header.html\"");
        snprintf(path, sizeof path, "%s/%s", g_site_root[0] ? g_site_root : g_include_root[0] ? g_include_root : g_page_dir, rest);
    } else snprintf(path, sizeof path, "%s/%s", e->dir, v.s);
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot find the file \"%s\"", v.s);
    Buf *src = buf_new(); char chunk[4096]; size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_putn(src, chunk, n);
    fclose(f);

    char saved_dir[2048]; snprintf(saved_dir, sizeof saved_dir, "%s", e->dir);
    dir_of(path, e->dir, sizeof e->dir);
    int saved_line = cur_line;

    e->include_depth++;
    cl_process_env(src->data, e, out);
    e->include_depth--;

    snprintf(e->dir, sizeof e->dir, "%s", saved_dir);
    cur_line = saved_line;
    held_pop();
    buf_free(src); v_free(&v);
}

/* Run an if / otherwise if / otherwise / end chain. Returns index after 'end'. */
static int exec_if(Env *e, Lines *L, int i, Toks *T, Buf *out) {
    Value c = parse_expr(e, T); expect_line_end(T);
    int taken = v_truthy(&c); v_free(&c);
    int done = 0, j = i + 1;

    for (;;) {
        int k = find_clause(L, j, i);
        if (taken && !done) { exec_lines(e, L, j, k, out); done = 1; }

        if (line_kind(&L->l[k]) == K_END) return k + 1;

        /* 'otherwise' or 'otherwise if ...' */
        cur_line = L->l[k].lineno;
        Toks U; tokenize(L->l[k].text, L->l[k].len, &U);
        expect_word(&U, "otherwise");
        if (accept_word(&U, "if")) {
            if (done) { taken = 0; U.pos = U.n; }       /* a branch already ran: skip this condition */
            else { Value v = parse_expr(e, &U); taken = v_truthy(&v); v_free(&v); }
        } else {
            taken = !done;
        }
        expect_line_end(&U);
        toks_free(&U);
        j = k + 1;
    }
}

/* Practice code gets 20000 steps: a line run, or a turn of a loop. */
static void sandbox_step(Env *e) {
    if (e->sandbox && ++e->steps > 20000) fail("the practice code ran for too long (more than 20000 steps). Is a loop too big?");
}

static void exec_lines_inner(Env *e, Lines *L, int from, int to, Buf *out);
/* Blocks inside blocks (if, for each, repeat, verbs) each take a C stack
   frame, so their nesting has a limit too. */
static int g_block_depth;
#define MAX_BLOCK_DEPTH 500
static void exec_lines(Env *e, Lines *L, int from, int to, Buf *out) {
    stack_check();
    if (++g_block_depth > MAX_BLOCK_DEPTH) { g_block_depth = 0; fail("blocks are nested too deeply (more than %d inside each other)", MAX_BLOCK_DEPTH); }
    exec_lines_inner(e, L, from, to, out);
    g_block_depth--;
}
static void exec_lines_inner(Env *e, Lines *L, int from, int to, Buf *out) {
    int i = from;
    e->out = out;
    while (i < to && !e->stopped) {
        SrcLine *S = &L->l[i];
        cur_line = S->lineno;
        e->line_text = S->text; e->line_len = S->len; e->hint_prefix = NULL; e->hint_name = NULL;
        Toks T;
        if (S->ntoks < 0) {                          /* the first time this line runs */
            forgive_unquoted(e);                     /* show Hello world: say to add quotes */
            tokenize(S->text, S->len, &T);
            live_drop(&T);                           /* the line owns its tokens now, see lines_free */
            S->toks = T.t; S->ntoks = T.n;
        }
        T.t = S->toks; T.n = S->ntoks; T.cap = 0; T.pos = 0;
        if (T.n == 0) { i++; continue; }

        if (T.t[0].t != T_WORD) {
            if (T.t[0].t == T_STR) fail("a line cannot start with text. Did you mean 'show \"%s\"'?", T.t[0].text);
            fail("a line must start with a command like show, set, if, for each, or repeat");
        }
        const char *cmd = T.t[0].text; T.pos = 1;

        if (e->sandbox) {
            sandbox_step(e);
            if (out->len > 50000) fail("the practice code made too much output (more than 50000 letters)");
            if (!strcmp(cmd, "include") || !strcmp(cmd, "open") || !strcmp(cmd, "tell") || !strcmp(cmd, "remember") ||
                !strcmp(cmd, "forget") || !strcmp(cmd, "go") || !strcmp(cmd, "send"))
                fail("'%s' is not allowed in practice code. Everything else is", cmd);
        }
        if (line_kind(S) == K_EMAIL) {
            int end = find_text_end(L, i);
            expect_form(&T, "email", "send email to \"you@site.com\"");
            expect_form(&T, "to", "send email to \"you@site.com\"");
            Value to = parse_expr(e, &T); held_push(to); expect_line_end(&T);
            e->plain_holes = 1;                              /* an email is plain text: no HTML escaping */
            Value r = record_block(e, L, i, end, 1); held_push(r);
            e->plain_holes = 0;
            cur_line = S->lineno;
            send_email(e, &to, &r);
            held_pop(); held_pop(); v_free(&to); v_free(&r);
            i = end + 1; continue;
        }
        if (line_kind(S) == K_RECORD) {
            int end = find_text_end(L, i);
            Value r = record_block(e, L, i, end, 0);
            env_set(e, T.t[1].text, r);
            i = end + 1; continue;
        }
        if (line_kind(S) == K_TEXT) {
            int end = find_text_end(L, i);
            int tainted; char *text = text_block(e, L, i, end, &tainted);
            if (!strcmp(cmd, "show")) { buf_puts(out, text); buf_putc(out, '\n'); }
            else { Value v = v_text(text); v.tainted = tainted; env_set(e, T.t[1].text, v); }   /* set NAME to ... end */
            free(text); i = end + 1; continue;
        }
        if      (!strcmp(cmd, "show"))     { exec_say(e, &T, out); i++; }
        else if (!strcmp(cmd, "set"))     { exec_set(e, &T);      i++; }
        else if (!strcmp(cmd, "add"))     { exec_add(e, &T);      i++; }
        else if (!strcmp(cmd, "include")) { exec_include(e, &T, out); i++; }
        else if (!strcmp(cmd, "tell")) {
            expect_form(&T, "database", "tell database \"SQL\" with VALUE, ...");
            Value sql = parse_primary(e, &T); held_push(sql);
            if (sql.t != V_TEXT) fail("'tell database' needs the SQL in quotes");
            if (sql.untrusted || sql.tainted) fail("the SQL has a visitor's value inside it, and a visitor could change the query. Put the value after 'with' and write ? in its place: tell database \"insert into notes (body) values (?)\" with note");
            Value params = parse_with(e, &T); held_push(params); expect_line_end(&T);
            db_tell(e, sql.s, &params);
            held_pop(); held_pop();
            v_free(&sql); v_free(&params); i++;
        }
        else if (!strcmp(cmd, "remember")) {
            Value key = parse_primary(e, &T);
            if (key.t != V_TEXT || !key.s[0]) fail("'remember' needs a name in quotes: remember \"user\" as name");
            expect_form(&T, "as", "remember \"name\" as VALUE");
            Value v = parse_expr(e, &T); held_push(v); expect_line_end(&T); held_pop();
            char *t = plain_text(&v);
            session_load(e); session_set(e, key.s, t); e->root->session_changed = 1;   /* the page's session, also from a verb */
            free(t); v_free(&v); v_free(&key); i++;
        }
        else if (!strcmp(cmd, "forget")) {
            if (accept_word(&T, "everything")) {
                expect_line_end(&T);
                session_load(e);
                Env *r = e->root;                                    /* the page's session, also from a verb */
                for (int k = 0; k < r->sn; k++) { free(r->skeys[k]); free(r->svals[k]); }
                r->sn = 0; r->session_changed = 1; i++;
            } else {
                Value key = parse_primary(e, &T); expect_line_end(&T);
                if (key.t != V_TEXT) fail("'forget' needs a name in quotes, or 'forget everything'");
                session_forget(e, key.s); v_free(&key); i++;
            }
        }
        else if (!strcmp(cmd, "go")) {
            expect_form(&T, "to", "go to \"/page\"");
            Value where = parse_expr(e, &T); held_push(where); expect_line_end(&T); held_pop();
            if (where.t != V_TEXT || !where.s[0]) fail("'go to' needs a page address in quotes, like \"/thanks.html\"");
            if (where.tainted) { char *t = plain_text(&where); free(where.s); where.s = t; }
            if ((where.untrusted || where.tainted) && (strchr(where.s, ':') || strchr(where.s, '\\') || !strncmp(where.s, "//", 2) || where.s[0] != '/'))
                fail("'go to' with a visitor's value may only point to a page on this site, starting with /, like \"/blog/[slug]\". Otherwise a visitor could send people anywhere");
            snprintf(e->root->redirect, sizeof e->root->redirect, "%s", where.s);
            for (Env *x = e; x; x = x->parent) x->stopped = 1;
            v_free(&where); i++;
        }
        else if (!strcmp(cmd, "open")) {
            expect_form(&T, "database", "open database \"file.db\"");
            Value f = parse_expr(e, &T); held_push(f); expect_line_end(&T); held_pop();
            if (f.t != V_TEXT) fail("'open database' needs a file name in quotes");
            if (f.untrusted || f.tainted) fail("'open database' cannot take a name that came from a visitor. Write it yourself: open database \"site.db\"");
            db_open(e, f.s); v_free(&f); i++;
        }
        else if (!strcmp(cmd, "if"))      { i = exec_if(e, L, i, &T, out); }
        else if (!strcmp(cmd, "for")) {
            expect_form(&T, "each", "for each NAME in LIST");
            const char *name = expect_name(&T);
            expect_form(&T, "in", "for each NAME in LIST");
            int from = T.pos;
            Value list = parse_expr(e, &T); held_push(list);
            if (at_op(&T, ',')) fail("'for each' walks a list kept in a name. First: set things to ..., ..., then: for each %s in things", name);
            expect_line_end(&T);
            if (list.t != V_LIST) {
                const char *what = (T.pos == from + 1 && T.t[from].t == T_WORD) ? T.t[from].text : "that";
                if (list.t == V_RECORD)
                    fail("'for each' walks a list, but %s is one record. Read its columns directly, like: show \"[%s of %s]\". (If it came from 'first of', that already picked one record from the list.)",
                         what, list.count ? list.keys[0] : "COLUMN", what);
                if (list.t == V_TEXT)
                    fail("'for each' walks a list, but %s is text. To walk its words, write: for each word in split %s by \" \"", what, what);
                if (list.t == V_NUM)
                    fail("'for each' walks a list, but %s is a number. To do something that many times, write: repeat %s times", what, what);
                fail("'for each' needs a list, but %s is %s", what, type_name(list.t));
            }
            int end = find_block_end(L, i);
            for (int k = 0; k < list.count && !e->stopped; k++) {
                sandbox_step(e);
                env_set(e, name, v_copy(&list.items[k]));
                exec_lines(e, L, i + 1, end, out);
            }
            held_pop(); v_free(&list);
            i = end + 1;
        }
        else if (!strcmp(cmd, "repeat")) {
            Value n = parse_expr(e, &T);
            expect_form(&T, "times", "repeat NUMBER times"); 
            int cond_at = -1;
            if (accept_word(&T, "until")) cond_at = T.pos; else expect_line_end(&T);
            if (n.t != V_NUM) fail("'repeat' needs a number, not %s", type_name(n.t));
            int end = find_block_end(L, i);
            double times = n.n > 2e9 ? 2e9 : n.n;
            for (int k = 0; k < (int)times && !e->stopped; k++) {
                sandbox_step(e);
                if (cond_at >= 0) {                                  /* repeat N times until CONDITION: stop early */
                    T.pos = cond_at;
                    Value c = parse_expr(e, &T); held_push(c); expect_line_end(&T); held_pop();
                    int done = v_truthy(&c); v_free(&c);
                    if (done) break;
                }
                exec_lines(e, L, i + 1, end, out);
            }
            i = end + 1;
        }
        else if (!strcmp(cmd, "to")) {
            const char *name = expect_name(&T);
            Value names = v_list(); held_push(names);               /* held: a bad argument name is an error */
            while (!at_end(&T)) {
                if (names.count == 8) fail("a verb can take up to 8 values");
                list_add(&names, v_text(expect_name(&T))); held_pop(); held_push(names);
            }
            int end = find_block_end(L, i);
            held_pop();
            char **args = NULL; int argc = names.count;
            if (argc) { args = malloc(sizeof(char *) * (size_t)argc); for (int a = 0; a < argc; a++) args[a] = xstrdup(names.items[a].s); }
            v_free(&names);
            Buf *body = buf_new();
            for (int j = i + 1; j < end; j++) { buf_putn(body, L->l[j].raw, L->l[j].rawlen); buf_putc(body, '\n'); }
            char *text = xstrdup(body->data); buf_free(body);
            verb_define(e, name, args, argc, text, end > i + 1 ? L->l[i + 1].lineno : L->l[i].lineno);
            i = end + 1;
        }
        else if (!strcmp(cmd, "give")) {
            if (!e->parent) fail("'give' only works inside a verb (a block that starts with 'to')");
            Value v = parse_expr(e, &T); held_push(v); expect_line_end(&T); held_pop();
            if (e->has_gave) v_free(&e->gave);
            e->gave = v; e->has_gave = 1; e->stopped = 1; i++;
        }
        else if (verb_find(e, cmd)) {
            Verb *vb = verb_find(e, cmd);
            Value args = v_list(); held_push(args);
            for (int a = 0; a < vb->argc; a++) {
                if (at_end(&T)) fail("'%s' takes %d value%s, but got %d. It is written: %s", vb->name, vb->argc, vb->argc == 1 ? "" : "s", a, verb_form(vb));
                list_add(&args, parse_expr(e, &T)); held_pop(); held_push(args);
            }
            if (!at_end(&T)) fail("'%s' takes %d value%s, but got more. It is written: %s", vb->name, vb->argc, vb->argc == 1 ? "" : "s", verb_form(vb));
            Value r = verb_call(e, vb, &args, out, 0);
            held_pop(); v_free(&r); v_free(&args); i++;
        }
        else if (!strcmp(cmd, "otherwise")) fail("'otherwise' without an 'if' before it");
        else if (!strcmp(cmd, "end"))       fail("'end' with no block to close");
        else {
            char lc[64]; lowercase(cmd, lc, sizeof lc);
            const char *syn = synonym(lc), *closest = nearest_word(cmd, 1);
            if (dict_find(lc) && dict_find(lc)->cmd) fail("Common words are always lowercase. Write '%s'", lc);
            if (syn && !strcmp(syn, "show")) {
                char rest[512]; rest_of_line_x(e, 1, rest, sizeof rest, 0);
                fail("'%s' is not a Common word. Use 'show': show %s", cmd, rest);
            }
            if (syn) fail("'%s' is not a Common word. Use '%s'", cmd, syn);
            if (dict_find(cmd)) fail("'%s' cannot start a line. It is written: %s", cmd, dict_find(cmd)->form ? dict_find(cmd)->form : "inside another command");
            if (env_get(e, cmd)) fail("a line cannot start with '%s'. Did you mean 'show %s' or 'set %s to ...'?", cmd, cmd, cmd);
            if (closest) fail("I do not understand '%s'. Did you mean '%s'?", cmd, closest);
            fail("I do not understand '%s'. Run 'cl words' to see every Common word", cmd);
        }

    }
}

/* ================================================================== */
/* Pages: replace every <cl> block with its output                     */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Showing code: <cl-code> blocks are highlighted, not run              */
/* ------------------------------------------------------------------ */

static void buf_putc_html(Buf *b, char c) { char t[2] = { c, 0 }; buf_puts_text(b, t); }   /* code samples sit between tags */

static void hl_span(Buf *out, const char *cls, const char *p, size_t n) {
    buf_puts(out, "<span class=\""); buf_puts(out, cls); buf_puts(out, "\">");
    for (size_t i = 0; i < n; i++) buf_putc_html(out, p[i]);
    buf_puts(out, "</span>");
}

/* Highlight Common source between p and end. With stop_at_bracket, returns at
   the ']' that closes a hole inside text; otherwise returns end. */
static const char *hl_code(const char *p, const char *end, Buf *out, int stop_at_bracket) {
    while (p < end) {
        char c = *p;
        if (stop_at_bracket && c == ']') return p;
        if (c == '<' && (end - p >= 4 && strncmp(p, "<cl>", 4) == 0)) { hl_span(out, "cl-tag", p, 4); p += 4; continue; }
        if (c == '<' && (end - p >= 5 && strncmp(p, "</cl>", 5) == 0)) { hl_span(out, "cl-tag", p, 5); p += 5; continue; }
        if (c == '#') {
            const char *nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) nl = end;
            hl_span(out, "cl-note", p, (size_t)(nl - p)); p = nl; continue;
        }
        if (c == '"') {
            buf_puts(out, "<span class=\"cl-text\">&quot;"); p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) { buf_putc_html(out, p[0]); buf_putc_html(out, p[1]); p += 2; continue; }
                if (*p == '[') {
                    buf_puts(out, "</span><span class=\"cl-hole\">["); p++;
                    p = hl_code(p, end, out, 1);
                    if (p < end && *p == ']') { buf_puts(out, "]"); p++; }
                    buf_puts(out, "</span><span class=\"cl-text\">");
                    continue;
                }
                buf_putc_html(out, *p++);
            }
            if (p < end) { buf_putc_html(out, '"'); p++; }
            buf_puts(out, "</span>"); continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            const char *q = p;
            while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
            char w[64]; size_t n = (size_t)(q - p);
            if (n < sizeof w) { memcpy(w, p, n); w[n] = 0; }
            else w[0] = 0;
            if (w[0] && dict_find(w)) hl_span(out, "cl-word", p, n);
            else for (size_t i = 0; i < n; i++) buf_putc_html(out, p[i]);
            p = q; continue;
        }
        buf_putc_html(out, c); p++;
    }
    return p;
}

/* Run one block's lines into `o`. Returns 1 on success, 0 if a Common error
   was raised (the message is in err_msg). Kept separate so that no local of
   the caller is live across setjmp. */
static int run_block(Env *env, Lines *L, Buf *o) {
    jmp_buf saved; memcpy(saved, err_jmp, sizeof(jmp_buf));
    int ok; int mark = g_nlive, hmark = g_nheld, bmark = g_nbufs, fmark = g_nframes, dmark = g_expr_depth, kmark = g_block_depth;
    if (setjmp(err_jmp) == 0) { exec_lines(env, L, 0, L->n, o); ok = 1; }
    else {
        ok = 0;   /* free what the error left behind: token lists, held values, text buffers */
        g_expr_depth = dmark;                                      /* the expressions the error left were not finished */
        g_block_depth = kmark;
        while (g_nlive > mark) { Toks dead = g_live[g_nlive - 1]; toks_free(&dead); }
        while (g_nheld > hmark) { Value dead = g_held[--g_nheld]; v_free(&dead); }
        while (g_nbufs > bmark) buf_free(g_bufs[--g_nbufs]);
        while (g_nframes > fmark) {                                  /* verbs the error was inside of */
            g_nframes--;
            Env *dead = g_frames[g_nframes].env; Lines *dl = (Lines *)g_frames[g_nframes].lines;
            if (dead->has_gave) v_free(&dead->gave);
            env_free(dead); free(dead); if (dl) lines_free(dl);
        }
    }
    memcpy(err_jmp, saved, sizeof(jmp_buf));
    return ok;
}

/* Run practice code in a fresh, restricted environment. Gives its output as text. */
static Value run_sandbox(Env *parent, const char *code) {
    if (parent->sandbox) fail("'try' cannot be used inside practice code");
    Env e; env_init(&e, parent->dir, parent->query, NULL, parent->path, NULL);   /* lessons may read the address */
    e.sandbox = 1;
    Lines *L = malloc(sizeof *L); split_lines(code, 1, L);
    Buf *o = buf_new();
    int saved_line = cur_line;
    g_budget_on = 1; g_budget_left = 64u * 1024 * 1024;
    int ok = run_block(&e, L, o);
    g_budget_on = 0;
    if (!ok) {
        buf_puts(o, "<pre class=\"cl-error\" style=\"color:#c62828;white-space:pre-wrap\">Common error: ");
        buf_puts_text(o, err_msg);
        buf_puts(o, "</pre>");
    }
    cur_line = saved_line;
    Value r = v_text(o->data); r.untrusted = 1;
    buf_free(o); lines_free(L); env_free(&e);
    return r;
}

static void cl_process_env(const char *html, Env *env, Buf *out) {
    const char *p = html;
    int line = 1;

    for (;;) {
        const char *open = strstr(p, "<cl>");
        const char *show = strstr(p, "<cl-code>");
        if (show && (!open || show < open)) {
            /* a code sample: highlight it, do not run it */
            buf_putn(out, p, (size_t)(show - p));
            for (const char *c = p; c < show; c++) if (*c == '\n') line++;
            const char *src = show + 9, *close = strstr(src, "</cl-code>");
            if (!close) { buf_puts(out, "<pre class=\"cl-error\">Common error: &lt;cl-code&gt; block is never closed</pre>"); return; }
            if (*src == '\n') src++;
            hl_code(src, close, out, 0);
            for (const char *c = show; c < close + 10; c++) if (*c == '\n') line++;
            p = close + 10;
            continue;
        }
        if (!open) { buf_puts(out, p); return; }

        buf_putn(out, p, (size_t)(open - p));
        for (const char *c = p; c < open; c++) if (*c == '\n') line++;

        const char *code  = open + 4;
        const char *close = strstr(code, "</cl>");
        if (!close) {
            buf_puts(out, "<pre class=\"cl-error\" style=\"color:#c62828;white-space:pre-wrap\">Common error: &lt;cl&gt; block is never closed</pre>");
            return;
        }

        char *src = xstrndup(code, (size_t)(close - code));
        Lines *L = malloc(sizeof *L); split_lines(src, line, L);
        Buf *o = buf_new();

        if (run_block(env, L, o)) {
            /* drop the final newline so "Hello, <cl>show name</cl>!" reads naturally */
            if (o->len && o->data[o->len - 1] == '\n') o->len--;
            buf_putn(out, o->data, o->len);
        } else {
            buf_putn(out, o->data, o->len);          /* keep what was printed before the error */
            buf_puts(out, "<pre class=\"cl-error\" style=\"color:#c62828;white-space:pre-wrap\">Common error: ");
            buf_puts_text(out, err_msg);
            buf_puts(out, "</pre>");
        }

        buf_free(o); lines_free(L); free(src);
        if (env->stopped) return;                     /* 'go to' ran: the rest of the page is not sent */
        for (const char *c = open; c < close + 5; c++) if (*c == '\n') line++;
        p = close + 5;
    }
}

/* ================================================================== */
/* Files                                                               */
/* ================================================================== */

static char *read_file(const char *path, size_t *n) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *d = malloc((size_t)st.st_size + 1);
    size_t r = fread(d, 1, (size_t)st.st_size, f);
    fclose(f);
    d[r] = 0;
    if (n) *n = r;
    return d;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* ================================================================== */
/* Serving pages: shared by the dev server, CGI mode and the CLI       */
/* ================================================================== */

typedef struct {
    const char *status;      /* "200 OK" */
    const char *type;        /* content type */
    char  *body; size_t len; /* caller frees body */
    char   location[1024];   /* set for a 303 redirect */
    char  *set_cookie;       /* Set-Cookie value, or NULL */
    int    cacheable;        /* static asset: let the browser cache it */
} Response;

static const char *mime_type(const char *path) {
    if (ends_with(path, ".html")) return "text/html; charset=utf-8";
    if (ends_with(path, ".css"))  return "text/css";
    if (ends_with(path, ".png"))  return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif"))  return "image/gif";
    if (ends_with(path, ".svg"))  return "image/svg+xml";
    if (ends_with(path, ".ico"))  return "image/x-icon";
    if (ends_with(path, ".webp")) return "image/webp";
    if (ends_with(path, ".woff2")) return "font/woff2";
    if (ends_with(path, ".pdf"))  return "application/pdf";
    if (ends_with(path, ".zip"))  return "application/zip";
    if (ends_with(path, ".txt") || ends_with(path, ".md")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* Run a page and fill in the response. */
static void run_page(const char *file, const char *html, const char *query, const char *body,
                     const char *url_path, const char *cookie, const char *status, Response *r) {
    Env env; env_init(&env, file, query, body, url_path, cookie);
    snprintf(g_page_dir, sizeof g_page_dir, "%s", env.dir);
    Buf *out = buf_new();
    cl_process_env(html, &env, out);
    char *sc = session_cookie(&env);
    r->set_cookie = sc[0] ? sc : NULL; if (!sc[0]) free(sc);
    if (env.redirect[0]) {
        r->status = "303 See Other";
        /* percent-encode anything a Location header cannot carry */
        size_t j = 0;
        for (const char *c = env.redirect; *c && j + 4 < sizeof r->location; c++) {
            unsigned char u = (unsigned char)*c;
            if (u <= ' ' || u >= 127 || u == '"' || u == '<' || u == '>' || u == '\\') j += (size_t)snprintf(r->location + j, 4, "%%%02X", u);
            else r->location[j++] = *c;
        }
        r->location[j] = 0;
        r->type = "text/plain; charset=utf-8";
        r->body = xstrdup(""); r->len = 0;
        buf_free(out);
    } else {
        r->status = status;
        r->type = "text/html; charset=utf-8";
        r->body = out->data; r->len = out->len;
        free(out);
    }
    env_free(&env);
    lines_free(g_retired); g_retired = NULL;      /* verbs that were defined again */
}

/* Find the file for a URL path. Clean URLs: /about -> about.html and
   /blog/my-post -> blog.html (the page reads the rest from `path`). */
static int resolve_page(const char *dir, const char *url, char *out, size_t n) {
    struct stat st;
    snprintf(out, n, "%s%s", dir, url);
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        strncat(out, ends_with(out, "/") ? "index.html" : "/index.html", n - strlen(out) - 1);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;

    char rest[4096]; snprintf(rest, sizeof rest, "%s", url);
    for (;;) {
        snprintf(out, n, "%s%s.html", dir, rest);
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 1;
        char *slash = strrchr(rest, '/');
        if (!slash || slash == rest) return 0;
        *slash = 0;
    }
}

static void plain(Response *r, const char *status, const char *text) {
    r->status = status; r->type = "text/plain; charset=utf-8";
    r->body = xstrdup(text); r->len = strlen(text);
}

/* A database, its seed file or the session secret is never sent, even if
   someone guesses the name. macOS and Windows find files whatever the case,
   and Windows ignores dots and spaces at the end of a name, so cl.SECRET and
   site.db. are the same files: compare the way the file system does. */
static int is_private(const char *path) {
    static const char *ends[] = { ".db", ".sqlite", ".sqlite3", ".sql", ".secret", ".eml", ".log", "/outbox", NULL };
    static const char *inside[] = { ".db-journal", ".db-wal", ".db-shm", NULL };
    char lc[8192]; lowercase(path, lc, sizeof lc);
    size_t n = strlen(lc); while (n && (lc[n-1] == '.' || lc[n-1] == ' ' || lc[n-1] == '/' || lc[n-1] == '\\')) lc[--n] = 0;
    for (int i = 0; ends[i]; i++) if (ends_with(lc, ends[i])) return 1;
    for (int i = 0; inside[i]; i++) if (strstr(lc, inside[i])) return 1;
    if (strstr(lc, "/outbox/") || strstr(lc, "\\outbox\\") || !strncmp(lc, "outbox/", 7)) return 1;   /* emails saved while developing */
    return 0;
}

/* Build the response for one request. */
static void handle(const char *dir, const char *method, const char *url, const char *query, const char *body, const char *cookie, Response *r) {
    memset(r, 0, sizeof *r);
    if (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "HEAD")) { plain(r, "405 Method Not Allowed", "Only GET and POST\n"); return; }
    if (strstr(url, "..") || is_private(url)) { plain(r, "403 Forbidden", "Forbidden\n"); return; }
#ifdef _WIN32
    if (strchr(url, ':')) { plain(r, "403 Forbidden", "Forbidden\n"); return; }   /* cl.secret::$DATA reads the same file */
#endif

    char path[8192];
    int found = resolve_page(dir, url, path, sizeof path);
    if (found && is_private(path)) { plain(r, "403 Forbidden", "Forbidden\n"); return; }
    if (!found) {
        snprintf(path, sizeof path, "%s/404.html", dir);          /* a custom 404 page, if the site has one */
        size_t len; char *html = read_file(path, &len);
        if (!html) { plain(r, "404 Not Found", "Not found\n"); return; }
        run_page(path, html, query, body, url, cookie, "404 Not Found", r);
        free(html);
        return;
    }
    size_t len; char *data = read_file(path, &len);
    if (!data) { plain(r, "404 Not Found", "Not found\n"); return; }
    if (ends_with(path, ".html")) {
        run_page(path, data, query, body, url, cookie, "200 OK", r);
        free(data);
    } else {
        r->status = "200 OK"; r->type = mime_type(path);
        r->body = data; r->len = len; r->cacheable = 1;
    }
}

/* ================================================================== */
/* Built-in web server (like `php -S`)                                 */
/* ================================================================== */

static int ci_starts(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

/* A copy of a header's value, or NULL. Caller frees. */
static char *header_text(const char *req, const char *name) {
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        if (ci_starts(p + 2, name) && p[2 + strlen(name)] == ':') {
            const char *v = p + 3 + strlen(name); while (*v == ' ') v++;
            const char *end = strstr(v, "\r\n"); if (!end) end = v + strlen(v);
            return xstrndup(v, (size_t)(end - v));
        }
    }
    return NULL;
}

/* Value of a header, or -1. */
static long header_number(const char *req, const char *name) {
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        if (ci_starts(p + 2, name) && p[2 + strlen(name)] == ':') return atol(p + 3 + strlen(name));
    }
    return -1;
}

/* Read one request. On success *req holds the whole thing (caller frees) and
   method, url and body point into it. Body is NULL when there is none. */
/* A form sent to this site from a page on another site. Browsers name the page
   that sent a POST in its Origin header; when it names a different host, the
   request is refused, so another site cannot make a visitor's browser submit
   a form here (sign in, post, send an email). No Origin: an older client, let it
   through. A proxy may change Host, so X-Forwarded-Host counts too. */
static int cross_site(const char *origin, const char *host, const char *fwd_host) {
    if (!origin || !*origin) return 0;
    const char *o = strstr(origin, "://");
    if (!o) return 1;                                   /* "null": a sandboxed frame or a file */
    o += 3;
    size_t on = strcspn(o, "/ \t");
    const char *hosts[2] = { host, fwd_host };
    for (int i = 0; i < 2; i++) {
        const char *h = hosts[i]; if (!h) continue;
        while (*h == ' ') h++;
        size_t hn = strcspn(h, ", \t\r\n");            /* X-Forwarded-Host may list several: the first is the visitor's */
        if (hn == on) {
            size_t k = 0; while (k < on && tolower((unsigned char)o[k]) == tolower((unsigned char)h[k])) k++;
            if (k == on) return 0;
        }
    }
    return 1;
}

static int read_request(sock_t c, char **req, char **method, char **url, char **body, char **cookie, int *cross) {
    Buf *b = buf_new(); char chunk[8192];
    size_t header_end = 0; long content_length = -1;
    for (;;) {
        int n = (int)sock_read(c, chunk, sizeof chunk);
        if (n <= 0) break;
        buf_putn(b, chunk, (size_t)n);
        if (!header_end) {
            char *he = strstr(b->data, "\r\n\r\n");
            if (he) {
                header_end = (size_t)(he - b->data) + 4;
                content_length = header_number(b->data, "Content-Length");
                if (content_length > 1024 * 1024) { buf_free(b); return 0; }   /* 1 MB is plenty for a form */
            } else if (b->len > 65536) { buf_free(b); return 0; }
        }
        if (header_end && (content_length <= 0 || b->len >= header_end + (size_t)content_length)) break;
    }
    if (!header_end) { buf_free(b); return 0; }
    if (content_length > 0 && b->len < header_end + (size_t)content_length) { buf_free(b); return 0; }   /* the body was cut short */

    *cookie = header_text(b->data, "Cookie");          /* before the request line is cut */
    char *origin = header_text(b->data, "Origin"), *host = header_text(b->data, "Host"), *fwd = header_text(b->data, "X-Forwarded-Host");
    *cross = cross_site(origin, host, fwd);
    free(origin); free(host); free(fwd);
    char *line_end = strstr(b->data, "\r\n"); *line_end = 0;
    *method = b->data;
    char *sp = strchr(*method, ' ');
    if (!sp) { buf_free(b); return 0; }
    *sp = 0; *url = sp + 1;
    sp = strchr(*url, ' '); if (sp) *sp = 0;
    if (content_length > 0) { *body = b->data + header_end; (*body)[content_length] = 0; }
    else *body = NULL;
    *req = b->data; free(b);
    return 1;
}

/* Sent with every response: never guess a file is HTML or a script, and never
   show this site's pages inside a frame on another site (clickjacking). */
#define SAFETY_HEADERS "X-Content-Type-Options: nosniff\r\nX-Frame-Options: SAMEORIGIN\r\n"

static void send_response(sock_t fd, const Response *r, int head_only) {
    char hdr[8192];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n%s%s%s%s%s%s%s" SAFETY_HEADERS "Connection: close\r\n\r\n",
        r->status, r->type, (unsigned long)r->len,
        r->location[0] ? "Location: " : "", r->location[0] ? r->location : "", r->location[0] ? "\r\n" : "",
        r->set_cookie ? "Set-Cookie: " : "", r->set_cookie ? r->set_cookie : "", r->set_cookie ? "\r\n" : "",
        r->cacheable ? "Cache-Control: max-age=3600\r\n" : "");
    if (h < 0 || (size_t)h >= sizeof hdr) {                  /* a huge cookie or address: never send past the buffer */
        static const char too_big[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 28\r\nConnection: close\r\n\r\nThe response was too large\n";
        sock_write(fd, too_big, sizeof too_big - 1); return;
    }
    if (sock_write(fd, hdr, (size_t)h) < 0) return;
    if (!head_only && r->len) sock_write(fd, r->body, r->len);
}

static void serve_one(sock_t c, const char *dir) {
    char *req, *method, *url, *body, *cookie = NULL; int cross = 0;
    struct sockaddr_in peer; memset(&peer, 0, sizeof peer);       /* the visitor's address, for the password limit */
#ifdef _WIN32
    int plen = (int)sizeof peer;
#else
    socklen_t plen = sizeof peer;
#endif
    if (getpeername(c, (struct sockaddr *)&peer, &plen) == 0) snprintf(g_remote, sizeof g_remote, "%s", inet_ntoa(peer.sin_addr));
    else snprintf(g_remote, sizeof g_remote, "unknown");
    if (!read_request(c, &req, &method, &url, &body, &cookie, &cross)) {
        Response bad; memset(&bad, 0, sizeof bad); plain(&bad, "400 Bad Request", "Bad request\n");
        send_response(c, &bad, 0); free(bad.body);
        return;
    }
    char *query = strchr(url, '?');
    if (query) *query++ = 0;
    char *upath = url_decode(url, strlen(url));

    Response r;
    if (cross && !strcmp(method, "POST")) { memset(&r, 0, sizeof r); plain(&r, "403 Forbidden", "Forbidden: this form was sent from another site\n"); }
    else handle(dir, method, upath, query, body, cookie, &r);
    send_response(c, &r, strcmp(method, "HEAD") == 0);
    printf("%s %s%s%s -> %s%s%s\n", method, url, query ? "?" : "", query ? query : "", r.status,
           r.location[0] ? " -> " : "", r.location[0] ? r.location : "");
    fflush(stdout);
    free(r.body); free(r.set_cookie); free(upath); free(req); free(cookie);
}

static void serve(const char *host, int port, const char *dir) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "cl: cannot start networking\n"); exit(1); }
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);          /* finished children are cleaned up automatically */
#endif

    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_valid(s)) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, sock_opt_ptr(&one), sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = host ? inet_addr(host) : htonl(INADDR_LOOPBACK);
    if (addr.sin_addr.s_addr == INADDR_NONE) { fprintf(stderr, "cl: '%s' is not an address like 0.0.0.0 or 127.0.0.1\n", host); exit(1); }
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); exit(1); }
    if (listen(s, 64) < 0) { perror("listen"); exit(1); }

    snprintf(g_site_root, sizeof g_site_root, "%s", dir);
    printf("Common server running at http://%s:%d/  (folder: %s)\n", host ? host : "localhost", port, dir);
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);

    for (;;) {
        sock_t c = accept(s, NULL, NULL);
        if (!sock_valid(c)) continue;
#ifndef _WIN32
        /* One process per request, like CGI: a slow or crashing page cannot
           take the server down, and nothing leaks across requests. */
        pid_t pid = fork();
        if (pid > 0) { sock_close(c); continue; }
        if (pid == 0) { sock_close(s); signal(SIGALRM, SIG_DFL); alarm(30); }   /* a slow client or page gets 30 s */
        serve_one(c, dir);
        sock_close(c);
        if (pid == 0) _exit(0);
#else
        serve_one(c, dir);
        sock_close(c);
#endif
    }
}

/* ================================================================== */
/* CGI mode: `cl cgi` behind Apache, nginx or any shared host          */
/* ================================================================== */

/* Under cl cgi: keep cl.secret, mail.log and outbox/ out of the web root, in
   <the folder above DOCUMENT_ROOT>/.common/<the web root's name>/, for example
   /home/you/.common/public_html/ for /home/you/public_html. Each site on the
   account gets its own folder. CL_PRIVATE_DIR chooses another folder. If the
   folder cannot be made, they stay next to the pages, protected by .htaccess. */
static void private_dir_for_cgi(void) {
    const char *chosen = getenv("CL_PRIVATE_DIR");
    if (chosen && *chosen) { snprintf(g_private_dir, sizeof g_private_dir, "%s", chosen); return; }
#ifndef _WIN32
    const char *dr = getenv("DOCUMENT_ROOT");
    if (!dr || !*dr) return;
    char root[2048]; snprintf(root, sizeof root, "%s", dr);
    size_t n = strlen(root); while (n > 1 && root[n - 1] == '/') root[--n] = 0;
    char *slash = strrchr(root, '/');
    if (!slash || slash == root || !slash[1]) return;
    *slash = 0;                                                   /* root is now the folder above, slash+1 the web root's name */
    char top[2100], mine[4200];
    snprintf(top, sizeof top, "%s/.common", root);
    snprintf(mine, sizeof mine, "%s/%s", top, slash + 1);
    mkdir(top, 0700); mkdir(mine, 0700);
    struct stat st;
    if (stat(mine, &st) == 0 && S_ISDIR(st.st_mode) && access(mine, W_OK) == 0)
        snprintf(g_private_dir, sizeof g_private_dir, "%s", mine);
#endif
}

/* Does the name end in .html, whatever the case? */
static int ends_with_html(const char *f) {
    size_t n = strlen(f);
    if (n < 5) return 0;
    char t[6]; for (int k = 0; k < 5; k++) t[k] = (char)tolower((unsigned char)f[n - 5 + k]);
    t[5] = 0;
    return !strcmp(t, ".html");
}

static int run_cgi(void) {
    const char *https = getenv("HTTPS"), *proto = getenv("HTTP_X_FORWARDED_PROTO");
    g_https = (https && (!strcmp(https, "on") || !strcmp(https, "1"))) || (proto && !strcmp(proto, "https"));
    g_mail_live = 1;                   /* behind a real web server, emails really go out */
    private_dir_for_cgi();
    const char *docroot = getenv("DOCUMENT_ROOT");
    if (docroot && *docroot) snprintf(g_site_root, sizeof g_site_root, "%s", docroot);   /* include "/header.html" starts here */
    const char *ra = getenv("REMOTE_ADDR");
    snprintf(g_remote, sizeof g_remote, "%s", ra && *ra ? ra : "unknown");
    for (char *c = g_remote; *c; c++) if (isspace((unsigned char)*c)) *c = '_';

    /* Which page to run. Web servers say it in different ways:
       - nginx with fcgiwrap, and most direct setups: SCRIPT_FILENAME is the page.
       - Apache's Action, and a rewrite to /cgi-bin/cl/page.html (Apache and
         LiteSpeed): SCRIPT_FILENAME is cl itself, and the page is
         PATH_TRANSLATED, or DOCUMENT_ROOT followed by PATH_INFO. */
    const char *sf = getenv("SCRIPT_FILENAME"), *pt = getenv("PATH_TRANSLATED");
    const char *pi = getenv("PATH_INFO"), *dr = getenv("DOCUMENT_ROOT");
    char built[4096];
    const char *file = NULL;
    if (sf && *sf && ends_with_html(sf)) file = sf;
    else if (pt && *pt) file = pt;
    else if (pi && *pi && dr && *dr) { snprintf(built, sizeof built, "%s%s", dr, pi); file = built; }
    else if (sf && *sf) file = sf;
    if (!file) { printf("Content-Type: text/plain\r\n\r\ncl cgi: the web server did not say which page to run (SCRIPT_FILENAME, PATH_TRANSLATED or PATH_INFO)\n"); return 1; }
    /* A request like /cgi-bin/cl/cl.secret can point the server at any file:
       run pages only, and never a database, seed, secret or saved email. */
    if (!ends_with_html(file) || is_private(file) || strstr(file, "/../")) {
        printf("Status: 403 Forbidden\r\nContent-Type: text/plain\r\n\r\nForbidden\n"); return 0;
    }

#ifndef _WIN32
    alarm(30);                         /* like the dev server: a runaway page gets 30 s */
#endif
    const char *query = getenv("QUERY_STRING"); if (query && !*query) query = NULL;
    /* The address the visitor asked for, like /blog/hello-world: REQUEST_URI
       still has it after a rewrite turned it into /cgi-bin/cl/blog.html. */
    char *path_buf = NULL;
    const char *ru = getenv("REQUEST_URI");
    if (ru && *ru) path_buf = url_decode(ru, strcspn(ru, "?"));
    const char *path = path_buf;
    if (!path || !*path) path = pi && *pi ? pi : getenv("SCRIPT_NAME");
    if (!path || !*path) path = "/";

    char *body = NULL;
    const char *cl = getenv("CONTENT_LENGTH");
    long n = cl ? atol(cl) : 0;
    if (n > 0 && n <= 1024 * 1024) { body = malloc((size_t)n + 1); size_t got = fread(body, 1, (size_t)n, stdin); body[got] = 0; }

    const char *method = getenv("REQUEST_METHOD");
    if (method && !strcmp(method, "POST") && cross_site(getenv("HTTP_ORIGIN"), getenv("HTTP_HOST"), getenv("HTTP_X_FORWARDED_HOST"))) {
        printf("Status: 403 Forbidden\r\nContent-Type: text/plain\r\n\r\nForbidden: this form was sent from another site\n"); free(body); free(path_buf); return 0;
    }
    size_t len; char *html = read_file(file, &len);
    const char *status = "200 OK";
    char page404[4200];
    if (!html) {                       /* a missing page: the site's own 404.html, as cl serve does */
        char dir[4096]; dir_of(file, dir, sizeof dir);
        snprintf(page404, sizeof page404, "%s/404.html", dir);
        html = read_file(page404, &len);
        if (!html) { printf("Status: 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNot found\n"); free(body); free(path_buf); return 0; }
        file = page404; status = "404 Not Found";
    }

    Response r; memset(&r, 0, sizeof r);
    run_page(file, html, query, body, path, getenv("HTTP_COOKIE"), status, &r);
    printf(SAFETY_HEADERS);
    if (r.set_cookie) printf("Set-Cookie: %s\r\n", r.set_cookie);
    if (r.location[0]) printf("Status: 303 See Other\r\nLocation: %s\r\n\r\n", r.location);
    else { printf("Status: %s\r\nContent-Type: %s\r\n\r\n", r.status, r.type); fwrite(r.body, 1, r.len, stdout); }
    free(r.body); free(r.set_cookie); free(html); free(body); free(path_buf);
    return 0;
}

/* ================================================================== */
/* Command line                                                        */
/* ================================================================== */

static void usage(void) {
    fprintf(stderr,
        "Common (cl) interpreter\n\n"
        "  cl page.html [query] [form] [path] [cookie]\n"
        "                                       run a page and print the finished HTML\n"
        "                                       query: \"name=Ada\"  form: POST data  path: \"/blog/x\"\n"
        "                                       cookie: the Cookie header, to test remembered values\n"
        "  cl serve [port] [dir]                serve a folder over HTTP (default: 8000, current folder)\n"
        "                                       port can be host:port, like 0.0.0.0:8000, to allow other machines\n"
        "  cl cgi                               run as a CGI program behind a web server\n"
        "  cl words [--markdown]                print every word in the language (as Markdown, for docs and AI agents)\n"
        "  cl version                           print the version\n");
    exit(2);
}

int main(int argc, char **argv) {
    stack_setup();                                           /* see stack_check */
    modules_load();                                          /* the databases this build was linked with */
    if (argc < 2) usage();

    if (strcmp(argv[1], "words") == 0) {
        if (argc > 2 && strcmp(argv[2], "--markdown") == 0) print_words_markdown(); else print_words();
        return 0;
    }
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) { printf("Common (CL) %s\n", CL_VERSION); return 0; }
    if (strcmp(argv[1], "cgi") == 0)   return run_cgi();

    if (strcmp(argv[1], "serve") == 0) {
        /* cl serve 8000  or  cl serve 0.0.0.0:8000 to accept connections from other machines (Docker) */
        char host[64] = ""; const char *portarg = argc > 2 ? argv[2] : "8000";
        const char *colon = strchr(portarg, ':');
        if (colon) { snprintf(host, sizeof host, "%.*s", (int)(colon - portarg), portarg); portarg = colon + 1; }
        int port = atoi(portarg);
        if (port <= 0 || port > 65535) { fprintf(stderr, "cl: '%s' is not a port number\n", argv[2]); return 2; }
        const char *dir = argc > 3 ? argv[3] : ".";
        serve(host[0] ? host : NULL, port, dir);
        return 0;
    }

    char *html = read_file(argv[1], NULL);
    if (!html) { fprintf(stderr, "cl: cannot read %s\n", argv[1]); return 1; }
#ifndef _WIN32
    /* From the command line, include "/x.html" starts in the folder cl is run
       from, as long as the page is inside it: cd mysite, then cl shop/index.html. */
    {
        char cwd[2048], real[4096];
        if (getcwd(cwd, sizeof cwd) && realpath(argv[1], real) && !strncmp(real, cwd, strlen(cwd)) && real[strlen(cwd)] == '/')
            snprintf(g_include_root, sizeof g_include_root, "%s", cwd);
    }
#endif

    Response r; memset(&r, 0, sizeof r);
    run_page(argv[1], html,
             argc > 2 && argv[2][0] ? argv[2] : NULL,
             argc > 3 && argv[3][0] ? argv[3] : NULL,
             argc > 4 && argv[4][0] ? argv[4] : "/",
             argc > 5 && argv[5][0] ? argv[5] : NULL, "200 OK", &r);
    fwrite(r.body, 1, r.len, stdout);
    if (r.location[0]) fprintf(stderr, "cl: this page goes to %s\n", r.location);
    if (r.set_cookie) fprintf(stderr, "cl: set-cookie: %s\n", r.set_cookie);
    free(r.body); free(r.set_cookie); free(html);
    return 0;
}
