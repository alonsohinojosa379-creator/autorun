#include "windef.h"
#include "winbase.h"
#include "ifdef.h"
#include "netiodef.h"
#include "wine/nsi.h"

const unsigned int nsi_wire_layout[] =
{
    sizeof(NPI_MODULEID), sizeof(struct nsiproxy_enumerate_all),
    offsetof(struct nsiproxy_get_all_parameters, key), offsetof(struct nsiproxy_get_parameter, key),
    sizeof(NET_LUID), sizeof(struct nsi_ndis_ifinfo_rw), sizeof(struct nsi_ndis_ifinfo_dynamic),
    sizeof(struct nsi_ndis_ifinfo_static), sizeof(struct nsi_ipv4_unicast_key),
    sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw),
    sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static),
    sizeof(struct nsi_ipv4_forward_key), sizeof(struct nsi_ipv6_forward_key),
    sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv4_forward_dynamic),
    sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static),
    sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw),
    sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static),
    offsetof(struct nsi_ndis_ifinfo_static, if_guid), offsetof(struct nsi_ndis_ifinfo_static, type),
    offsetof(struct nsi_ndis_ifinfo_rw, phys_addr), offsetof(struct nsi_ndis_ifinfo_dynamic, xmit_speed),
    offsetof(struct nsi_ipv4_unicast_key, addr), offsetof(struct nsi_ipv4_forward_key, luid),
    offsetof(struct nsi_ipv4_forward_key, next_hop)
};
