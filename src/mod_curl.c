/* mod_curl.c - the fetch module for Common: addresses on the web, by libcurl.
 *
 * Compiled in when curl/curl.h is installed (the Makefile checks). Sends the
 * request 'fetch' asks for and hands back the status, the content type and the
 * reply. The core has already checked the address (http or https, a host no
 * visitor chose), added the keys for that host, and counts the requests; it
 * turns the reply into values and marks them as another site's. See
 * cl_module.h for the rules a module follows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cl_module.h"

typedef struct { char *data; size_t len, cap, max; int too_big; } Reply;

static size_t cu_write(char *p, size_t size, size_t n, void *ud) {
    Reply *r = ud; size_t add = size * n;
    if (r->len + add > r->max) { r->too_big = 1; return 0; }          /* 0 stops the transfer */
    if (r->len + add + 1 > r->cap) {
        size_t cap = r->cap ? r->cap : 16384;
        while (cap < r->len + add + 1) cap *= 2;
        char *d = realloc(r->data, cap); if (!d) return 0;
        r->data = d; r->cap = cap;
    }
    memcpy(r->data + r->len, p, add); r->len += add; r->data[r->len] = 0;
    return add;
}

static void cu_fetch(const CLHost *h, const CLFetchRequest *req, CLFetchReply *rep) {
    CURL *c = curl_easy_init();
    if (!c) h->fail("could not start libcurl to fetch %s", req->host);
    Reply r = { NULL, 0, 0, req->max_bytes, 0 };
    char err[CURL_ERROR_SIZE] = "", agent[64];
    snprintf(agent, sizeof agent, "Common/%s", h->version);

    struct curl_slist *hd = curl_slist_append(NULL, "Accept: application/json, text/plain;q=0.9, */*;q=0.5");
    if (req->body) hd = curl_slist_append(hd, "Content-Type: application/json");
    for (int i = 0; i < req->nheaders; i++) hd = curl_slist_append(hd, req->headers[i]);

    curl_easy_setopt(c, CURLOPT_URL, req->url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hd);
    curl_easy_setopt(c, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, req->seconds);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, req->seconds < 5 ? req->seconds : 5L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");                 /* gzip and the rest, whatever this libcurl has */
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cu_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)req->max_bytes);
    /* Only http and https, also after a redirect. A request with a key never
       follows one, so the key cannot travel on to another address. */
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, req->nheaders ? 0L : 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    if (req->body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(req->body));
    }

    CURLcode rc = curl_easy_perform(c);
    long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    char *ct = NULL; curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    char *type = NULL;                                                 /* no strdup: it is not C11 */
    if (ct) { size_t n = strlen(ct) + 1; type = malloc(n); if (type) memcpy(type, ct, n); }
    char why[CURL_ERROR_SIZE + 64];
    snprintf(why, sizeof why, "%s", err[0] ? err : curl_easy_strerror(rc));
    curl_slist_free_all(hd); curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        free(r.data); free(type);                                      /* fail does not return */
        long secs = req->seconds;
        if (r.too_big || rc == CURLE_FILESIZE_EXCEEDED)
            h->fail("the reply from %s is larger than %zu MB, the most 'fetch' reads", req->host, req->max_bytes >> 20);
        if (rc == CURLE_OPERATION_TIMEDOUT) h->fail("%s did not answer within %ld seconds", req->host, secs);
        if (rc == CURLE_COULDNT_RESOLVE_HOST) h->fail("cannot find the server %s. Check the address, and that this computer is online", req->host);
        if (rc == CURLE_COULDNT_CONNECT) h->fail("%s did not take the connection. Is the address right, and the server running?", req->host);
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR)
            h->fail("the secure connection to %s failed: %s. Its certificate may be wrong or out of date", req->host, why);
        if (rc == CURLE_UNSUPPORTED_PROTOCOL || rc == CURLE_TOO_MANY_REDIRECTS)
            h->fail("%s sent the request on to an address 'fetch' does not follow (%s)", req->host, why);
        h->fail("could not fetch from %s: %s", req->host, why);
    }
    rep->status = status;
    rep->content_type = type;
    rep->body = r.data ? r.data : calloc(1, 1);
    rep->len = r.len;
}

static void cu_free_reply(CLFetchReply *rep) {
    free(rep->body); free(rep->content_type);
    rep->body = NULL; rep->content_type = NULL; rep->len = 0;
}

static const CLFetch cu_fetcher = { "libcurl", cu_fetch, cu_free_reply };
static const CLCapability cu_caps[] = { { CL_CAP_FETCH, &cu_fetcher } };
static CLModule cu_module = { CL_MODULE_API, "curl", "libcurl", cu_caps, 1 };
static char cu_library[64];

CL_MODULE(curl) {
    (void)host;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    snprintf(cu_library, sizeof cu_library, "libcurl %s", curl_version_info(CURLVERSION_NOW)->version);
    cu_module.library = cu_library;
    return &cu_module;
}
