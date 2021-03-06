/* BGP EVPN
 * Copyright (C) 2016 Cumulus Networks, Inc.
 *
 * This file is part of Quagga.
 *
 * Quagga is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * Quagga is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GN5U General Public License
 * along with Quagga; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _QUAGGA_BGP_EVPN_H
#define _QUAGGA_BGP_EVPN_H

#include "vxlan.h"
#include "zebra.h"
#include "hash.h"
#include "vty.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ecommunity.h"

/* EVPN route types. */
typedef enum
{
  BGP_EVPN_AD_ROUTE = 1,          /* Ethernet Auto-Discovery (A-D) route */
  BGP_EVPN_MAC_IP_ROUTE,          /* MAC/IP Advertisement route */
  BGP_EVPN_IMET_ROUTE,            /* Inclusive Multicast Ethernet Tag route */
  BGP_EVPN_ES_ROUTE,              /* Ethernet Segment route */
  BGP_EVPN_IP_PREFIX_ROUTE,       /* IP Prefix route */
} bgp_evpn_route_type;

/*
 * Hash table of VNIs - configured, learnt and local.
 * TODO: Configuration is not supported right now.
 */
struct bgpevpn
{
  vni_t                     vni;
  u_int32_t                 flags;
#define VNI_FLAG_CFGD              0x1  /* VNI is user configured */
#define VNI_FLAG_LIVE              0x2  /* VNI is "live" */
#define VNI_FLAG_RD_CFGD           0x4  /* RD is user configured. */
#define VNI_FLAG_IMPRT_CFGD        0x8  /* Import RT is user configured */
#define VNI_FLAG_EXPRT_CFGD        0x10 /* Export RT is user configured */

  /* RD for this VNI. */
  struct prefix_rd          prd;

  /* Route type 3 field */
  struct in_addr            originator_ip;

  /* Import and Export RTs. */
  struct ecommunity         *import_rtl;
  struct ecommunity         *export_rtl;

  QOBJ_FIELDS
};

DECLARE_QOBJ_TYPE(bgpevpn)

/* Mapping of Import RT to VNIs.
 * The Import RTs of all VNIs are maintained in a hash table with each
 * RT linking to all VNIs that will import routes matching this RT.
 */
struct irt_node
{
  /* RT */
  struct ecommunity_val rt;

  /* List of VNIs importing routes matching this RT. */
  struct list *vnis;
};

#define EVPN_ROUTE_LEN 50

#define RT_TYPE_IMPORT 1
#define RT_TYPE_EXPORT 2

static inline int
is_vni_configured (struct bgpevpn *vpn)
{
  return (CHECK_FLAG (vpn->flags, VNI_FLAG_CFGD));
}

static inline int
is_vni_live (struct bgpevpn *vpn)
{
  return (CHECK_FLAG (vpn->flags, VNI_FLAG_LIVE));
}

static inline int
is_rd_configured (struct bgpevpn *vpn)
{
  return (CHECK_FLAG (vpn->flags, VNI_FLAG_RD_CFGD));
}

static inline int
bgp_evpn_rd_matches_existing (struct bgpevpn *vpn, struct prefix_rd *prd)
{
  return(memcmp (&vpn->prd.val, prd->val, ECOMMUNITY_SIZE) == 0);
}

static inline int
bgp_evpn_rt_matches_existing (struct ecommunity *ecom,
                              struct ecommunity *ecomadd)
{
  return (ecommunity_match (ecom, ecomadd));
}

static inline int
is_import_rt_configured (struct bgpevpn *vpn)
{
  return (CHECK_FLAG (vpn->flags, VNI_FLAG_IMPRT_CFGD));
}

static inline int
is_export_rt_configured (struct bgpevpn *vpn)
{
  return (CHECK_FLAG (vpn->flags, VNI_FLAG_EXPRT_CFGD));
}

static inline int
is_vni_param_configured (struct bgpevpn *vpn)
{
  return (is_rd_configured (vpn) ||
          is_import_rt_configured (vpn) ||
          is_export_rt_configured (vpn));
}

extern char *
bgp_evpn_route2str (struct prefix_evpn *p, char *buf, int len);
extern void
bgp_evpn_map_vni_to_its_rts (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_evpn_unmap_vni_from_its_rts (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_evpn_derive_auto_rt_import (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_evpn_derive_auto_rt_export (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_evpn_derive_auto_rd (struct bgp *bgp, struct bgpevpn *vpn);
extern struct bgpevpn *
bgp_evpn_lookup_vni (struct bgp *bgp, vni_t vni);
extern struct bgpevpn *
bgp_evpn_new (struct bgp *bgp, vni_t vni, struct in_addr originator_ip);
extern void
bgp_evpn_free (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_evpn_encode_prefix (struct stream *s, struct prefix *p,
                        struct prefix_rd *prd, int addpath_encode,
                        u_int32_t addpath_tx_id);
extern int
bgp_evpn_nlri_sanity_check (struct peer *peer, int afi, safi_t safi,
                            u_char *pnt, bgp_size_t length, int *numpfx);
extern int
bgp_evpn_nlri_parse (struct peer *peer, struct attr *attr, struct bgp_nlri *packet);
extern int
bgp_evpn_install_route (struct bgp *bgp, afi_t afi, safi_t safi,
                        struct prefix *p, struct bgp_info *ri);
extern int
bgp_evpn_uninstall_route (struct bgp *bgp, afi_t afi, safi_t safi,
                          struct prefix *p, struct bgp_info *ri);
extern void
bgp_evpn_handle_router_id_update (struct bgp *bgp, int withdraw) ;
extern int
bgp_evpn_install_routes (struct bgp *bgp, struct bgpevpn *vpn);
extern int
bgp_evpn_uninstall_routes (struct bgp *bgp, struct bgpevpn *vpn);
extern int
bgp_evpn_handle_export_rt_change (struct bgp *bgp, struct bgpevpn *vpn);
extern int
bgp_evpn_local_macip_add (struct bgp *bgp, vni_t vni,
                          struct ethaddr *mac);
extern int
bgp_evpn_local_macip_del (struct bgp *bgp, vni_t vni,
                          struct ethaddr *mac);
extern int
bgp_evpn_local_vni_add (struct bgp *bgp, vni_t vni, struct in_addr originator_ip);
extern int
bgp_evpn_local_vni_del (struct bgp *bgp, vni_t vni);
extern void
bgp_evpn_init (struct bgp *bgp);
extern void
bgp_evpn_cleanup_on_disable (struct bgp *bgp);
extern void
bgp_evpn_cleanup (struct bgp *bgp);


/* UI functions */

extern void
bgp_evpn_configure_import_rt (struct bgp *bgp, struct bgpevpn *vpn,
                              struct ecommunity *ecomadd);
extern void
bgp_evpn_unconfigure_import_rt (struct bgp *bgp, struct bgpevpn *vpn,
                                struct ecommunity *ecomadd);
extern void
bgp_evpn_configure_export_rt (struct bgp *bgp, struct bgpevpn *vpn,
                              struct ecommunity *ecomadd);
extern void
bgp_evpn_unconfigure_export_rt (struct bgp *bgp, struct bgpevpn *vpn,
                                struct ecommunity *ecomadd);
extern void
bgp_evpn_configure_rd (struct bgp *bgp, struct bgpevpn *vpn,
                       struct prefix_rd *rd);
extern void
bgp_evpn_unconfigure_rd (struct bgp *bgp, struct bgpevpn *vpn);
extern struct bgpevpn *
bgp_evpn_create_update_vni (struct bgp *bgp, vni_t vni);
extern int
bgp_evpn_delete_vni (struct bgp *bgp, struct bgpevpn *vpn);
extern void
bgp_config_write_evpn_info (struct vty *vty, struct bgp *bgp, afi_t afi,
                            safi_t safi, int *write);
extern void
bgp_evpn_show_import_rts (struct vty *vty, struct bgp *bgp);
extern void
bgp_evpn_show_vni (struct vty *vty, struct bgp *bgp, vni_t vni);
extern void
bgp_evpn_show_all_vnis (struct vty *vty, struct bgp *bgp);
extern void
bgp_evpn_set_advertise_vni (struct bgp *bgp);
extern void
bgp_evpn_unset_advertise_vni (struct bgp *bgp);

#endif /* _QUAGGA_BGP_EVPN_H */
