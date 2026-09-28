#ifndef WINE_NX_HORIZON_DLLS_CURL_H
#define WINE_NX_HORIZON_DLLS_CURL_H

#include "horizon_dlls.h"

/* Transports to the DLL repository over HTTPS, one per connection, sharing
 * their DNS and TLS sessions; returns how many could be opened. */
unsigned int horizon_dlls_curl_open( struct horizon_dlls_transport *transports, unsigned int count );
void horizon_dlls_curl_close( struct horizon_dlls_transport *transports, unsigned int count );
/* How a transport's last download went -- curl's result, the HTTP status,
 * the bytes, the time and the host -- for the log. */
const char *horizon_dlls_curl_last( const struct horizon_dlls_transport *transport );

#endif
