#define sysctl test_sysctl
#include "../../dlls/ntdll/unix/horizon_network.c"
#include <assert.h>
#include <stdio.h>

static unsigned char if_table[4096], route_table[4096];
static size_t if_size, route_size;
static int native_error, resize_once, test_errno;
static Service bsd_service = { .session = 1 };
static Result nifm_result;
static unsigned int nifm_refs;

static const unsigned char captured_loopback[232] =
{
    0xa8,0x00,0x05,0x0e,0x10,0x00,0x00,0x00,0x49,0x80,0x00,0x00,0x01,0x00,0x00,0x00,
    0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    [0x50]=0x0f,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x33,0xc3,0x05,0x00,0x00,0x00,0x00,0x00,
    0x38,0x12,0x01,0x00,0x18,0x03,0x00,0x00,0x6c,0x6f,0x30,0x00,0x00,0x00,0x00,0x00,
    [0xa8]=0x40,0x00,0x05,0x0c,0xa4,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x05,0x02,0x00,0x00,0xff,0x00,0x00,0x00,0x10,0x02,0x00,0x00,
    0x7f,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x02,0x00,0x00,
    0x7f,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const unsigned char captured_route[248] =
{
    0xf8,0x00,0x05,0x04,0x02,0x00,0x00,0x00,0x01,0x00,0x10,0x00,0x37,0x00,0x00,0x00,
    [0x28]=0xdc,0x05,
    [0x4c]=0x01,
    [0x5c]=0x10,0x02,0x00,0x00,0xc0,0xa8,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x36,0x12,0x02,0x00,0x06,
    [0xa0]=0x00,0x00,0xff,0xff,0x07,0xff,0xff,0xff,0xff,0xff,0xff,0x00,
    0x38,0x12,0x02,0x00,0x06,0x03,0x06,0x00,0x77,0x6c,0x30,0x02,0x12,0x34,0x56,0x78,0x9a,
    [0xe4]=0x10,0x02,0x00,0x00,0xc0,0xa8,0x1f,0xb8
};

int *__errno(void) { return &test_errno; }
void __assert_func(const char *file, int line, const char *function, const char *expression)
{ printf("%s:%d %s: %s\n",file,line,function,expression); abort(); }
Service *bsdGetServiceSession(void) { return &bsd_service; }
Result nifmInitialize(NifmServiceType type) { nifm_refs++; return 0; }
void nifmExit(void) { assert(nifm_refs); nifm_refs--; }
Result nifmGetInternetConnectionStatus(NifmInternetConnectionType *type, u32 *strength, NifmInternetConnectionStatus *status)
{ *type=NifmInternetConnectionType_WiFi; *strength=3; *status=NifmInternetConnectionStatus_Connected; return nifm_result; }
Result nifmGetCurrentIpConfigInfo(u32 *addr, u32 *mask, u32 *gateway, u32 *primary, u32 *secondary)
{ *addr=0x0a01a8c0; *mask=0x00ffffff; *gateway=*primary=*secondary=0x0101a8c0; return 0; }

int test_sysctl(const int *mib, unsigned int count, void *out, size_t *size, const void *in, size_t in_size)
{
    const void *data=mib[4]==NET_RT_IFLIST ? if_table : route_table;
    size_t needed=mib[4]==NET_RT_IFLIST ? if_size : route_size;
    assert(count==6 && mib[0]==CTL_NET && mib[1]==PF_ROUTE && mib[3]==AF_INET && !in && !in_size);
    if(native_error) { errno=native_error; return -1; }
    if(out && resize_once) { resize_once=0; errno=ENOMEM; return -1; }
    if(out) { assert(*size>=needed); memcpy(out,data,needed); }
    *size=needed;
    return 0;
}

static void put32(unsigned char *out, uint32_t value)
{
    for (unsigned int i=0;i<4;i++) out[i]=value>>(i*8);
}

static void prepare(void)
{
    memset(if_table,0,sizeof(if_table)); memset(route_table,0,sizeof(route_table));
    memcpy(if_table,captured_loopback,sizeof(captured_loopback));
    if_size=sizeof(captured_loopback);
    if_table[12]=if_table[114]=if_table[180]=3;
    if_table[16]=if_table[116]=71;
    if_table[20]=LINK_STATE_UP;
    put32(if_table+24,1500); put32(if_table+32,72000000);
    put32(if_table+36,100); put32(if_table+44,200);
    put32(if_table+56,1234); put32(if_table+60,5678);
    put32(if_table+64,5); put32(if_table+68,6);
    if_table[117]=5; if_table[118]=6;
    memcpy(if_table+120,"wlan0\x02\x12\x34\x56\x78\x9a",11);
    if_table[188]=7;
    put32(if_table+192,0x00ffffff); put32(if_table+200,0x0a01a8c0);
    route_size=136;
    route_table[0]=136; route_table[2]=RTM_VERSION; route_table[3]=RTM_GET; route_table[4]=3;
    put32(route_table+8,RTF_UP|RTF_GATEWAY);
    put32(route_table+12,RTA_DST|RTA_GATEWAY|RTA_NETMASK);
    route_table[92]=route_table[108]=16;
    route_table[93]=route_table[109]=AF_INET;
    put32(route_table+112,0x0101a8c0);
}

int main(void)
{
    struct horizon_network_snapshot snapshot;
    uint32_t servers[2]; unsigned int count;
    memcpy(if_table,captured_loopback,sizeof(captured_loopback));
    if_size=sizeof(captured_loopback);
    assert(!horizon_network_query(&snapshot,0));
    assert(snapshot.interface_count==1 && snapshot.address_count==1);
    assert(snapshot.interfaces[0].index==1 && snapshot.interfaces[0].type==24);
    assert(!strcmp(snapshot.interfaces[0].name,"lo0") && !snapshot.interfaces[0].mac_length);
    assert(snapshot.interfaces[0].mtu==16384 && snapshot.interfaces[0].connected);
    assert(snapshot.addresses[0].address==0x0100007f && snapshot.addresses[0].mask==0xff);
    horizon_network_free(&snapshot);
    prepare();
    resize_once=1;
    assert(!horizon_network_query(&snapshot,1));
    assert(snapshot.interface_count==1 && snapshot.address_count==1 && snapshot.route_count==1);
    assert(snapshot.interfaces[0].index==3 && snapshot.interfaces[0].mtu==1500);
    assert(snapshot.interfaces[0].connected && snapshot.interfaces[0].mac_length==6);
    assert(!memcmp(snapshot.interfaces[0].mac,"\x02\x12\x34\x56\x78\x9a",6));
    assert(snapshot.interfaces[0].in_bytes==1234 && snapshot.interfaces[0].speed==72000000);
    assert(snapshot.interfaces[0].out_bytes==5678 && snapshot.interfaces[0].in_packets==100);
    assert(snapshot.interfaces[0].out_packets==200 && snapshot.interfaces[0].in_multicast==5);
    assert(snapshot.interfaces[0].out_multicast==6 && !snapshot.interfaces[0].out_drops);
    assert(snapshot.addresses[0].address==0x0a01a8c0 && snapshot.addresses[0].mask==0x00ffffff);
    assert(snapshot.routes[0].gateway==0x0101a8c0 && !snapshot.routes[0].mask);
    horizon_network_free(&snapshot);
    memcpy(route_table,captured_route,sizeof(captured_route)); route_size=sizeof(captured_route);
    if_table[12]=if_table[114]=if_table[180]=2;
    assert(!horizon_network_query(&snapshot,1) && snapshot.route_count==1);
    assert(snapshot.routes[0].index==2 && snapshot.routes[0].prefix==0x001fa8c0);
    assert(snapshot.routes[0].mask==0x00ffffff && !snapshot.routes[0].gateway && !snapshot.routes[0].loopback);
    horizon_network_free(&snapshot);
    memcpy(route_table+route_size,captured_route,sizeof(captured_route)); route_size*=2;
    put32(route_table+sizeof(captured_route)+44,17);
    assert(!horizon_network_query(&snapshot,1) && snapshot.route_count==2);
    assert(!snapshot.routes[0].metric && snapshot.routes[1].metric==17);
    horizon_network_free(&snapshot);
    for(size_t size=1;size<route_size;size++)
    {
        size_t saved=route_size;
        route_size=size;
        int result=horizon_network_query(&snapshot,1);
        assert(!result || result==EIO);
        horizon_network_free(&snapshot);
        route_size=saved;
    }
    for(size_t i=0;i<route_size;i++)
    {
        route_table[i]^=0xff;
        horizon_network_query(&snapshot,1);
        horizon_network_free(&snapshot);
        route_table[i]^=0xff;
    }
    prepare();
    if_table[22]=2;
    assert(!horizon_network_query(&snapshot,0) && snapshot.interfaces[0].speed==7200000000ULL);
    horizon_network_free(&snapshot);
    if_table[22]=255;
    assert(horizon_network_query(&snapshot,0)==EIO && !snapshot.interfaces);
    if_table[22]=0;
    if_table[23]=63;
    assert(horizon_network_query(&snapshot,0)==EIO && !snapshot.interfaces);
    if_table[23]=255;
    assert(horizon_network_query(&snapshot,0)==EIO && !snapshot.interfaces);
    if_table[23]=96;
    for(size_t size=1;size<if_size;size++)
    {
        size_t saved=if_size;
        if_size=size;
        int result=horizon_network_query(&snapshot,1);
        assert(!result || result==EIO);
        horizon_network_free(&snapshot);
        if_size=saved;
    }
    for(size_t i=0;i<if_size;i++)
    {
        if_table[i]^=0xff;
        horizon_network_query(&snapshot,1);
        horizon_network_free(&snapshot);
        if_table[i]^=0xff;
    }
    native_error=EIO;
    assert(horizon_network_query(&snapshot,1)==EIO && !snapshot.interfaces);
    native_error=0; if_size=route_size=0;
    assert(!horizon_network_query(&snapshot,1) && !snapshot.interface_count);
    horizon_network_free(&snapshot);
    bsd_service.session=INVALID_HANDLE;
    assert(horizon_network_query(&snapshot,1)==ENETDOWN);
    assert(!horizon_network_dns(servers,&count) && count==1 && servers[0]==0x0101a8c0 && !nifm_refs);
    nifm_result=0xd46ed;
    assert(!horizon_network_dns(servers,&count) && !count && !nifm_refs);
    nifm_result=1;
    assert(horizon_network_dns(servers,&count)==EIO && !count && !nifm_refs);
    puts("Horizon network: captured Switch adapters/routes, 32-bit counters/metrics, link speed, malformed records and offline DNS passed");
    return 0;
}
