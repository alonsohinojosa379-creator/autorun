/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define USE_WS_PREFIX
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ifdef.h"
#define __WINE_INIT_NPI_MODULEID
#include "netiodef.h"
#include "ipifcons.h"
#include "wine/nsi.h"
#include "horizon_network.h"
#include "horizon_nsi.h"

enum table_kind { IFINFO, INDEX_LUID, UNICAST, FORWARD, INTERFACE };

struct nsi_table
{
    const NPI_MODULEID *module;
    unsigned int id, sizes[4];
    enum table_kind kind;
    int ipv6;
};

static const struct nsi_table tables[] =
{
    { &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
      { sizeof(NET_LUID), sizeof(struct nsi_ndis_ifinfo_rw), sizeof(struct nsi_ndis_ifinfo_dynamic),
        sizeof(struct nsi_ndis_ifinfo_static) }, IFINFO, 0 },
    { &NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE, { sizeof(UINT), 0, 0, sizeof(NET_LUID) }, INDEX_LUID, 0 },
    { &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE,
      { sizeof(struct nsi_ipv4_unicast_key), sizeof(struct nsi_ip_unicast_rw),
        sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) }, UNICAST, 0 },
    { &NPI_MS_IPV6_MODULEID, NSI_IP_UNICAST_TABLE,
      { sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw),
        sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) }, UNICAST, 1 },
    { &NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE,
      { sizeof(struct nsi_ipv4_forward_key), sizeof(struct nsi_ip_forward_rw),
        sizeof(struct nsi_ipv4_forward_dynamic), sizeof(struct nsi_ip_forward_static) }, FORWARD, 0 },
    { &NPI_MS_IPV6_MODULEID, NSI_IP_FORWARD_TABLE,
      { sizeof(struct nsi_ipv6_forward_key), sizeof(struct nsi_ip_forward_rw),
        sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static) }, FORWARD, 1 },
    { &NPI_MS_IPV4_MODULEID, NSI_IP_INTERFACE_TABLE,
      { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw),
        sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static) }, INTERFACE, 0 },
    { &NPI_MS_IPV6_MODULEID, NSI_IP_INTERFACE_TABLE,
      { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw),
        sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static) }, INTERFACE, 1 },
};

struct nsi_row
{
    union { NET_LUID luid; UINT index; struct nsi_ipv4_unicast_key unicast; struct nsi_ipv4_forward_key forward; } key;
    union { struct nsi_ndis_ifinfo_rw iface; struct nsi_ip_unicast_rw unicast;
            struct nsi_ip_forward_rw forward; struct nsi_ip_interface_rw ip; } rw;
    union { struct nsi_ndis_ifinfo_dynamic iface; struct nsi_ip_unicast_dynamic unicast;
            struct nsi_ipv4_forward_dynamic forward; struct nsi_ip_interface_dynamic ip; } dynamic;
    union { struct nsi_ndis_ifinfo_static iface; NET_LUID luid; struct nsi_ip_unicast_static unicast;
            struct nsi_ip_forward_static forward; struct nsi_ip_interface_static ip; } stat;
};

int horizon_nsi_device_name( const void *name, unsigned int size )
{
    static const char *paths[] = { "\\??\\nsi", "\\Device\\nsi" };
    unsigned int i, j;

    for (i = 0; i < ARRAY_SIZE(paths); i++)
    {
        if (size != strlen(paths[i]) * sizeof(WCHAR)) continue;
        for (j = 0; j < size / sizeof(WCHAR); j++)
        {
            WCHAR c;
            char expected = paths[i][j];
            memcpy( &c, (const BYTE *)name + j * sizeof(c), sizeof(c) );
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (expected >= 'A' && expected <= 'Z') expected += 'a' - 'A';
            if (c != expected) break;
        }
        if (j == size / sizeof(WCHAR)) return 1;
    }
    return 0;
}

static const struct nsi_table *find_table( const NPI_MODULEID *module, unsigned int id )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(tables); i++)
        if (tables[i].id == id && NmrIsEqualNpiModuleId( tables[i].module, module )) return &tables[i];
    return NULL;
}

static NET_LUID interface_luid( const struct horizon_network_interface *iface )
{
    NET_LUID luid = {0};
    luid.Info.NetLuidIndex = iface->index;
    luid.Info.IfType = iface->type;
    return luid;
}

static unsigned int prefix_length( uint32_t mask )
{
    const unsigned char *bytes = (const unsigned char *)&mask;
    unsigned int i, bits = 0;
    for (i = 0; i < 32 && (bytes[i / 8] & (0x80u >> (i % 8))); i++) bits++;
    return bits;
}

static void counted_string( IF_COUNTED_STRING *out, const char *name )
{
    unsigned int i;
    for (i = 0; name[i] && i < ARRAY_SIZE(out->String) - 1; i++) out->String[i] = (unsigned char)name[i];
    out->Length = i * sizeof(WCHAR);
}

static unsigned int row_count( const struct nsi_table *table, const struct horizon_network_snapshot *snapshot )
{
    if (table->ipv6) return 0; /* Horizon's BSD service exposes IPv4 only. */
    if (table->kind == UNICAST) return snapshot->address_count;
    if (table->kind == FORWARD) return snapshot->route_count;
    return snapshot->interface_count;
}

static int fill_row( const struct nsi_table *table, const struct horizon_network_snapshot *snapshot,
                     unsigned int number, struct nsi_row *row )
{
    const struct horizon_network_interface *iface;
    const struct horizon_network_address *address = NULL;
    const struct horizon_network_route *route = NULL;
    unsigned int index, i;
    NET_LUID luid;

    memset( row, 0, sizeof(*row) );
    if (table->kind == UNICAST) index = (address = &snapshot->addresses[number])->index;
    else if (table->kind == FORWARD) index = (route = &snapshot->routes[number])->index;
    else index = snapshot->interfaces[number].index;
    for (i = 0; i < snapshot->interface_count; i++)
        if (snapshot->interfaces[i].index == index) break;
    if (i == snapshot->interface_count) return 0;
    iface = &snapshot->interfaces[i];
    luid = interface_luid( iface );
    row->key.luid = luid;

    switch (table->kind)
    {
    case IFINFO:
        row->rw.iface.admin_status = iface->up ? MIB_IF_ADMIN_STATUS_UP : MIB_IF_ADMIN_STATUS_DOWN;
        counted_string( &row->rw.iface.alias, iface->name );
        row->rw.iface.phys_addr.Length = iface->mac_length;
        memcpy( row->rw.iface.phys_addr.Address, iface->mac, iface->mac_length );
        row->dynamic.iface.oper_status = iface->connected ? IfOperStatusUp : IfOperStatusDown;
        row->dynamic.iface.flags.not_media_conn = !iface->connected;
        row->dynamic.iface.media_conn_state = iface->connected ? MediaConnectStateConnected : MediaConnectStateDisconnected;
        row->dynamic.iface.mtu = iface->mtu;
        row->dynamic.iface.xmit_speed = row->dynamic.iface.rcv_speed = iface->speed;
        row->dynamic.iface.in_octets = iface->in_bytes;
        row->dynamic.iface.out_octets = iface->out_bytes;
        row->dynamic.iface.in_ucast_pkts = iface->in_packets >= iface->in_multicast ? iface->in_packets - iface->in_multicast : 0;
        row->dynamic.iface.out_ucast_pkts = iface->out_packets >= iface->out_multicast ? iface->out_packets - iface->out_multicast : 0;
        row->dynamic.iface.in_mcast_pkts = iface->in_multicast;
        row->dynamic.iface.out_mcast_pkts = iface->out_multicast;
        row->dynamic.iface.in_errors = iface->in_errors;
        row->dynamic.iface.out_errors = iface->out_errors;
        row->dynamic.iface.in_discards = iface->in_drops;
        row->dynamic.iface.out_discards = iface->out_drops;
        row->stat.iface.if_index = iface->index;
        counted_string( &row->stat.iface.descr, iface->name );
        row->stat.iface.type = iface->type;
        row->stat.iface.access_type = iface->type == IF_TYPE_SOFTWARE_LOOPBACK ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
        row->stat.iface.conn_type = NET_IF_CONNECTION_DEDICATED;
        row->stat.iface.if_guid.Data1 = iface->index;
        memcpy( row->stat.iface.if_guid.Data4 + 2, "NetDev", 6 );
        row->stat.iface.conn_present = iface->type != IF_TYPE_SOFTWARE_LOOPBACK;
        row->stat.iface.perm_phys_addr = row->rw.iface.phys_addr;
        row->stat.iface.flags.hw = iface->type != IF_TYPE_SOFTWARE_LOOPBACK;
        break;
    case INDEX_LUID:
        row->key.index = iface->index;
        row->stat.luid = luid;
        break;
    case UNICAST:
        row->key.unicast.addr.S_un.S_addr = address->address;
        row->rw.unicast.preferred_lifetime = row->rw.unicast.valid_lifetime = ~0u;
        row->rw.unicast.prefix_origin = IpPrefixOriginOther;
        row->rw.unicast.suffix_origin = IpSuffixOriginOther;
        row->rw.unicast.on_link_prefix = prefix_length( address->mask );
        row->dynamic.unicast.dad_state = IpDadStatePreferred;
        break;
    case FORWARD:
        memset( &row->key, 0, sizeof(row->key) );
        row->key.forward.luid = row->key.forward.luid2 = luid;
        row->key.forward.prefix.S_un.S_addr = route->prefix;
        row->key.forward.prefix_len = prefix_length( route->mask );
        row->key.forward.next_hop.S_un.S_addr = route->gateway;
        row->rw.forward.valid_lifetime = row->rw.forward.preferred_lifetime = ~0u;
        row->rw.forward.metric = route->metric;
        row->rw.forward.protocol = MIB_IPPROTO_LOCAL;
        row->rw.forward.loopback = route->loopback;
        row->rw.forward.immortal = 1;
        row->dynamic.forward.addr2 = row->key.forward.prefix;
        row->stat.forward.origin = NlroManual;
        row->stat.forward.if_index = iface->index;
        break;
    case INTERFACE:
        row->rw.ip.metric = iface->metric;
        row->rw.ip.mtu = iface->mtu;
        row->dynamic.ip.if_index = iface->index;
        row->dynamic.ip.connected = iface->connected;
        break;
    }
    return 1;
}

static int key_equal( const struct nsi_table *table, const struct nsi_row *row, const void *key )
{
    struct nsi_row other = {0};
    memcpy( &other.key, key, table->sizes[0] );
    if (table->kind == UNICAST)
        return row->key.unicast.luid.Value == other.key.unicast.luid.Value &&
               row->key.unicast.addr.S_un.S_addr == other.key.unicast.addr.S_un.S_addr;
    if (table->kind == FORWARD)
        return row->key.forward.luid.Value == other.key.forward.luid.Value &&
               row->key.forward.prefix.S_un.S_addr == other.key.forward.prefix.S_un.S_addr &&
               row->key.forward.prefix_len == other.key.forward.prefix_len &&
               row->key.forward.next_hop.S_un.S_addr == other.key.forward.next_hop.S_un.S_addr;
    return !memcmp( &row->key, key, table->sizes[0] );
}

unsigned int horizon_nsi_ioctl( unsigned int code, const void *input, unsigned int input_size,
                                void **output, unsigned int capacity, unsigned int *size )
{
    union
    {
        struct nsiproxy_enumerate_all enumerate;
        struct nsiproxy_get_all_parameters all;
        struct nsiproxy_get_parameter parameter;
    } request;
    const struct nsi_table *table;
    struct horizon_network_snapshot snapshot = {0};
    unsigned int sizes[4] = {0}, count = 1, id, key_offset = 0, type = 0, offset = 0;
    unsigned int rows, written = 0, i, part, total, status = STATUS_SUCCESS;
    const void *parts[4];
    struct nsi_row row;
    BYTE *columns[4], *buffer;
    uint64_t required = 0;
    int error;

    *output = NULL;
    *size = 0;
    memset( &request, 0, sizeof(request) );
    switch (code)
    {
    case IOCTL_NSIPROXY_WINE_ENUMERATE_ALL:
        if (input_size != sizeof(request.enumerate)) return STATUS_INVALID_PARAMETER;
        memcpy( &request.enumerate, input, input_size );
        id = request.enumerate.table;
        sizes[0] = request.enumerate.key_size;
        sizes[1] = request.enumerate.rw_size;
        sizes[2] = request.enumerate.dynamic_size;
        sizes[3] = request.enumerate.static_size;
        count = request.enumerate.count;
        required = sizeof(UINT);
        break;
    case IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS:
        key_offset = offsetof(struct nsiproxy_get_all_parameters, key);
        if (input_size < key_offset) return STATUS_INVALID_PARAMETER;
        memcpy( &request.all, input, key_offset );
        id = request.all.table;
        sizes[0] = request.all.key_size;
        sizes[1] = request.all.rw_size;
        sizes[2] = request.all.dynamic_size;
        sizes[3] = request.all.static_size;
        break;
    case IOCTL_NSIPROXY_WINE_GET_PARAMETER:
        key_offset = offsetof(struct nsiproxy_get_parameter, key);
        if (input_size < key_offset) return STATUS_INVALID_PARAMETER;
        memcpy( &request.parameter, input, key_offset );
        id = request.parameter.table;
        sizes[0] = request.parameter.key_size;
        type = request.parameter.param_type + 1;
        offset = request.parameter.data_offset;
        if (type < 1 || type > 3) return STATUS_INVALID_PARAMETER;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (!(table = find_table( &request.enumerate.module, id ))) return STATUS_NOT_SUPPORTED;
    for (part = 0; part < 4; part++)
        if (sizes[part] && sizes[part] != table->sizes[part]) return STATUS_INVALID_PARAMETER;
    if (key_offset && (sizes[0] != table->sizes[0] || sizes[0] > input_size - key_offset))
        return STATUS_INVALID_PARAMETER;
    if (type)
    {
        if (offset > table->sizes[type] || capacity > table->sizes[type] - offset) return STATUS_INVALID_PARAMETER;
        required = capacity;
    }
    else for (part = !!key_offset; part < 4; part++) required += (uint64_t)sizes[part] * count;
    if (required > capacity) return STATUS_INVALID_PARAMETER;
    total = required;
    if (total && !(*output = calloc( 1, total ))) return STATUS_NO_MEMORY;
    buffer = *output;
    if (!table->ipv6 && (error = horizon_network_query( &snapshot, table->kind == FORWARD )))
    {
        status = error == ENOMEM ? STATUS_NO_MEMORY : STATUS_DEVICE_NOT_READY;
        goto done;
    }
    rows = row_count( table, &snapshot );
    parts[0] = &row.key;
    parts[1] = &row.rw;
    parts[2] = &row.dynamic;
    parts[3] = &row.stat;
    if (key_offset)
    {
        for (i = 0; i < rows; i++)
            if (fill_row( table, &snapshot, i, &row ) &&
                key_equal( table, &row, (const BYTE *)input + key_offset )) break;
        if (i == rows) { status = STATUS_OBJECT_NAME_NOT_FOUND; goto done; }
        if (type)
        {
            if (total) memcpy( buffer, (const BYTE *)parts[type] + offset, total );
        }
        else for (part = 1; part < 4; part++)
        {
            if (sizes[part]) { memcpy( buffer, parts[part], sizes[part] ); buffer += sizes[part]; }
        }
    }
    else
    {
        buffer += sizeof(UINT);
        for (part = 0; part < 4; part++)
        {
            columns[part] = buffer;
            buffer += sizes[part] * count;
        }
        for (i = 0; i < rows; i++)
        {
            if (!fill_row( table, &snapshot, i, &row )) continue;
            if (written < count)
                for (part = 0; part < 4; part++)
                    if (sizes[part]) memcpy( columns[part] + written * sizes[part], parts[part], sizes[part] );
            written++;
        }
        if (total > sizeof(UINT) && written > count)
        {
            written = count;
            status = STATUS_BUFFER_OVERFLOW;
        }
        else if (!count && (sizes[0] || sizes[1] || sizes[2] || sizes[3]) && written)
        {
            written = 0;
            status = STATUS_BUFFER_OVERFLOW;
        }
        memcpy( *output, &written, sizeof(written) );
    }
    *size = total;
done:
    horizon_network_free( &snapshot );
    if (!*size) { free( *output ); *output = NULL; }
    return status;
}
