#include <assert.h>
#include <sys/mman.h>

#define gethostid mock_gethostid
#define getaddrinfo mock_getaddrinfo
#define freeaddrinfo mock_freeaddrinfo
#include "../source/ws2_32_unix_stub.c"

static long local_ip;
static int dns_error, dns_calls, dns_frees;
static struct sockaddr_in addresses[2];
static struct addrinfo records[2];

long mock_gethostid(void) { return local_ip; }
void wine_nx_runtime_trace( const char *message ) { (void)message; }
int mock_getaddrinfo( const char *name, const char *service, const struct addrinfo *hints, struct addrinfo **out )
{
    assert( name && !strcmp(name, "example.test") );
    assert( !service && hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM );
    dns_calls++;
    *out = records;
    return dns_error;
}
void mock_freeaddrinfo( struct addrinfo *info ) { assert(info == records); dns_frees++; }

static void check_host( void *buffer, BOOL wow64, unsigned int count, const char *name, uint32_t ip )
{
    char *hostname;
    void *first;
    if (wow64)
    {
        struct ws_hostent32 *host = buffer;
        ULONG *list = ULongToPtr(host->addresses);
        hostname = ULongToPtr(host->name);
        assert(host->family == WS_AF_INET && host->length == 4);
        assert(!*(ULONG *)ULongToPtr(host->aliases));
        assert(!list[count]);
        first = ULongToPtr(list[0]);
    }
    else
    {
        struct ws_hostent *host = buffer;
        hostname = host->name;
        assert(host->family == WS_AF_INET && host->length == 4);
        assert(!host->aliases[0] && !host->addresses[count]);
        first = host->addresses[0];
    }
    assert(!strcmp(hostname, name));
    assert(!memcmp(first, &ip, 4));
}

int main(void)
{
    char *memory = mmap((void *)0x18000000, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    unsigned int *size = (void *)(memory + 0x1000), needed;
    struct { ULONG name, host, size; } byname;
    struct { ULONG name; unsigned int size; } hostname;
    int wide;

    assert(memory != MAP_FAILED);
    hostname.name = PtrToUlong(memory);
    hostname.size = 1;
    assert(wine_nx_wow64_gethostname(&hostname) == WS_ERROR_INSUFFICIENT_BUFFER);
    hostname.size = 100;
    assert(!wine_nx_wow64_gethostname(&hostname));
    assert(!strcmp(memory, "wine-nx"));
    byname.name = PtrToUlong(memory);
    byname.host = PtrToUlong(memory + 0x2000);
    byname.size = PtrToUlong(size);
    local_ip = htonl(0xc0a8010a);
    *size = 0;
    assert(wine_nx_ws2_32_wow64_unix_funcs[2](&byname) == WS_ERROR_INSUFFICIENT_BUFFER);
    needed = *size;
    memset(memory + 0x2000, 0x5a, needed + 1);
    *size = needed - 1;
    assert(wine_nx_ws2_32_wow64_unix_funcs[2](&byname) == WS_ERROR_INSUFFICIENT_BUFFER);
    assert((unsigned char)memory[0x2000] == 0x5a);
    assert(*size == needed);
    assert(!wine_nx_ws2_32_wow64_unix_funcs[2](&byname));
    check_host(memory + 0x2000, TRUE, 1, "wine-nx", local_ip);
    assert((unsigned char)memory[0x2000 + needed] == 0x5a);
    assert(!dns_calls);

    records[0].ai_family = records[1].ai_family = AF_INET;
    records[0].ai_addr = (void *)&addresses[0];
    records[1].ai_addr = (void *)&addresses[1];
    records[0].ai_next = &records[1];
    records[0].ai_canonname = "canonical.test";
    addresses[0].sin_addr.s_addr = htonl(0x0a000001);
    addresses[1].sin_addr.s_addr = htonl(0x0a000002);
    for (wide = 0; wide < 2; wide++)
    {
        *size = 1024;
        local_ip = INADDR_LOOPBACK;
        assert(!get_host_by_name("WINE-NX", memory + 0x2000, size, !wide));
        check_host(memory + 0x2000, !wide, 1, "wine-nx", htonl(INADDR_LOOPBACK));
        assert(!get_host_by_name("example.test", memory + 0x2000, size, !wide));
        check_host(memory + 0x2000, !wide, 2, "canonical.test", addresses[0].sin_addr.s_addr);
    }
    assert(dns_calls == 2 && dns_frees == 2);
    dns_error = EAI_AGAIN;
    assert(get_host_by_name("example.test", memory + 0x2000, size, TRUE) == WSATRY_AGAIN);
    assert(dns_frees == 2);
    assert(!munmap(memory, 0x10000));
    puts("Winsock hostname tests passed");
}
