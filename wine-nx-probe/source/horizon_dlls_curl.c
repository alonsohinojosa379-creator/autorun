/* The DLL repository over HTTPS: one connection kept for every file, since an
 * update is hundreds of small downloads from the same host. */
#include "horizon_dlls_curl.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

struct horizon_dlls_curl
{
    CURL *curl;
};

struct transfer
{
    horizon_dlls_sink sink;
    void *context;
    horizon_dlls_progress progress;
    void *opaque;
    const char *url;
    int stopped, cancelled;
};

static size_t receive( void *data, size_t size, size_t count, void *opaque )
{
    struct transfer *t = opaque;

    if (count && size > (size_t)-1 / count) { t->stopped = 1; return 0; }
    if (!t->sink( t->context, data, size * count )) { t->stopped = 1; return 0; }
    return size * count;
}

static int report( void *opaque, curl_off_t total, curl_off_t current, curl_off_t upload_total,
                   curl_off_t upload_current )
{
    struct transfer *t = opaque;

    (void)upload_total;
    (void)upload_current;
    if (t->progress && t->progress( t->opaque, t->url, current > 0 ? (unsigned long long)current : 0,
                                    total > 0 ? (unsigned long long)total : 0 ))
        t->cancelled = 1;
    return t->cancelled;
}

static enum horizon_dlls_result fetch( void *opaque, const char *url, horizon_dlls_sink sink, void *context,
                                       horizon_dlls_progress progress, void *progress_opaque )
{
    struct horizon_dlls_curl *c = opaque;
    struct transfer t = { sink, context, progress, progress_opaque, url, 0, 0 };
    CURLcode code;
    long status = 0;

    /* Only the repository, over HTTPS; the engine checks the rest of the URL. */
    if (strncmp( url, HORIZON_DLLS_RAW, sizeof(HORIZON_DLLS_RAW) - 1 )) return HORIZON_DLLS_INVALID;
    if (curl_easy_setopt( c->curl, CURLOPT_URL, url ) ||
        curl_easy_setopt( c->curl, CURLOPT_WRITEFUNCTION, receive ) ||
        curl_easy_setopt( c->curl, CURLOPT_WRITEDATA, &t ) ||
        curl_easy_setopt( c->curl, CURLOPT_XFERINFOFUNCTION, report ) ||
        curl_easy_setopt( c->curl, CURLOPT_XFERINFODATA, &t ))
        return HORIZON_DLLS_NETWORK;
    code = curl_easy_perform( c->curl );
    curl_easy_getinfo( c->curl, CURLINFO_RESPONSE_CODE, &status );
    if (t.cancelled) return HORIZON_DLLS_CANCELLED;
    if (t.stopped) return HORIZON_DLLS_IO;
    if (code != CURLE_OK) return HORIZON_DLLS_NETWORK;
    if (status == 404) return HORIZON_DLLS_NOT_FOUND;
    if (status < 200 || status >= 300) return HORIZON_DLLS_NETWORK;
    return HORIZON_DLLS_OK;
}

int horizon_dlls_curl_open( struct horizon_dlls_transport *transport )
{
    struct horizon_dlls_curl *c;
    CURL *curl;

    if (curl_global_init( CURL_GLOBAL_DEFAULT ) != CURLE_OK || !(curl = curl_easy_init())) return 0;
    if (curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_MAXREDIRS, 5L ) ||
        curl_easy_setopt( curl, CURLOPT_CONNECTTIMEOUT, 12L ) ||
        /* No whole-transfer limit: a large file on a slow connection is fine
         * as long as it keeps coming. */
        curl_easy_setopt( curl, CURLOPT_LOW_SPEED_LIMIT, 1024L ) ||
        curl_easy_setopt( curl, CURLOPT_LOW_SPEED_TIME, 30L ) ||
        curl_easy_setopt( curl, CURLOPT_NOSIGNAL, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_NOPROGRESS, 0L ) ||
        curl_easy_setopt( curl, CURLOPT_SSL_VERIFYPEER, 1L ) ||
        curl_easy_setopt( curl, CURLOPT_SSL_VERIFYHOST, 2L ) ||
        curl_easy_setopt( curl, CURLOPT_USERAGENT, "Autorun/DLLs" ) ||
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt( curl, CURLOPT_PROTOCOLS_STR, "https" ) ||
        curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS_STR, "https" ) ||
#else
        curl_easy_setopt( curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS ) ||
        curl_easy_setopt( curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS ) ||
#endif
        !(c = calloc( 1, sizeof(*c) )))
    {
        curl_easy_cleanup( curl );
        return 0;
    }
    c->curl = curl;
    transport->fetch = fetch;
    transport->opaque = c;
    return 1;
}

void horizon_dlls_curl_close( struct horizon_dlls_transport *transport )
{
    struct horizon_dlls_curl *c = transport->opaque;

    if (!c) return;
    curl_easy_cleanup( c->curl );
    free( c );
    transport->opaque = NULL;
}
