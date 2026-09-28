#include <assert.h>
#include <stdio.h>
#include <wchar.h>
#include <sys/mman.h>
#define USE_WS_PREFIX
#include "winsock2.h"
#include "../../dlls/ntdll/unix/horizon_nsi.c"
#include "../source/dnsapi_unix.c"
#include "iphlpapi.h"

static int query_error, offline;
static unsigned int queries;
static DWORD last_error;

int horizon_network_query( struct horizon_network_snapshot *snapshot, int routes )
{
    unsigned int i;
    queries++;
    if (query_error) return query_error;
    snapshot->interface_count = 2;
    snapshot->interfaces = calloc( 2, sizeof(*snapshot->interfaces) );
    snapshot->address_count = offline ? 1 : 2;
    snapshot->addresses = calloc( 2, sizeof(*snapshot->addresses) );
    for (i = 0; i < 2; i++)
    {
        snapshot->interfaces[i].index = i + 1;
        snapshot->interfaces[i].type = i ? IF_TYPE_IEEE80211 : IF_TYPE_SOFTWARE_LOOPBACK;
        snapshot->interfaces[i].up = snapshot->interfaces[i].connected = !i || !offline;
        snapshot->interfaces[i].mtu = i ? 1500 : 16384;
        strcpy( snapshot->interfaces[i].name, i ? "wlan0" : "lo0" );
        snapshot->addresses[i].index = i + 1;
    }
    snapshot->interfaces[1].mac_length = 6;
    memcpy( snapshot->interfaces[1].mac, "\x02\x12\x34\x56\x78\x9a", 6 );
    snapshot->addresses[0].address = 0x0100007f;
    snapshot->addresses[0].mask = 0x000000ff;
    snapshot->addresses[1].address = 0x0a01a8c0;
    snapshot->addresses[1].mask = 0x00ffffff;
    if (routes && !offline)
    {
        snapshot->route_count = 1;
        snapshot->routes = calloc( 1, sizeof(*snapshot->routes) );
        snapshot->routes[0].index = 2;
        snapshot->routes[0].gateway = 0x0101a8c0;
    }
    return 0;
}

void horizon_network_free( struct horizon_network_snapshot *snapshot )
{
    free( snapshot->interfaces ); free( snapshot->addresses ); free( snapshot->routes );
    memset( snapshot, 0, sizeof(*snapshot) );
}

int horizon_network_dns( uint32_t servers[2], unsigned int *count )
{
    servers[0] = 0x0101a8c0;
    *count = offline ? 0 : 1;
    return query_error;
}

static void test_tables(void)
{
    struct nsiproxy_enumerate_all request = {0};
    unsigned int i, count, size, status;
    void *output;

    assert( horizon_nsi_device_name(L"\\??\\nSi", 14) );
    assert( horizon_nsi_device_name(L"\\device\\NSI", 22) );
    assert( !horizon_nsi_device_name(L"\\??\\NsiX", 16) );
    assert( !horizon_nsi_device_name(L"\\??\\Nsi", 13) );
    assert( !horizon_nsi_device_name(NULL, 0) );
    for (i = 0; i < ARRAY_SIZE(tables); i++)
    {
        const struct nsi_table *table = &tables[i];
        unsigned int row_size = table->sizes[0] + table->sizes[1] + table->sizes[2] + table->sizes[3];
        request.module = *table->module;
        request.table = table->id;
        request.count = 0;
        request.key_size = request.rw_size = request.dynamic_size = request.static_size = 0;
        status = horizon_nsi_ioctl( IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request), &output, 4, &size );
        assert( !status && size == 4 );
        memcpy( &count, output, 4 );
        assert( count == (table->ipv6 ? 0 : table->kind == FORWARD ? 1 : 2) );
        free( output );
        request.key_size = table->sizes[0]; request.rw_size = table->sizes[1];
        request.dynamic_size = table->sizes[2]; request.static_size = table->sizes[3];
        for (request.count = 0; request.count < 4; request.count++)
        {
            status = horizon_nsi_ioctl( IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request),
                                        &output, 4 + row_size * request.count, &size );
            assert( status == (request.count < count ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS) );
            assert( *(UINT *)output == min(count, request.count) && size == 4 + row_size * request.count );
            if (count && request.count)
            {
                struct nsiproxy_get_parameter parameter = {0};
                BYTE input[256];
                unsigned int part, offset = 4 + request.key_size * request.count;
                parameter.module = *table->module; parameter.table = table->id; parameter.key_size = table->sizes[0];
                for (part = 1; part < 4; part++)
                {
                    void *value;
                    unsigned int value_size;
                    if (table->sizes[part])
                    {
                        parameter.param_type = part - 1;
                        memcpy( input, &parameter, offsetof(struct nsiproxy_get_parameter, key) );
                        memcpy( input + offsetof(struct nsiproxy_get_parameter, key), (BYTE *)output + 4, parameter.key_size );
                        assert( !horizon_nsi_ioctl( IOCTL_NSIPROXY_WINE_GET_PARAMETER, input,
                                                   offsetof(struct nsiproxy_get_parameter, key) + parameter.key_size,
                                                   &value, table->sizes[part], &value_size ) );
                        assert( value_size == table->sizes[part] && !memcmp(value, (BYTE *)output + offset, value_size) );
                        free( value );
                    }
                    offset += request.count * table->sizes[part];
                }
            }
            free( output );
        }
        request.count = UINT32_MAX;
        assert( horizon_nsi_ioctl( IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request),
                                   &output, UINT32_MAX, &size ) == STATUS_INVALID_PARAMETER );
        assert( !size && !output );
    }
    request.module = NPI_MS_NDIS_MODULEID; request.table = NSI_NDIS_IFINFO_TABLE;
    request.count = 0; request.key_size = request.rw_size = request.dynamic_size = request.static_size = 0;
    query_error = ENOMEM;
    assert( horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request), &output, 4, &size) == STATUS_NO_MEMORY );
    assert( !output && !size );
    query_error = EIO;
    assert( horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request), &output, 4, &size) == STATUS_DEVICE_NOT_READY );
    query_error = 0;
    assert( horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION, NULL, 0, &output, 4, &size) == STATUS_NOT_SUPPORTED );
    request.table = 123;
    assert( horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &request, sizeof(request), &output, 4, &size) == STATUS_NOT_SUPPORTED );
    puts("NSI: table columns, counts, short buffers, parameter lookups, overflow and native failures passed");
}

HANDLE WINAPI GetProcessHeap(void) { return (HANDLE)1; }
void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) { return flags & HEAP_ZERO_MEMORY ? calloc(1,size) : malloc(size); }
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *ptr) { free(ptr); return TRUE; }
HANDLE WINAPI CreateFileW(const WCHAR *path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE template)
{ assert(!wcscmp(path,L"\\\\.\\Nsi")); return (HANDLE)2; }
BOOL WINAPI CloseHandle(HANDLE handle) { return TRUE; }
DWORD WINAPI GetLastError(void) { return last_error; }
BOOL WINAPI DeviceIoControl(HANDLE handle, DWORD code, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *received, OVERLAPPED *overlapped)
{
    void *output;
    unsigned int size, status = horizon_nsi_ioctl(code,in,in_size,&output,out_size,&size);
    if (size) memcpy(out,output,size);
    free(output);
    *received = size;
    last_error = !status ? 0 : status == STATUS_BUFFER_OVERFLOW ? ERROR_MORE_DATA : ERROR_INVALID_PARAMETER;
    return !status;
}
INT WINAPI MultiByteToWideChar(UINT cp, DWORD flags, const char *in, INT in_size, WCHAR *out, INT out_size)
{
    int length = in_size < 0 ? strlen(in) + 1 : in_size;
    if (!out) return length;
    assert(out_size >= length);
    for (int i=0;i<length;i++) out[i]=(unsigned char)in[i];
    return length;
}
DNS_STATUS WINAPI DnsQueryConfig(DNS_CONFIG_TYPE config, DWORD flags, const WCHAR *adapter, void *reserved, void *buffer, DWORD *size)
{
    if (config == DnsConfigSearchList)
    {
        struct get_searchlist_params p = {buffer,size};
        return get_searchlist(&p);
    }
    struct get_serverlist_params p = {config == DnsConfigDnsServersIpv4 ? WS_AF_INET :
                                    config == DnsConfigDnsServersIpv6 ? WS_AF_INET6 : WS_AF_UNSPEC, buffer, size};
    return get_serverlist(&p);
}

size_t test_wcslen(const wchar_t *s) { const wchar_t *end=s; while(*end) end++; return end-s; }
wchar_t *test_wcscpy(wchar_t *out, const wchar_t *in) { wchar_t *start=out; do { *out++=*in; } while(*in++); return start; }
int test_wcscmp(const wchar_t *a, const wchar_t *b) { while(*a && *a==*b) {a++;b++;} return *a-*b; }

static void test_adapters(void)
{
    for (offline=0;offline<=1;offline++)
    for (unsigned int family=0;family<2;family++)
    {
        ULONG size=0;
        unsigned int seen=0;
        IP_ADAPTER_ADDRESSES *adapters, *aa;
        assert(GetAdaptersAddresses(family ? WS_AF_INET : WS_AF_UNSPEC,0,NULL,NULL,&size)==ERROR_BUFFER_OVERFLOW);
        assert(size>sizeof(*adapters));
        adapters=malloc(size);
        assert(!GetAdaptersAddresses(family ? WS_AF_INET : WS_AF_UNSPEC,0,NULL,adapters,&size));
        for(aa=adapters;aa;aa=aa->Next)
        {
            assert(aa->IfIndex==1 || aa->IfIndex==2);
            assert(aa->Luid.Info.NetLuidIndex==aa->IfIndex);
            assert(aa->PhysicalAddressLength==(aa->IfIndex==1 ? 0 : 6));
            assert(!!aa->FirstUnicastAddress==(aa->IfIndex==1 || !offline));
            assert(aa->OperStatus==(aa->IfIndex==1 || !offline ? IfOperStatusUp : IfOperStatusDown));
            seen++;
        }
        assert(seen==2);
        free(adapters);
    }
    puts("Wine GetAdaptersAddresses: AF_INET/AF_UNSPEC, online/offline, MAC/LUID, buffer retry and DNS passed");
}

static void test_validation(void)
{
    struct nsiproxy_get_all_parameters request = {0};
    struct nsi_ipv4_unicast_key key = {0};
    BYTE input[128], random_input[128];
    void *output;
    unsigned int size, value = 0x31415926;

    offline = 0;
    request.module = NPI_MS_IPV4_MODULEID; request.table = NSI_IP_UNICAST_TABLE;
    request.key_size = sizeof(key); request.rw_size = sizeof(struct nsi_ip_unicast_rw);
    key.luid.Info.NetLuidIndex = 2; key.luid.Info.IfType = IF_TYPE_IEEE80211;
    key.addr.S_un.S_addr = 0x0a01a8c0; key.pad = ~0u;
    memcpy(input,&request,offsetof(struct nsiproxy_get_all_parameters,key));
    memcpy(input+offsetof(struct nsiproxy_get_all_parameters,key),&key,sizeof(key));
    assert(!horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS,input,
                              offsetof(struct nsiproxy_get_all_parameters,key)+sizeof(key),&output,request.rw_size,&size));
    assert(((struct nsi_ip_unicast_rw *)output)->on_link_prefix==24);
    free(output);
    for (unsigned int i=0;i<10000;i++)
    {
        for (unsigned int j=0;j<sizeof(random_input);j++)
        { value=value*1664525u+1013904223u; random_input[j]=value>>24; }
        horizon_nsi_ioctl(IOCTL_NSIPROXY_WINE_ENUMERATE_ALL+(i%3)*4,random_input,i%sizeof(random_input),&output,i%4096,&size);
        assert(!output && !size);
    }
    {
        BYTE *low=mmap((void *)0x10000000,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        struct { USHORT family; ULONG addrs,len; } params;
        struct { ULONG list,len; } search;
        assert(low!=MAP_FAILED && (uintptr_t)low+65536<UINT32_MAX);
        params.family=WS_AF_INET; params.addrs=(uintptr_t)low; params.len=(uintptr_t)(low+4096);
        *(DWORD *)(low+4096)=4096;
        assert(!wine_nx_dnsapi_wow64_unix_funcs[1](&params));
        assert(((DNS_ADDR_ARRAY *)low)->AddrCount==1);
        search.list=(uintptr_t)low; search.len=params.len;
        assert(!wine_nx_dnsapi_wow64_unix_funcs[0](&search));
        assert(!*(WCHAR *)low && *(DWORD *)(low+4096)==sizeof(WCHAR));
        munmap(low,65536);
    }
    puts("NSI: padding-independent lookup, malformed requests and 32-bit DNS marshalling passed");
}

int main(void)
{
    test_tables();
    test_adapters();
    test_validation();
    return 0;
}
