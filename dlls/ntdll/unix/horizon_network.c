/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <switch/services/bsd.h>
#include <switch/services/nifm.h>

#include "horizon_network.h"

/* Horizon retains 32-bit BSD interface counters, unlike libnx's if_data. */
struct horizon_ifinfo
{
    uint16_t length;
    uint8_t version, type;
    uint32_t addresses, flags;
    uint16_t index, reserved;
    struct
    {
        uint8_t type, physical, address_length, header_length;
        uint8_t link_state, vhid, baudrate_power, length;
        uint32_t mtu, metric, baudrate;
        uint32_t in_packets, in_errors, out_packets, out_errors, collisions;
        uint32_t in_bytes, out_bytes, in_multicast, out_multicast, in_drops, no_protocol;
    } data;
};

_Static_assert( offsetof(struct horizon_ifinfo, data) == 16, "Horizon interface header" );
_Static_assert( sizeof(struct horizon_ifinfo) == 80, "Horizon interface counter layout" );

struct horizon_route_message
{
    uint16_t length;
    uint8_t version, type;
    uint16_t index, reserved;
    uint32_t flags, addresses, pid, sequence, error, flags_mask, initialized;
    struct
    {
        uint32_t locks, mtu, hopcount, expire, recvpipe, sendpipe;
        uint32_t ssthresh, rtt, rttvar, packets, weight, reserved[3];
    } metrics;
};

_Static_assert( offsetof(struct horizon_route_message, metrics) == 36, "Horizon route metrics" );
_Static_assert( sizeof(struct horizon_route_message) == 92, "Horizon route header" );

static int read_table( int table, unsigned char **buffer, size_t *size )
{
    int mib[] = { CTL_NET, PF_ROUTE, 0, AF_INET, table, 0 };
    unsigned int attempt;

    *buffer = NULL;
    for (attempt = 0; attempt < 3; attempt++)
    {
        size_t capacity = 0;
        int error;

        if (sysctl( mib, 6, NULL, &capacity, NULL, 0 )) return errno;
        if (!capacity) { *size = 0; return 0; }
        if (!(*buffer = malloc( capacity ))) return ENOMEM;
        *size = capacity;
        if (!sysctl( mib, 6, *buffer, size, NULL, 0 ))
        {
            if (*size <= capacity) return 0;
            error = EIO;
        }
        else error = errno;
        free( *buffer );
        *buffer = NULL;
        if (error != ENOMEM) return error;
    }
    return EAGAIN;
}

static int read_sockaddrs( const unsigned char *data, size_t size, unsigned int mask,
                           const unsigned char **addresses )
{
    unsigned int i;
    memset( addresses, 0, RTAX_MAX * sizeof(*addresses) );
    for (i = 0; i < RTAX_MAX; i++)
    {
        size_t length;
        if (!(mask & (1u << i))) continue;
        if (size < 2) return EIO;
        length = data[0] ? (data[0] + sizeof(long) - 1) & ~(sizeof(long) - 1) : sizeof(long);
        if (length > size) return EIO;
        addresses[i] = data;
        data += length;
        size -= length;
    }
    return 0;
}

static uint32_t read_ipv4( const unsigned char *address )
{
    uint32_t result = 0;
    size_t offset = offsetof(struct sockaddr_in, sin_addr), length;

    /* Routing netmasks can be shorter than sockaddr_in, including length zero. */
    if (!address || address[0] <= offset) return 0;
    length = address[0] - offset;
    if (length > sizeof(result)) length = sizeof(result);
    memcpy( &result, address + offset, length );
    return result;
}

static int parse_interfaces( const unsigned char *data, size_t size, struct horizon_network_snapshot *snapshot )
{
    while (size)
    {
        const unsigned char *addresses[RTAX_MAX];
        uint16_t length;
        int error;

        if (size < 4) goto invalid;
        memcpy( &length, data, sizeof(length) );
        if (length < 4 || length > size || data[2] != RTM_VERSION) goto invalid;
        if (data[3] == RTM_IFINFO)
        {
            struct horizon_ifinfo message;
            struct horizon_network_interface *iface;
            const unsigned char *link;
            size_t name_length, mac_length, header_size, offset = offsetof(struct sockaddr_dl, sdl_data);

            if (length < sizeof(message)) goto invalid;
            memcpy( &message, data, sizeof(message) );
            header_size = offsetof(struct horizon_ifinfo, data) + message.data.length;
            if (header_size < sizeof(message) || header_size > length) goto invalid;
            if ((error = read_sockaddrs( data + header_size, length - header_size,
                                        message.addresses, addresses ))) goto invalid;
            link = addresses[RTAX_IFP];
            if (!link || link[0] < offset || link[1] != AF_LINK) goto invalid;
            name_length = link[offsetof(struct sockaddr_dl, sdl_nlen)];
            mac_length = link[offsetof(struct sockaddr_dl, sdl_alen)];
            if (offset + name_length + mac_length > link[0]) goto invalid;
            iface = &snapshot->interfaces[snapshot->interface_count++];
            if (name_length >= sizeof(iface->name) || mac_length > sizeof(iface->mac)) goto invalid;
            memcpy( iface->name, link + offset, name_length );
            memcpy( iface->mac, link + offset + name_length, mac_length );
            iface->mac_length = mac_length;
            iface->index = message.index;
            iface->type = message.data.type;
            iface->mtu = message.data.mtu;
            iface->metric = message.data.metric;
            iface->up = !!(message.flags & IFF_UP);
            iface->connected = iface->up && message.data.link_state != LINK_STATE_DOWN;
            iface->speed = message.data.baudrate;
            for (unsigned int power = 0; power < message.data.baudrate_power; power++)
            {
                if (iface->speed > UINT64_MAX / 10) goto invalid;
                iface->speed *= 10;
            }
            iface->in_bytes = message.data.in_bytes;
            iface->out_bytes = message.data.out_bytes;
            iface->in_packets = message.data.in_packets;
            iface->out_packets = message.data.out_packets;
            iface->in_multicast = message.data.in_multicast;
            iface->out_multicast = message.data.out_multicast;
            iface->in_errors = message.data.in_errors;
            iface->out_errors = message.data.out_errors;
            iface->in_drops = message.data.in_drops;
        }
        else if (data[3] == RTM_NEWADDR)
        {
            struct ifa_msghdr message;
            struct horizon_network_address *addr;

            if (length < sizeof(message)) goto invalid;
            memcpy( &message, data, sizeof(message) );
            if ((error = read_sockaddrs( data + sizeof(message), length - sizeof(message),
                                        message.ifam_addrs, addresses ))) goto invalid;
            if (addresses[RTAX_IFA] && addresses[RTAX_IFA][1] == AF_INET)
            {
                if (addresses[RTAX_IFA][0] < sizeof(struct sockaddr_in)) goto invalid;
                addr = &snapshot->addresses[snapshot->address_count++];
                addr->index = message.ifam_index;
                addr->address = read_ipv4( addresses[RTAX_IFA] );
                addr->mask = read_ipv4( addresses[RTAX_NETMASK] );
            }
        }
        data += length;
        size -= length;
    }
    return 0;
invalid:
    return EIO;
}

static int parse_routes( const unsigned char *data, size_t size, struct horizon_network_snapshot *snapshot )
{
    while (size)
    {
        struct horizon_route_message message;
        struct horizon_network_route *route;
        const unsigned char *addresses[RTAX_MAX];
        unsigned int i;
        int error;

        if (size < sizeof(message)) goto invalid;
        memcpy( &message, data, sizeof(message) );
        if (message.length < sizeof(message) || message.length > size ||
            message.version != RTM_VERSION) goto invalid;
        if ((error = read_sockaddrs( data + sizeof(message), message.length - sizeof(message),
                                    message.addresses, addresses ))) goto invalid;
        if ((message.flags & RTF_UP) && !(message.flags & (RTF_REJECT | RTF_BLACKHOLE | RTF_LLDATA)) &&
            addresses[RTAX_DST] && addresses[RTAX_DST][1] == AF_INET)
        {
            for (i = 0; i < snapshot->interface_count; i++)
                if (snapshot->interfaces[i].index == message.index) break;
            if (i < snapshot->interface_count)
            {
                route = &snapshot->routes[snapshot->route_count++];
                route->index = message.index;
                route->metric = message.metrics.hopcount;
                route->loopback = snapshot->interfaces[i].type == 24;
                route->prefix = read_ipv4( addresses[RTAX_DST] );
                route->mask = (message.flags & RTF_HOST) ? UINT32_MAX : read_ipv4( addresses[RTAX_NETMASK] );
                if (addresses[RTAX_GATEWAY] && addresses[RTAX_GATEWAY][1] == AF_INET)
                    route->gateway = read_ipv4( addresses[RTAX_GATEWAY] );
            }
        }
        data += message.length;
        size -= message.length;
    }
    return 0;
invalid:
    return EIO;
}

void horizon_network_free( struct horizon_network_snapshot *snapshot )
{
    free( snapshot->interfaces );
    free( snapshot->addresses );
    free( snapshot->routes );
    memset( snapshot, 0, sizeof(*snapshot) );
}

int horizon_network_query( struct horizon_network_snapshot *snapshot, int routes )
{
    unsigned char *buffer = NULL;
    size_t size;
    int error;

    memset( snapshot, 0, sizeof(*snapshot) );
    if (!serviceIsActive( bsdGetServiceSession() )) return ENETDOWN;
    if ((error = read_table( NET_RT_IFLIST, &buffer, &size ))) return error;
    if (size)
    {
        snapshot->interfaces = calloc( size / sizeof(struct horizon_ifinfo) + 1, sizeof(*snapshot->interfaces) );
        snapshot->addresses = calloc( size / sizeof(struct ifa_msghdr) + 1, sizeof(*snapshot->addresses) );
        if (!snapshot->interfaces || !snapshot->addresses) error = ENOMEM;
        else error = parse_interfaces( buffer, size, snapshot );
    }
    free( buffer );
    buffer = NULL;
    if (!error && routes && !(error = read_table( NET_RT_DUMP, &buffer, &size )) && size)
    {
        snapshot->routes = calloc( size / sizeof(struct horizon_route_message) + 1, sizeof(*snapshot->routes) );
        if (!snapshot->routes) error = ENOMEM;
        else error = parse_routes( buffer, size, snapshot );
    }
    free( buffer );
    if (error) horizon_network_free( snapshot );
    return error;
}

int horizon_network_dns( uint32_t servers[2], unsigned int *count )
{
    NifmInternetConnectionType type;
    NifmInternetConnectionStatus state;
    u32 strength, address, mask, gateway, primary, secondary;
    Result result;

    *count = 0;
    if (R_FAILED( result = nifmInitialize( NifmServiceType_User ) )) return EIO;
    result = nifmGetInternetConnectionStatus( &type, &strength, &state );
    if (result == 0xd46ed || (R_SUCCEEDED(result) && state != NifmInternetConnectionStatus_Connected)) result = 0;
    else if (R_SUCCEEDED(result))
    {
        result = nifmGetCurrentIpConfigInfo( &address, &mask, &gateway, &primary, &secondary );
        if (R_SUCCEEDED(result))
        {
            if (primary) servers[(*count)++] = primary;
            if (secondary && secondary != primary) servers[(*count)++] = secondary;
        }
    }
    nifmExit();
    return R_SUCCEEDED(result) ? 0 : EIO;
}
