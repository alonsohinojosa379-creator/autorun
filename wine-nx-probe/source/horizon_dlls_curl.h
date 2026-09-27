#ifndef WINE_NX_HORIZON_DLLS_CURL_H
#define WINE_NX_HORIZON_DLLS_CURL_H

#include "horizon_dlls.h"

/* A transport to the DLL repository over HTTPS; 0 when curl cannot start. */
int horizon_dlls_curl_open( struct horizon_dlls_transport *transport );
void horizon_dlls_curl_close( struct horizon_dlls_transport *transport );

#endif
