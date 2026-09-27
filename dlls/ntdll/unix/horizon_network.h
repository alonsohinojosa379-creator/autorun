#ifndef WINE_HORIZON_NETWORK_H
#define WINE_HORIZON_NETWORK_H

#include <stdint.h>

struct horizon_network_interface
{
    unsigned int index, type, mtu, metric, up, connected;
    char name[64];
    unsigned char mac[32];
    unsigned int mac_length;
    uint64_t speed, in_bytes, out_bytes, in_packets, out_packets;
    uint64_t in_multicast, out_multicast, in_errors, out_errors, in_drops, out_drops;
};

struct horizon_network_address
{
    unsigned int index;
    uint32_t address, mask;
};

struct horizon_network_route
{
    unsigned int index, metric, loopback;
    uint32_t prefix, mask, gateway;
};

struct horizon_network_snapshot
{
    unsigned int interface_count, address_count, route_count;
    struct horizon_network_interface *interfaces;
    struct horizon_network_address *addresses;
    struct horizon_network_route *routes;
};

int horizon_network_query( struct horizon_network_snapshot *snapshot, int routes );
void horizon_network_free( struct horizon_network_snapshot *snapshot );
int horizon_network_dns( uint32_t servers[2], unsigned int *count );

#endif
