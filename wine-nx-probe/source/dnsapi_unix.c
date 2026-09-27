/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#define USE_WS_PREFIX
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "windns.h"
#include "inaddr.h"
#include "ws2def.h"
#include "ws2ipdef.h"
#include "wine/unixlib.h"
#include "horizon_network.h"

struct get_searchlist_params { WCHAR *list; DWORD *len; };
struct get_serverlist_params { USHORT family; DNS_ADDR_ARRAY *addrs; DWORD *len; };

static NTSTATUS get_searchlist( void *args )
{
    struct get_searchlist_params *params = args;
    DWORD capacity;

    if (!params->len) return ERROR_INVALID_PARAMETER;
    capacity = *params->len;
    *params->len = sizeof(WCHAR);
    if (!params->list) return ERROR_SUCCESS;
    if (capacity < sizeof(WCHAR)) return ERROR_MORE_DATA;
    *params->list = 0;
    return ERROR_SUCCESS;
}

static NTSTATUS get_serverlist( void *args )
{
    struct get_serverlist_params *params = args;
    uint32_t servers[2];
    unsigned int count = 0, i;
    DWORD needed, capacity;

    if (!params->len || (params->family != WS_AF_INET && params->family != WS_AF_INET6 &&
                        params->family != WS_AF_UNSPEC)) return ERROR_INVALID_PARAMETER;
    if (params->family != WS_AF_INET6 && horizon_network_dns( servers, &count )) return ERROR_GEN_FAILURE;
    needed = offsetof(DNS_ADDR_ARRAY, AddrArray[count]);
    capacity = *params->len;
    *params->len = needed;
    if (!params->addrs) return ERROR_SUCCESS;
    if (capacity < needed) return ERROR_MORE_DATA;
    memset( params->addrs, 0, needed );
    params->addrs->MaxCount = params->addrs->AddrCount = count;
    params->addrs->Family = params->family;
    for (i = 0; i < count; i++)
    {
        struct WS_sockaddr_in address = {0};
        address.sin_family = WS_AF_INET;
        address.sin_addr.S_un.S_addr = servers[i];
        memcpy( params->addrs->AddrArray[i].MaxSa, &address, sizeof(address) );
        params->addrs->AddrArray[i].Data.DnsAddrUserDword[0] = sizeof(address);
    }
    return ERROR_SUCCESS;
}

static NTSTATUS set_serverlist( void *args )
{
    (void)args;
    return ERROR_NOT_SUPPORTED;
}

static NTSTATUS query( void *args )
{
    (void)args;
    return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
}

static NTSTATUS wow64_get_searchlist( void *args )
{
    const struct { ULONG list, len; } *params32 = args;
    struct get_searchlist_params params = { ULongToPtr(params32->list), ULongToPtr(params32->len) };
    return get_searchlist( &params );
}

static NTSTATUS wow64_get_serverlist( void *args )
{
    const struct { USHORT family; ULONG addrs, len; } *params32 = args;
    struct get_serverlist_params params = { params32->family, ULongToPtr(params32->addrs), ULongToPtr(params32->len) };
    return get_serverlist( &params );
}

const unixlib_entry_t wine_nx_dnsapi_unix_funcs[] = { get_searchlist, get_serverlist, set_serverlist, query };
const unixlib_entry_t wine_nx_dnsapi_wow64_unix_funcs[] = { wow64_get_searchlist, wow64_get_serverlist, set_serverlist, query };
const unsigned int wine_nx_dnsapi_wow64_unix_count = ARRAY_SIZE(wine_nx_dnsapi_wow64_unix_funcs);
