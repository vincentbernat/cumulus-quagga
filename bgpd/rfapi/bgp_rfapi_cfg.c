/* 
 *
 * Copyright 2009-2016, LabN Consulting, L.L.C.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */
#include "lib/zebra.h"

#include "lib/command.h"
#include "lib/prefix.h"
#include "lib/memory.h"
#include "lib/linklist.h"
#include "lib/table.h"
#include "lib/plist.h"
#include "lib/routemap.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_mplsvpn.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_rd.h"

#include "bgpd/bgp_ecommunity.h"
#include "bgpd/rfapi/rfapi.h"
#include "bgpd/rfapi/bgp_rfapi_cfg.h"
#include "bgpd/rfapi/rfapi_backend.h"
#include "bgpd/rfapi/rfapi_import.h"
#include "bgpd/rfapi/rfapi_private.h"
#include "bgpd/rfapi/rfapi_monitor.h"
#include "bgpd/rfapi/vnc_zebra.h"
#include "bgpd/rfapi/vnc_export_bgp.h"
#include "bgpd/rfapi/vnc_export_bgp_p.h"
#include "bgpd/rfapi/rfapi_vty.h"
#include "bgpd/rfapi/vnc_import_bgp.h"

#if ENABLE_BGP_VNC

#undef BGP_VNC_DEBUG_MATCH_GROUP


DEFINE_MGROUP(RFAPI, "rfapi")
DEFINE_MTYPE(RFAPI, RFAPI_CFG,			  "NVE Configuration")
DEFINE_MTYPE(RFAPI, RFAPI_GROUP_CFG,		  "NVE Group Configuration")
DEFINE_MTYPE(RFAPI, RFAPI_L2_CFG,		  "RFAPI L2 Group Configuration")
DEFINE_MTYPE(RFAPI, RFAPI_RFP_GROUP_CFG,	  "RFAPI RFP Group Configuration")
DEFINE_MTYPE(RFAPI, RFAPI,			  "RFAPI Generic")
DEFINE_MTYPE(RFAPI, RFAPI_DESC,			  "RFAPI Descriptor")
DEFINE_MTYPE(RFAPI, RFAPI_IMPORTTABLE,		  "RFAPI Import Table")
DEFINE_MTYPE(RFAPI, RFAPI_MONITOR,		  "RFAPI Monitor VPN")
DEFINE_MTYPE(RFAPI, RFAPI_MONITOR_ENCAP,	  "RFAPI Monitor Encap")
DEFINE_MTYPE(RFAPI, RFAPI_NEXTHOP,		  "RFAPI Next Hop")
DEFINE_MTYPE(RFAPI, RFAPI_VN_OPTION,		  "RFAPI VN Option")
DEFINE_MTYPE(RFAPI, RFAPI_UN_OPTION,		  "RFAPI UN Option")
DEFINE_MTYPE(RFAPI, RFAPI_WITHDRAW,		  "RFAPI Withdraw")
DEFINE_MTYPE(RFAPI, RFAPI_RFG_NAME,		  "RFAPI RFGName")
DEFINE_MTYPE(RFAPI, RFAPI_ADB,			  "RFAPI Advertisement Data")
DEFINE_MTYPE(RFAPI, RFAPI_ETI,			  "RFAPI Export Table Info")
DEFINE_MTYPE(RFAPI, RFAPI_NVE_ADDR,		  "RFAPI NVE Address")
DEFINE_MTYPE(RFAPI, RFAPI_PREFIX_BAG,		  "RFAPI Prefix Bag")
DEFINE_MTYPE(RFAPI, RFAPI_IT_EXTRA,		  "RFAPI IT Extra")
DEFINE_MTYPE(RFAPI, RFAPI_INFO,			  "RFAPI Info")
DEFINE_MTYPE(RFAPI, RFAPI_ADDR,			  "RFAPI Addr")
DEFINE_MTYPE(RFAPI, RFAPI_UPDATED_RESPONSE_QUEUE, "RFAPI Updated Rsp Queue")
DEFINE_MTYPE(RFAPI, RFAPI_RECENT_DELETE,	  "RFAPI Recently Deleted Route")
DEFINE_MTYPE(RFAPI, RFAPI_L2ADDR_OPT,		  "RFAPI L2 Address Option")
DEFINE_MTYPE(RFAPI, RFAPI_AP,			  "RFAPI Advertised Prefix")
DEFINE_MTYPE(RFAPI, RFAPI_MONITOR_ETH,		  "RFAPI Monitor Ethernet")

DEFINE_QOBJ_TYPE(rfapi_nve_group_cfg)
DEFINE_QOBJ_TYPE(rfapi_l2_group_cfg)
/***********************************************************************
 *			RFAPI Support
 ***********************************************************************/


/* 
 * compaitibility to old quagga_time call
 * time_t value in terms of stabilised absolute time. 
 * replacement for POSIX time()
 */
time_t 
rfapi_time (time_t *t)
{
  time_t clock = bgp_clock();
  if (t)
    *t = clock;
  return clock;
}

void
nve_group_to_nve_list (
  struct rfapi_nve_group_cfg	*rfg,
  struct list			**nves,
  uint8_t			family)     /* AF_INET, AF_INET6 */
{
  struct listnode *hln;
  struct rfapi_descriptor *rfd;

  /*
   * loop over nves in this grp, add to list
   */
  for (ALL_LIST_ELEMENTS_RO (rfg->nves, hln, rfd))
    {
      if (rfd->vn_addr.addr_family == family)
        {
          if (!*nves)
            *nves = list_new ();
          listnode_add (*nves, rfd);
        }
    }
}


struct rfapi_nve_group_cfg *
bgp_rfapi_cfg_match_group (
  struct rfapi_cfg	*hc,
  struct prefix		*vn,
  struct prefix		*un)
{
  struct rfapi_nve_group_cfg *rfg_vn = NULL;
  struct rfapi_nve_group_cfg *rfg_un = NULL;

  struct route_table *rt_vn;
  struct route_table *rt_un;
  struct route_node *rn_vn;
  struct route_node *rn_un;

  struct rfapi_nve_group_cfg *rfg;
  struct listnode *node, *nnode;

  switch (vn->family)
    {
    case AF_INET:
      rt_vn = &(hc->nve_groups_vn[AFI_IP]);
      break;
    case AF_INET6:
      rt_vn = &(hc->nve_groups_vn[AFI_IP6]);
      break;
    default:
      return NULL;
    }

  switch (un->family)
    {
    case AF_INET:
      rt_un = &(hc->nve_groups_un[AFI_IP]);
      break;
    case AF_INET6:
      rt_un = &(hc->nve_groups_un[AFI_IP6]);
      break;
    default:
      return NULL;
    }

  rn_vn = route_node_match (rt_vn, vn); /* NB locks node */
  if (rn_vn)
    {
      rfg_vn = rn_vn->info;
      route_unlock_node (rn_vn);
    }

  rn_un = route_node_match (rt_un, un); /* NB locks node */
  if (rn_un)
    {
      rfg_un = rn_un->info;
      route_unlock_node (rn_un);
    }

#if BGP_VNC_DEBUG_MATCH_GROUP
  {
    char buf[BUFSIZ];

    prefix2str (vn, buf, BUFSIZ);
    zlog_debug ("%s: vn prefix: %s", __func__, buf);

    prefix2str (un, buf, BUFSIZ);
    zlog_debug ("%s: un prefix: %s", __func__, buf);

    zlog_debug ("%s: rn_vn=%p, rn_un=%p, rfg_vn=%p, rfg_un=%p",
                __func__, rn_vn, rn_un, rfg_vn, rfg_un);
  }
#endif


  if (rfg_un == rfg_vn)         /* same group */
    return rfg_un;
  if (!rfg_un)                  /* un doesn't match, return vn-matched grp */
    return rfg_vn;
  if (!rfg_vn)                  /* vn doesn't match, return un-matched grp */
    return rfg_un;

  /* 
   * Two different nve groups match: the group configured earlier wins.
   * For now, just walk the sequential list and pick the first one.
   * If this approach is too slow, then store serial numbers in the
   * nve group structures as they are defined and just compare
   * serial numbers.
   */
  for (ALL_LIST_ELEMENTS (hc->nve_groups_sequential, node, nnode, rfg))
    {
      if ((rfg == rfg_un) || (rfg == rfg_vn))
        {
          return rfg;
        }
    }
  zlog_debug ("%s: shouldn't happen, returning NULL when un and vn match",
              __func__);
  return NULL;                  /* shouldn't happen */
}

/*------------------------------------------
 * rfapi_get_rfp_start_val
 *
 * Returns value passed to rfapi on rfp_start
 *
 * input:
 *	void *		bgp structure
 *
 * returns:
 *	void *          
 *------------------------------------------*/
void *
rfapi_get_rfp_start_val (void *bgpv)
{
  struct bgp *bgp = bgpv;
  if (bgp == NULL || bgp->rfapi == NULL)
    return NULL;
  return bgp->rfapi->rfp;
}

/*------------------------------------------
 * bgp_rfapi_is_vnc_configured
 *
 * Returns if VNC (BGP VPN messaging /VPN & encap SAFIs) are configured
 *
 * input: 
 *    bgp        NULL (=use default instance)
 *
 * output:
 *
 * return value: If VNC is configured for the bgpd instance
 *	0		Success
 *	ENXIO		VNC not configured
 --------------------------------------------*/
int
bgp_rfapi_is_vnc_configured (struct bgp *bgp)
{
  if (bgp == NULL)
    bgp = bgp_get_default ();

  if (bgp && bgp->rfapi_cfg)
    {
      struct peer *peer;
      struct peer_group *group;
      struct listnode *node, *nnode;
      /* if have configured VPN neighbors, assume running VNC */
      for (ALL_LIST_ELEMENTS (bgp->group, node, nnode, group))
        {
          if (group->conf->afc[AFI_IP][SAFI_MPLS_VPN] ||
              group->conf->afc[AFI_IP6][SAFI_MPLS_VPN])
            return 0;
        }
      for (ALL_LIST_ELEMENTS (bgp->peer, node, nnode, peer))
        {
          if (peer->afc[AFI_IP][SAFI_MPLS_VPN] ||
              peer->afc[AFI_IP6][SAFI_MPLS_VPN])
            return 0;
        }
    }
  return ENXIO;
}

/***********************************************************************
 *			VNC Configuration/CLI
 ***********************************************************************/


DEFUN (vnc_advertise_un_method,
       vnc_advertise_un_method_cmd,
       "vnc advertise-un-method (encap-safi|encap-attr)",
       VNC_CONFIG_STR
       "Method of advertising UN addresses\n"
       "Via Encapsulation SAFI\n"
       "Via Tunnel Encap attribute (in VPN SAFI)\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "VNC not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }


  if (!strncmp (argv[0], "encap-safi", 7))
    {
      bgp->rfapi_cfg->flags |= BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP;
    }
  else
    {
      bgp->rfapi_cfg->flags &= ~BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP;
    }

  return CMD_SUCCESS;
}

/*-------------------------------------------------------------------------
 *			RFG defaults
 *-----------------------------------------------------------------------*/


DEFUN (vnc_defaults,
       vnc_defaults_cmd,
       "vnc defaults", VNC_CONFIG_STR "Configure default NVE group\n")
{
  vty->node = BGP_VNC_DEFAULTS_NODE;
  return CMD_SUCCESS;
}

static int
set_ecom_list (
  struct vty		*vty,
  int			argc,
  const char		**argv,
  struct ecommunity	**list)
{
  struct ecommunity *ecom = NULL;
  struct ecommunity *ecomadd;

  for (; argc; --argc, ++argv)
    {

      ecomadd = ecommunity_str2com (*argv, ECOMMUNITY_ROUTE_TARGET, 0);
      if (!ecomadd)
        {
          vty_out (vty, "Malformed community-list value%s", VTY_NEWLINE);
          if (ecom)
            ecommunity_free (&ecom);
          return CMD_WARNING;
        }

      if (ecom)
        {
          ecommunity_merge (ecom, ecomadd);
          ecommunity_free (&ecomadd);
        }
      else
        {
          ecom = ecomadd;
        }
    }

  if (*list)
    {
      ecommunity_free (&*list);
    }
  *list = ecom;

  return CMD_SUCCESS;
}

DEFUN (vnc_defaults_rt_import,
       vnc_defaults_rt_import_cmd,
       "rt import .RTLIST",
       "Specify default route targets\n"
       "Import filter\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  struct bgp *bgp = vty->index;
  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  return set_ecom_list (vty, argc, argv,
                        &bgp->rfapi_cfg->default_rt_import_list);
}

DEFUN (vnc_defaults_rt_export,
       vnc_defaults_rt_export_cmd,
       "rt export .RTLIST",
       "Configure default route targets\n"
       "Export filter\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  struct bgp *bgp = vty->index;
  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  return set_ecom_list (vty, argc, argv,
                        &bgp->rfapi_cfg->default_rt_export_list);
}

DEFUN (vnc_defaults_rt_both,
       vnc_defaults_rt_both_cmd,
       "rt both .RTLIST",
       "Configure default route targets\n"
       "Export+import filters\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  int rc;
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  rc =
    set_ecom_list (vty, argc, argv, &bgp->rfapi_cfg->default_rt_import_list);
  if (rc != CMD_SUCCESS)
    return rc;
  return set_ecom_list (vty, argc, argv,
                        &bgp->rfapi_cfg->default_rt_export_list);
}

DEFUN (vnc_defaults_rd,
       vnc_defaults_rd_cmd,
       "rd ASN:nn_or_IP-address:nn",
       "Specify default route distinguisher\n"
       "Route Distinguisher (<as-number>:<number> | <ip-address>:<number> | auto:vn:<number> )\n")
{
  int ret;
  struct prefix_rd prd;
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strncmp (argv[0], "auto:vn:", 8))
    {
      /*
       * use AF_UNIX to designate automatically-assigned RD
       * auto:vn:nn where nn is a 2-octet quantity
       */
      char *end = NULL;
      uint32_t value32 = strtoul (argv[0] + 8, &end, 10);
      uint16_t value = value32 & 0xffff;

      if (!*(argv[0] + 5) || *end)
        {
          vty_out (vty, "%% Malformed rd%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
      if (value32 > 0xffff)
        {
          vty_out (vty, "%% Malformed rd (must be less than %u%s",
                   0x0ffff, VTY_NEWLINE);
          return CMD_WARNING;
        }

      memset (&prd, 0, sizeof (prd));
      prd.family = AF_UNIX;
      prd.prefixlen = 64;
      prd.val[0] = (RD_TYPE_IP >> 8) & 0x0ff;
      prd.val[1] = RD_TYPE_IP & 0x0ff;
      prd.val[6] = (value >> 8) & 0x0ff;
      prd.val[7] = value & 0x0ff;

    }
  else
    {

      ret = str2prefix_rd (argv[0], &prd);
      if (!ret)
        {
          vty_out (vty, "%% Malformed rd%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  bgp->rfapi_cfg->default_rd = prd;
  return CMD_SUCCESS;
}

DEFUN (vnc_defaults_l2rd,
       vnc_defaults_l2rd_cmd,
       "l2rd (ID|auto:vn)",
       "Specify default Local Nve ID value to use in RD for L2 routes\n"
       "Fixed value 1-255\n"
       "use the low-order octet of the NVE's VN address\n")
{
  struct bgp *bgp = vty->index;
  uint8_t value = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "auto:vn"))
    {
      value = 0;
    }
  else
    {
      char *end = NULL;
      unsigned long value_l = strtoul (argv[0], &end, 10);

      value = value_l & 0xff;
      if (!*(argv[0]) || *end)
        {
          vty_out (vty, "%% Malformed l2 nve ID \"%s\"%s", argv[0],
                   VTY_NEWLINE);
          return CMD_WARNING;
        }
      if ((value_l < 1) || (value_l > 0xff))
        {
          vty_out (vty,
                   "%% Malformed l2 nve id (must be greater than 0 and less than %u%s",
                   0x100, VTY_NEWLINE);
          return CMD_WARNING;
        }
    }
  bgp->rfapi_cfg->flags |= BGP_VNC_CONFIG_L2RD;
  bgp->rfapi_cfg->default_l2rd = value;

  return CMD_SUCCESS;
}

DEFUN (vnc_defaults_no_l2rd,
       vnc_defaults_no_l2rd_cmd,
       "no l2rd",
       NO_STR
       "Specify default Local Nve ID value to use in RD for L2 routes\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  bgp->rfapi_cfg->default_l2rd = 0;
  bgp->rfapi_cfg->flags &= ~BGP_VNC_CONFIG_L2RD;

  return CMD_SUCCESS;
}

DEFUN (vnc_defaults_responselifetime,
       vnc_defaults_responselifetime_cmd,
       "response-lifetime (LIFETIME|infinite)",
       "Specify default response lifetime\n"
       "Response lifetime in seconds\n" "Infinite response lifetime\n")
{
  uint32_t rspint;
  struct bgp *bgp = vty->index;
  struct rfapi *h = NULL;
  struct listnode *hdnode;
  struct rfapi_descriptor *rfd;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  h = bgp->rfapi;
  if (!h)
    return CMD_WARNING;

  if (!strcmp (argv[0], "infinite"))
    {
      rspint = RFAPI_INFINITE_LIFETIME;
    }
  else
    {
      VTY_GET_INTEGER ("Response Lifetime", rspint, argv[0]);
      if (rspint > INT32_MAX)
        rspint = INT32_MAX;     /* is really an int, not an unsigned int */
    }

  bgp->rfapi_cfg->default_response_lifetime = rspint;

  for (ALL_LIST_ELEMENTS_RO (&h->descriptors, hdnode, rfd))
    if (rfd->rfg && !(rfd->rfg->flags & RFAPI_RFG_RESPONSE_LIFETIME))
      rfd->response_lifetime = rfd->rfg->response_lifetime = rspint;

  return CMD_SUCCESS;
}

static struct rfapi_nve_group_cfg *
rfapi_group_lookup_byname (struct bgp *bgp, const char *name)
{
  struct rfapi_nve_group_cfg *rfg;
  struct listnode *node, *nnode;

  for (ALL_LIST_ELEMENTS
       (bgp->rfapi_cfg->nve_groups_sequential, node, nnode, rfg))
    {
      if (!strcmp (rfg->name, name))
        return rfg;
    }
  return NULL;
}

static struct rfapi_nve_group_cfg *
rfapi_group_new ()
{
  struct rfapi_nve_group_cfg *rfg;

  rfg = XCALLOC (MTYPE_RFAPI_GROUP_CFG, sizeof (struct rfapi_nve_group_cfg));
  QOBJ_REG (rfg, rfapi_nve_group_cfg);

  return rfg;
}

static struct rfapi_l2_group_cfg *
rfapi_l2_group_lookup_byname (struct bgp *bgp, const char *name)
{
  struct rfapi_l2_group_cfg *rfg;
  struct listnode *node, *nnode;

  if (bgp->rfapi_cfg->l2_groups == NULL)        /* not the best place for this */
    bgp->rfapi_cfg->l2_groups = list_new ();

  for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->l2_groups, node, nnode, rfg))
    {
      if (!strcmp (rfg->name, name))
        return rfg;
    }
  return NULL;
}

static struct rfapi_l2_group_cfg *
rfapi_l2_group_new ()
{
  struct rfapi_l2_group_cfg *rfg;

  rfg = XCALLOC (MTYPE_RFAPI_L2_CFG, sizeof (struct rfapi_l2_group_cfg));
  QOBJ_REG (rfg, rfapi_l2_group_cfg);

  return rfg;
}

static void
rfapi_l2_group_del (struct rfapi_l2_group_cfg *rfg)
{
  QOBJ_UNREG (rfg);
  XFREE (MTYPE_RFAPI_L2_CFG, rfg);
}

static int
rfapi_str2route_type (
  const char	*l3str,
  const char	*pstr,
  afi_t		*afi,
  int		*type)
{
  if (!l3str || !pstr)
    return EINVAL;

  if (!strcmp (l3str, "ipv4"))
    {
      *afi = AFI_IP;
    }
  else
    {
      if (!strcmp (l3str, "ipv6"))
        *afi = AFI_IP6;
      else
        return ENOENT;
    }

  if (!strcmp (pstr, "connected"))
    *type = ZEBRA_ROUTE_CONNECT;
  if (!strcmp (pstr, "kernel"))
    *type = ZEBRA_ROUTE_KERNEL;
  if (!strcmp (pstr, "static"))
    *type = ZEBRA_ROUTE_STATIC;
  if (!strcmp (pstr, "bgp"))
    *type = ZEBRA_ROUTE_BGP;
  if (!strcmp (pstr, "bgp-direct"))
    *type = ZEBRA_ROUTE_BGP_DIRECT;
  if (!strcmp (pstr, "bgp-direct-to-nve-groups"))
    *type = ZEBRA_ROUTE_BGP_DIRECT_EXT;

  if (!strcmp (pstr, "rip"))
    {
      if (*afi == AFI_IP)
        *type = ZEBRA_ROUTE_RIP;
      else
        *type = ZEBRA_ROUTE_RIPNG;
    }

  if (!strcmp (pstr, "ripng"))
    {
      if (*afi == AFI_IP)
        return EAFNOSUPPORT;
      *type = ZEBRA_ROUTE_RIPNG;
    }

  if (!strcmp (pstr, "ospf"))
    {
      if (*afi == AFI_IP)
        *type = ZEBRA_ROUTE_OSPF;
      else
        *type = ZEBRA_ROUTE_OSPF6;
    }

  if (!strcmp (pstr, "ospf6"))
    {
      if (*afi == AFI_IP)
        return EAFNOSUPPORT;
      *type = ZEBRA_ROUTE_OSPF6;
    }

  return 0;
}

/*-------------------------------------------------------------------------
 *			redistribute
 *-----------------------------------------------------------------------*/

#define VNC_REDIST_ENABLE(bgp, afi, type) do {			\
    switch (type) {						\
	case ZEBRA_ROUTE_BGP_DIRECT:				\
	    vnc_import_bgp_redist_enable((bgp), (afi));		\
	    break;						\
	case ZEBRA_ROUTE_BGP_DIRECT_EXT:			\
	    vnc_import_bgp_exterior_redist_enable((bgp), (afi));\
	    break;						\
	default:						\
	    vnc_redistribute_set((bgp), (afi), (type));		\
	    break;						\
    }								\
} while (0)

#define VNC_REDIST_DISABLE(bgp, afi, type) do {			\
    switch (type) {						\
	case ZEBRA_ROUTE_BGP_DIRECT:				\
	    vnc_import_bgp_redist_disable((bgp), (afi));	\
	    break;						\
	case ZEBRA_ROUTE_BGP_DIRECT_EXT:			\
	    vnc_import_bgp_exterior_redist_disable((bgp), (afi));\
	    break;						\
	default:						\
	    vnc_redistribute_unset((bgp), (afi), (type));	\
	    break;						\
    }								\
} while (0)

static uint8_t redist_was_enabled[AFI_MAX][ZEBRA_ROUTE_MAX];

static void
vnc_redistribute_prechange (struct bgp *bgp)
{
  afi_t afi;
  int type;

  zlog_debug ("%s: entry", __func__);
  memset (redist_was_enabled, 0, sizeof (redist_was_enabled));

  /*
   * Look to see if we have any redistribution enabled. If so, flush
   * the corresponding routes and turn off redistribution temporarily.
   * We need to do it because the RD's used for the redistributed
   * routes depend on the nve group.
   */
  for (afi = AFI_IP; afi < AFI_MAX; ++afi)
    {
      for (type = 0; type < ZEBRA_ROUTE_MAX; ++type)
        {
          if (bgp->rfapi_cfg->redist[afi][type])
            {
              redist_was_enabled[afi][type] = 1;
              VNC_REDIST_DISABLE (bgp, afi, type);
            }
        }
    }
  zlog_debug ("%s: return", __func__);
}

static void
vnc_redistribute_postchange (struct bgp *bgp)
{
  afi_t afi;
  int type;

  zlog_debug ("%s: entry", __func__);
  /*
   * If we turned off redistribution above, turn it back on. Doing so
   * will tell zebra to resend the routes to us
   */
  for (afi = AFI_IP; afi < AFI_MAX; ++afi)
    {
      for (type = 0; type < ZEBRA_ROUTE_MAX; ++type)
        {
          if (redist_was_enabled[afi][type])
            {
              VNC_REDIST_ENABLE (bgp, afi, type);
            }
        }
    }
  zlog_debug ("%s: return", __func__);
}

DEFUN (vnc_redistribute_rh_roo_localadmin,
       vnc_redistribute_rh_roo_localadmin_cmd,
       "vnc redistribute resolve-nve roo-ec-local-admin <0-65535>",
       VNC_CONFIG_STR
       "Redistribute routes into VNC\n"
       "Resolve-NVE mode\n"
       "Route Origin Extended Community Local Admin Field\n" "Field value\n")
{
  struct bgp *bgp = vty->index;
  uint32_t localadmin;
  char *endptr;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "RFAPI not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  localadmin = strtoul (argv[0], &endptr, 0);
  if (!*(argv[0]) || *endptr)
    {
      vty_out (vty, "%% Malformed value%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (localadmin > 0xffff)
    {
      vty_out (vty, "%% Value out of range (0-%d)%s", 0xffff, VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (bgp->rfapi_cfg->resolve_nve_roo_local_admin == localadmin)
    return CMD_SUCCESS;

  if ((bgp->rfapi_cfg->flags & BGP_VNC_CONFIG_EXPORT_BGP_MODE_BITS) ==
      BGP_VNC_CONFIG_EXPORT_BGP_MODE_CE)
    {

      vnc_export_bgp_prechange (bgp);
    }
  vnc_redistribute_prechange (bgp);

  bgp->rfapi_cfg->resolve_nve_roo_local_admin = localadmin;

  if ((bgp->rfapi_cfg->flags & BGP_VNC_CONFIG_EXPORT_BGP_MODE_BITS) ==
      BGP_VNC_CONFIG_EXPORT_BGP_MODE_CE)
    {

      vnc_export_bgp_postchange (bgp);
    }
  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}


DEFUN (vnc_redistribute_mode,
       vnc_redistribute_mode_cmd,
       "vnc redistribute mode (nve-group|plain|resolve-nve)",
       VNC_CONFIG_STR
       "Redistribute routes into VNC\n"
       "Redistribution mode\n"
       "Based on redistribute nve-group\n"
       "Unmodified\n" "Resolve each nexthop to connected NVEs\n")
{
  struct bgp *bgp = vty->index;
  vnc_redist_mode_t newmode;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "RFAPI not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }


  switch (*argv[0])
    {
    case 'n':
      newmode = VNC_REDIST_MODE_RFG;
      break;

    case 'p':
      newmode = VNC_REDIST_MODE_PLAIN;
      break;

    case 'r':
      newmode = VNC_REDIST_MODE_RESOLVE_NVE;
      break;

    default:
      vty_out (vty, "unknown redistribute mode%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (newmode != bgp->rfapi_cfg->redist_mode)
    {
      vnc_redistribute_prechange (bgp);
      bgp->rfapi_cfg->redist_mode = newmode;
      vnc_redistribute_postchange (bgp);
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_redistribute_protocol,
       vnc_redistribute_protocol_cmd,
       "vnc redistribute (ipv4|ipv6) (bgp|bgp-direct|bgp-direct-to-nve-groups|connected|kernel|ospf|rip|static)",
       VNC_CONFIG_STR
       "Redistribute routes into VNC\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "From BGP\n"
       "From BGP without Zebra\n"
       "From BGP without Zebra, only to configured NVE groups\n"
       "Connected interfaces\n"
       "From kernel routes\n"
       "From Open Shortest Path First (OSPF)\n"
       "From Routing Information Protocol (RIP)\n" "From Static routes\n")
{
  int type = ZEBRA_ROUTE_MAX;     /* init to bogus value */
  struct bgp *bgp = vty->index;
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "RFAPI not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (rfapi_str2route_type (argv[0], argv[1], &afi, &type))
    {
      vty_out (vty, "%% Invalid route type%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (type == ZEBRA_ROUTE_BGP_DIRECT_EXT)
    {
      if (bgp->rfapi_cfg->redist_bgp_exterior_view_name)
        {
          VNC_REDIST_DISABLE (bgp, afi, type);  /* disabled view implicitly */
          free (bgp->rfapi_cfg->redist_bgp_exterior_view_name);
          bgp->rfapi_cfg->redist_bgp_exterior_view_name = NULL;
        }
      bgp->rfapi_cfg->redist_bgp_exterior_view = bgp;
    }

  VNC_REDIST_ENABLE (bgp, afi, type);

  return CMD_SUCCESS;
}

DEFUN (vnc_no_redistribute_protocol,
       vnc_no_redistribute_protocol_cmd,
       "no vnc redistribute (ipv4|ipv6) (bgp|bgp-direct|bgp-direct-to-nve-groups|connected|kernel|ospf|rip|static)",
       NO_STR
       VNC_CONFIG_STR
       "Redistribute from other protocol\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "From BGP\n"
       "From BGP without Zebra\n"
       "From BGP without Zebra, only to configured NVE groups\n"
       "Connected interfaces\n"
       "From kernel routes\n"
       "From Open Shortest Path First (OSPF)\n"
       "From Routing Information Protocol (RIP)\n" "From Static routes\n")
{
  int type;
  struct bgp *bgp = vty->index;
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "RFAPI not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (rfapi_str2route_type (argv[0], argv[1], &afi, &type))
    {
      vty_out (vty, "%% Invalid route type%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  VNC_REDIST_DISABLE (bgp, afi, type);

  if (type == ZEBRA_ROUTE_BGP_DIRECT_EXT)
    {
      if (bgp->rfapi_cfg->redist_bgp_exterior_view_name)
        {
          free (bgp->rfapi_cfg->redist_bgp_exterior_view_name);
          bgp->rfapi_cfg->redist_bgp_exterior_view_name = NULL;
        }
      bgp->rfapi_cfg->redist_bgp_exterior_view = NULL;
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_redistribute_bgp_exterior,
       vnc_redistribute_bgp_exterior_cmd,
       "vnc redistribute (ipv4|ipv6) bgp-direct-to-nve-groups view NAME",
       VNC_CONFIG_STR
       "Redistribute routes into VNC\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "From BGP without Zebra, only to configured NVE groups\n"
       "From BGP view\n" "BGP view name\n")
{
  int type;
  struct bgp *bgp = vty->index;
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "RFAPI not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (rfapi_str2route_type (argv[0], "bgp-direct-to-nve-groups", &afi, &type))
    {
      vty_out (vty, "%% Invalid route type%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (bgp->rfapi_cfg->redist_bgp_exterior_view_name)
    free (bgp->rfapi_cfg->redist_bgp_exterior_view_name);
  bgp->rfapi_cfg->redist_bgp_exterior_view_name = strdup (argv[1]);
  /* could be NULL if name is not defined yet */
  bgp->rfapi_cfg->redist_bgp_exterior_view = bgp_lookup_by_name (argv[1]);

  VNC_REDIST_ENABLE (bgp, afi, type);

  return CMD_SUCCESS;
}

DEFUN (vnc_redistribute_nvegroup,
       vnc_redistribute_nvegroup_cmd,
       "vnc redistribute nve-group NAME",
       VNC_CONFIG_STR
       "Assign a NVE group to routes redistributed from another routing protocol\n"
       "NVE group\n" "Group name\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vnc_redistribute_prechange (bgp);

  /*
   * OK if nve group doesn't exist yet; we'll set the pointer
   * when the group is defined later
   */
  bgp->rfapi_cfg->rfg_redist = rfapi_group_lookup_byname (bgp, argv[0]);
  if (bgp->rfapi_cfg->rfg_redist_name)
    free (bgp->rfapi_cfg->rfg_redist_name);
  bgp->rfapi_cfg->rfg_redist_name = strdup (argv[0]);

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_redistribute_no_nvegroup,
       vnc_redistribute_no_nvegroup_cmd,
       "no vnc redistribute nve-group",
       NO_STR
       VNC_CONFIG_STR
       "Redistribute from other protocol\n"
       "Assign a NVE group to routes redistributed from another routing protocol\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vnc_redistribute_prechange (bgp);

  bgp->rfapi_cfg->rfg_redist = NULL;
  if (bgp->rfapi_cfg->rfg_redist_name)
    free (bgp->rfapi_cfg->rfg_redist_name);
  bgp->rfapi_cfg->rfg_redist_name = NULL;

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}


DEFUN (vnc_redistribute_lifetime,
       vnc_redistribute_lifetime_cmd,
       "vnc redistribute lifetime (LIFETIME|infinite)",
       VNC_CONFIG_STR
       "Assign a lifetime to routes redistributed from another routing protocol\n"
       "lifetime value (32 bit)\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vnc_redistribute_prechange (bgp);

  if (!strcmp (argv[0], "infinite"))
    {
      bgp->rfapi_cfg->redist_lifetime = RFAPI_INFINITE_LIFETIME;
    }
  else
    {
      VTY_GET_INTEGER ("Response Lifetime", bgp->rfapi_cfg->redist_lifetime,
                       argv[0]);
    }

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

/*-- redist policy, non-nvegroup start --*/

DEFUN (vnc_redist_bgpdirect_no_prefixlist,
       vnc_redist_bgpdirect_no_prefixlist_cmd,
       "no vnc redistribute (bgp-direct|bgp-direct-to-nve-groups) (ipv4|ipv6) prefix-list",
       NO_STR
       VNC_CONFIG_STR
       "Redistribute from other protocol\n"
       "Redistribute from BGP directly\n"
       "Redistribute from BGP without Zebra, only to configured NVE groups\n"
       "IPv4 routes\n"
       "IPv6 routes\n" "Prefix-list for filtering redistributed routes\n")
{
  struct bgp *bgp = vty->index;
  afi_t afi;
  struct rfapi_cfg *hc;
  uint8_t route_type = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "bgp-direct"))
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT;
    }
  else
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT_EXT;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  vnc_redistribute_prechange (bgp);

  if (hc->plist_redist_name[route_type][afi])
    free (hc->plist_redist_name[route_type][afi]);
  hc->plist_redist_name[route_type][afi] = NULL;
  hc->plist_redist[route_type][afi] = NULL;

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_redist_bgpdirect_prefixlist,
       vnc_redist_bgpdirect_prefixlist_cmd,
       "vnc redistribute (bgp-direct|bgp-direct-to-nve-groups) (ipv4|ipv6) prefix-list NAME",
       VNC_CONFIG_STR
       "Redistribute from other protocol\n"
       "Redistribute from BGP directly\n"
       "Redistribute from BGP without Zebra, only to configured NVE groups\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "Prefix-list for filtering redistributed routes\n"
       "prefix list name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;
  afi_t afi;
  uint8_t route_type = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "bgp-direct"))
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT;
    }
  else
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT_EXT;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  vnc_redistribute_prechange (bgp);

  if (hc->plist_redist_name[route_type][afi])
    free (hc->plist_redist_name[route_type][afi]);
  hc->plist_redist_name[route_type][afi] = strdup (argv[2]);
  hc->plist_redist[route_type][afi] = prefix_list_lookup (afi, argv[2]);

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_redist_bgpdirect_no_routemap,
       vnc_redist_bgpdirect_no_routemap_cmd,
       "no vnc redistribute (bgp-direct|bgp-direct-to-nve-groups) route-map",
       NO_STR
       VNC_CONFIG_STR
       "Redistribute from other protocols\n"
       "Redistribute from BGP directly\n"
       "Redistribute from BGP without Zebra, only to configured NVE groups\n"
       "Route-map for filtering redistributed routes\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;
  uint8_t route_type = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "bgp-direct"))
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT;
    }
  else
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT_EXT;
    }

  vnc_redistribute_prechange (bgp);

  if (hc->routemap_redist_name[route_type])
    free (hc->routemap_redist_name[route_type]);
  hc->routemap_redist_name[route_type] = NULL;
  hc->routemap_redist[route_type] = NULL;

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_redist_bgpdirect_routemap,
       vnc_redist_bgpdirect_routemap_cmd,
       "vnc redistribute (bgp-direct|bgp-direct-to-nve-groups) route-map NAME",
       VNC_CONFIG_STR
       "Redistribute from other protocols\n"
       "Redistribute from BGP directly\n"
       "Redistribute from BGP without Zebra, only to configured NVE groups\n"
       "Route-map for filtering exported routes\n" "route map name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;
  uint8_t route_type = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "bgp-direct"))
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT;
    }
  else
    {
      route_type = ZEBRA_ROUTE_BGP_DIRECT_EXT;
    }

  vnc_redistribute_prechange (bgp);

  if (hc->routemap_redist_name[route_type])
    free (hc->routemap_redist_name[route_type]);
  hc->routemap_redist_name[route_type] = strdup (argv[1]);
  hc->routemap_redist[route_type] = route_map_lookup_by_name (argv[1]);

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

/*-- redist policy, non-nvegroup end --*/

/*-- redist policy, nvegroup start --*/

DEFUN (vnc_nve_group_redist_bgpdirect_no_prefixlist,
       vnc_nve_group_redist_bgpdirect_no_prefixlist_cmd,
       "no redistribute bgp-direct (ipv4|ipv6) prefix-list",
       NO_STR
       "Redistribute from other protocol\n"
       "Redistribute from BGP directly\n"
       "Disable redistribute filter\n"
       "IPv4 routes\n"
       "IPv6 routes\n" "Prefix-list for filtering redistributed routes\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg)
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  vnc_redistribute_prechange (bgp);

  if (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi])
    free (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi]);
  rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi] = NULL;
  rfg->plist_redist[ZEBRA_ROUTE_BGP_DIRECT][afi] = NULL;

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_redist_bgpdirect_prefixlist,
       vnc_nve_group_redist_bgpdirect_prefixlist_cmd,
       "redistribute bgp-direct (ipv4|ipv6) prefix-list NAME",
       "Redistribute from other protocol\n"
       "Redistribute from BGP directly\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "Prefix-list for filtering redistributed routes\n"
       "prefix list name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  vnc_redistribute_prechange (bgp);

  if (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi])
    free (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi]);
  rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi] = strdup (argv[1]);
  rfg->plist_redist[ZEBRA_ROUTE_BGP_DIRECT][afi] =
    prefix_list_lookup (afi, argv[1]);

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_redist_bgpdirect_no_routemap,
       vnc_nve_group_redist_bgpdirect_no_routemap_cmd,
       "no redistribute bgp-direct route-map",
       NO_STR
       "Redistribute from other protocols\n"
       "Redistribute from BGP directly\n"
       "Disable redistribute filter\n"
       "Route-map for filtering redistributed routes\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vnc_redistribute_prechange (bgp);

  if (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT])
    free (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT]);
  rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT] = NULL;
  rfg->routemap_redist[ZEBRA_ROUTE_BGP_DIRECT] = NULL;

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_redist_bgpdirect_routemap,
       vnc_nve_group_redist_bgpdirect_routemap_cmd,
       "redistribute bgp-direct route-map NAME",
       "Redistribute from other protocols\n"
       "Redistribute from BGP directly\n"
       "Route-map for filtering exported routes\n" "route map name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vnc_redistribute_prechange (bgp);

  if (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT])
    free (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT]);
  rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT] = strdup (argv[0]);
  rfg->routemap_redist[ZEBRA_ROUTE_BGP_DIRECT] =
    route_map_lookup_by_name (argv[0]);

  vnc_redistribute_postchange (bgp);

  return CMD_SUCCESS;
}

/*-- redist policy, nvegroup end --*/

/*-------------------------------------------------------------------------
 *			export
 *-----------------------------------------------------------------------*/

DEFUN (vnc_export_mode,
       vnc_export_mode_cmd,
       "vnc export (bgp|zebra) mode (group-nve|ce|none|registering-nve)",
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Select export mode\n"
       "Export routes with nve-group next-hops\n"
       "Export routes with NVE connected router next-hops\n"
       "Disable export\n" "Export routes with registering NVE as next-hop\n")
{
  struct bgp *bgp = vty->index;
  uint32_t oldmode = 0;
  uint32_t newmode = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "VNC not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      oldmode = bgp->rfapi_cfg->flags & BGP_VNC_CONFIG_EXPORT_BGP_MODE_BITS;
      switch (*argv[1])
        {
        case 'g':
          newmode = BGP_VNC_CONFIG_EXPORT_BGP_MODE_GRP;
          break;
        case 'c':
          newmode = BGP_VNC_CONFIG_EXPORT_BGP_MODE_CE;
          break;
        case 'n':
          newmode = 0;
          break;
        case 'r':
          newmode = BGP_VNC_CONFIG_EXPORT_BGP_MODE_RH;
          break;
        default:
          vty_out (vty, "Invalid mode specified%s", VTY_NEWLINE);
          return CMD_WARNING;
        }

      if (newmode == oldmode)
        {
          vty_out (vty, "Mode unchanged%s", VTY_NEWLINE);
          return CMD_SUCCESS;
        }

      vnc_export_bgp_prechange (bgp);

      bgp->rfapi_cfg->flags &= ~BGP_VNC_CONFIG_EXPORT_BGP_MODE_BITS;
      bgp->rfapi_cfg->flags |= newmode;

      vnc_export_bgp_postchange (bgp);


    }
  else
    {
      /*
       * export to zebra with RH mode is not yet implemented
       */
      vty_out (vty, "Changing modes for zebra export not implemented yet%s",
               VTY_NEWLINE);
      return CMD_WARNING;

      oldmode = bgp->rfapi_cfg->flags & BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_BITS;
      bgp->rfapi_cfg->flags &= ~BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_BITS;
      switch (*argv[1])
        {
        case 'g':
          if (oldmode == BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_RH)
            {
              /* TBD */
            }
          bgp->rfapi_cfg->flags |= BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_GRP;
          if (oldmode != BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_GRP)
            {
              /* TBD */
            }
          break;
        case 'n':
          if (oldmode == BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_RH)
            {
              /* TBD */
            }
          if (oldmode == BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_GRP)
            {
              /* TBD */
            }
          break;
        case 'r':
          if (oldmode == BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_GRP)
            {
              /* TBD */
            }
          bgp->rfapi_cfg->flags |= BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_RH;
          if (oldmode != BGP_VNC_CONFIG_EXPORT_ZEBRA_MODE_RH)
            {
              /* TBD */
            }
          break;
        default:
          vty_out (vty, "Invalid mode%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  return CMD_SUCCESS;
}

static struct rfapi_rfg_name *
rfgn_new ()
{
  return XCALLOC (MTYPE_RFAPI_RFG_NAME, sizeof (struct rfapi_rfg_name));
}

static void
rfgn_free (struct rfapi_rfg_name *rfgn)
{
  XFREE (MTYPE_RFAPI_RFG_NAME, rfgn);
}

DEFUN (vnc_export_nvegroup,
       vnc_export_nvegroup_cmd,
       "vnc export (bgp|zebra) group-nve group NAME",
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "NVE group, used in 'group-nve' export mode\n"
       "NVE group\n" "Group name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_nve_group_cfg *rfg_new;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rfg_new = rfapi_group_lookup_byname (bgp, argv[1]);

  if (*argv[0] == 'b')
    {

      struct listnode *node;
      struct rfapi_rfg_name *rfgn;

      /*
       * Set group for export to BGP Direct
       */

      /* see if group is already included in export list */
      for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                                 node, rfgn))
        {

          if (!strcmp (rfgn->name, argv[1]))
            {
              /* already in the list: we're done */
              return CMD_SUCCESS;
            }
        }

      rfgn = rfgn_new ();
      rfgn->name = strdup (argv[1]);
      rfgn->rfg = rfg_new;      /* OK if not set yet */

      listnode_add (bgp->rfapi_cfg->rfg_export_direct_bgp_l, rfgn);

      zlog_debug ("%s: testing rfg_new", __func__);
      if (rfg_new)
        {
          zlog_debug ("%s: testing bgp grp mode enabled", __func__);
          if (VNC_EXPORT_BGP_GRP_ENABLED (bgp->rfapi_cfg))
            zlog_debug ("%s: calling vnc_direct_bgp_add_group", __func__);
          vnc_direct_bgp_add_group (bgp, rfg_new);
        }

    }
  else
    {

      struct listnode *node;
      struct rfapi_rfg_name *rfgn;

      /*
       * Set group for export to Zebra
       */

      /* see if group is already included in export list */
      for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_zebra_l,
                                 node, rfgn))
        {

          if (!strcmp (rfgn->name, argv[1]))
            {
              /* already in the list: we're done */
              return CMD_SUCCESS;
            }
        }

      rfgn = rfgn_new ();
      rfgn->name = strdup (argv[1]);
      rfgn->rfg = rfg_new;      /* OK if not set yet */

      listnode_add (bgp->rfapi_cfg->rfg_export_zebra_l, rfgn);

      if (rfg_new)
        {
          if (VNC_EXPORT_ZEBRA_GRP_ENABLED (bgp->rfapi_cfg))
            vnc_zebra_add_group (bgp, rfg_new);
        }
    }

  return CMD_SUCCESS;
}

/*
 * This command applies to routes exported from VNC to BGP directly
 * without going though zebra
 */
DEFUN (vnc_no_export_nvegroup,
       vnc_no_export_nvegroup_cmd,
       "vnc export (bgp|zebra) group-nve no group NAME",
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "NVE group, used in 'group-nve' export mode\n"
       "Disable export of VNC routes\n" "NVE group\n" "Group name\n")
{
  struct bgp *bgp = vty->index;
  struct listnode *node, *nnode;
  struct rfapi_rfg_name *rfgn;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                              node, nnode, rfgn))
        {

          if (rfgn->name && !strcmp (rfgn->name, argv[1]))
            {
              zlog_debug ("%s: matched \"%s\"", __func__, rfgn->name);
              if (rfgn->rfg)
                vnc_direct_bgp_del_group (bgp, rfgn->rfg);
              free (rfgn->name);
              list_delete_node (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                                node);
              rfgn_free (rfgn);
              break;
            }
        }
    }
  else
    {
      for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->rfg_export_zebra_l,
                              node, nnode, rfgn))
        {

          zlog_debug ("does rfg \"%s\" match?", rfgn->name);
          if (rfgn->name && !strcmp (rfgn->name, argv[1]))
            {
              if (rfgn->rfg)
                vnc_zebra_del_group (bgp, rfgn->rfg);
              free (rfgn->name);
              list_delete_node (bgp->rfapi_cfg->rfg_export_zebra_l, node);
              rfgn_free (rfgn);
              break;
            }
        }
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_export_no_prefixlist,
       vnc_nve_group_export_no_prefixlist_cmd,
       "no export (bgp|zebra) (ipv4|ipv6) prefix-list [NAME]",
       NO_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "Prefix-list for filtering exported routes\n" "prefix list name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  if (*argv[0] == 'b')
    {
      if (((argc >= 3) && !strcmp (argv[2], rfg->plist_export_bgp_name[afi]))
          || (argc < 3))
        {

          if (rfg->plist_export_bgp_name[afi])
            free (rfg->plist_export_bgp_name[afi]);
          rfg->plist_export_bgp_name[afi] = NULL;
          rfg->plist_export_bgp[afi] = NULL;

          vnc_direct_bgp_reexport_group_afi (bgp, rfg, afi);
        }
    }
  else
    {
      if (((argc >= 3)
           && !strcmp (argv[2], rfg->plist_export_zebra_name[afi]))
          || (argc < 3))
        {
          if (rfg->plist_export_zebra_name[afi])
            free (rfg->plist_export_zebra_name[afi]);
          rfg->plist_export_zebra_name[afi] = NULL;
          rfg->plist_export_zebra[afi] = NULL;

          vnc_zebra_reexport_group_afi (bgp, rfg, afi);
        }
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_export_prefixlist,
       vnc_nve_group_export_prefixlist_cmd,
       "export (bgp|zebra) (ipv4|ipv6) prefix-list NAME",
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "IPv4 routes\n"
       "IPv6 routes\n"
       "Prefix-list for filtering exported routes\n" "prefix list name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  if (*argv[0] == 'b')
    {
      if (rfg->plist_export_bgp_name[afi])
        free (rfg->plist_export_bgp_name[afi]);
      rfg->plist_export_bgp_name[afi] = strdup (argv[2]);
      rfg->plist_export_bgp[afi] = prefix_list_lookup (afi, argv[2]);

      vnc_direct_bgp_reexport_group_afi (bgp, rfg, afi);

    }
  else
    {
      if (rfg->plist_export_zebra_name[afi])
        free (rfg->plist_export_zebra_name[afi]);
      rfg->plist_export_zebra_name[afi] = strdup (argv[2]);
      rfg->plist_export_zebra[afi] = prefix_list_lookup (afi, argv[2]);

      vnc_zebra_reexport_group_afi (bgp, rfg, afi);
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_export_no_routemap,
       vnc_nve_group_export_no_routemap_cmd,
       "no export (bgp|zebra) route-map [NAME]",
       NO_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Route-map for filtering exported routes\n" "route map name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      if (((argc >= 2) && !strcmp (argv[1], rfg->routemap_export_bgp_name)) ||
          (argc < 2))
        {

          if (rfg->routemap_export_bgp_name)
            free (rfg->routemap_export_bgp_name);
          rfg->routemap_export_bgp_name = NULL;
          rfg->routemap_export_bgp = NULL;

          vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP);
          vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP6);
        }
    }
  else
    {
      if (((argc >= 2) && !strcmp (argv[1], rfg->routemap_export_zebra_name))
          || (argc < 2))
        {
          if (rfg->routemap_export_zebra_name)
            free (rfg->routemap_export_zebra_name);
          rfg->routemap_export_zebra_name = NULL;
          rfg->routemap_export_zebra = NULL;

          vnc_zebra_reexport_group_afi (bgp, rfg, AFI_IP);
          vnc_zebra_reexport_group_afi (bgp, rfg, AFI_IP6);
        }
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_export_routemap,
       vnc_nve_group_export_routemap_cmd,
       "export (bgp|zebra) route-map NAME",
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Route-map for filtering exported routes\n" "route map name\n")
{
  struct bgp *bgp = vty->index;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!bgp->rfapi_cfg)
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      if (rfg->routemap_export_bgp_name)
        free (rfg->routemap_export_bgp_name);
      rfg->routemap_export_bgp_name = strdup (argv[1]);
      rfg->routemap_export_bgp = route_map_lookup_by_name (argv[1]);
      vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP);
      vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP6);
    }
  else
    {
      if (rfg->routemap_export_zebra_name)
        free (rfg->routemap_export_zebra_name);
      rfg->routemap_export_zebra_name = strdup (argv[1]);
      rfg->routemap_export_zebra = route_map_lookup_by_name (argv[1]);
      vnc_zebra_reexport_group_afi (bgp, rfg, AFI_IP);
      vnc_zebra_reexport_group_afi (bgp, rfg, AFI_IP6);
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_export_no_prefixlist,
       vnc_nve_export_no_prefixlist_cmd,
       "no vnc export (bgp|zebra) (ipv4|ipv6) prefix-list [NAME]",
       NO_STR
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "IPv4 prefixes\n"
       "IPv6 prefixes\n"
       "Prefix-list for filtering exported routes\n" "Prefix list name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  if (*argv[0] == 'b')
    {
      if (((argc >= 3) && !strcmp (argv[2], hc->plist_export_bgp_name[afi]))
          || (argc < 3))
        {

          if (hc->plist_export_bgp_name[afi])
            free (hc->plist_export_bgp_name[afi]);
          hc->plist_export_bgp_name[afi] = NULL;
          hc->plist_export_bgp[afi] = NULL;
          vnc_direct_bgp_reexport (bgp, afi);
        }
    }
  else
    {
      if (((argc >= 3) && !strcmp (argv[2], hc->plist_export_zebra_name[afi]))
          || (argc < 3))
        {

          if (hc->plist_export_zebra_name[afi])
            free (hc->plist_export_zebra_name[afi]);
          hc->plist_export_zebra_name[afi] = NULL;
          hc->plist_export_zebra[afi] = NULL;
          /* TBD vnc_zebra_rh_reexport(bgp, afi); */
        }
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_export_prefixlist,
       vnc_nve_export_prefixlist_cmd,
       "vnc export (bgp|zebra) (ipv4|ipv6) prefix-list NAME",
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Filters, used in 'registering-nve' export mode\n"
       "IPv4 prefixes\n"
       "IPv6 prefixes\n"
       "Prefix-list for filtering exported routes\n" "Prefix list name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;
  afi_t afi;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[1], "ipv4"))
    {
      afi = AFI_IP;
    }
  else
    {
      afi = AFI_IP6;
    }

  if (*argv[0] == 'b')
    {
      if (hc->plist_export_bgp_name[afi])
        free (hc->plist_export_bgp_name[afi]);
      hc->plist_export_bgp_name[afi] = strdup (argv[2]);
      hc->plist_export_bgp[afi] = prefix_list_lookup (afi, argv[2]);
      vnc_direct_bgp_reexport (bgp, afi);
    }
  else
    {
      if (hc->plist_export_zebra_name[afi])
        free (hc->plist_export_zebra_name[afi]);
      hc->plist_export_zebra_name[afi] = strdup (argv[2]);
      hc->plist_export_zebra[afi] = prefix_list_lookup (afi, argv[2]);
      /* TBD vnc_zebra_rh_reexport(bgp, afi); */
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_export_no_routemap,
       vnc_nve_export_no_routemap_cmd,
       "no vnc export (bgp|zebra) route-map [NAME]",
       NO_STR
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Route-map for filtering exported routes\n" "Route map name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      if (((argc >= 2) && !strcmp (argv[1], hc->routemap_export_bgp_name)) ||
          (argc < 2))
        {

          if (hc->routemap_export_bgp_name)
            free (hc->routemap_export_bgp_name);
          hc->routemap_export_bgp_name = NULL;
          hc->routemap_export_bgp = NULL;
          vnc_direct_bgp_reexport (bgp, AFI_IP);
          vnc_direct_bgp_reexport (bgp, AFI_IP6);
        }
    }
  else
    {
      if (((argc >= 2) && !strcmp (argv[1], hc->routemap_export_zebra_name))
          || (argc < 2))
        {

          if (hc->routemap_export_zebra_name)
            free (hc->routemap_export_zebra_name);
          hc->routemap_export_zebra_name = NULL;
          hc->routemap_export_zebra = NULL;
          /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP); */
          /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP6); */
        }
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_export_routemap,
       vnc_nve_export_routemap_cmd,
       "vnc export (bgp|zebra) route-map NAME",
       VNC_CONFIG_STR
       "Export to other protocols\n"
       "Export to BGP\n"
       "Export to Zebra (experimental)\n"
       "Filters, used in 'registering-nve' export mode\n"
       "Route-map for filtering exported routes\n" "Route map name\n")
{
  struct bgp *bgp = vty->index;
  struct rfapi_cfg *hc;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      vty_out (vty, "rfapi not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*argv[0] == 'b')
    {
      if (hc->routemap_export_bgp_name)
        free (hc->routemap_export_bgp_name);
      hc->routemap_export_bgp_name = strdup (argv[1]);
      hc->routemap_export_bgp = route_map_lookup_by_name (argv[1]);
      vnc_direct_bgp_reexport (bgp, AFI_IP);
      vnc_direct_bgp_reexport (bgp, AFI_IP6);
    }
  else
    {
      if (hc->routemap_export_zebra_name)
        free (hc->routemap_export_zebra_name);
      hc->routemap_export_zebra_name = strdup (argv[1]);
      hc->routemap_export_zebra = route_map_lookup_by_name (argv[1]);
      /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP); */
      /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP6); */
    }
  return CMD_SUCCESS;
}


/*
 * respond to changes in the global prefix list configuration
 */
void
vnc_prefix_list_update (struct bgp *bgp)
{
  afi_t afi;
  struct listnode *n;
  struct rfapi_nve_group_cfg *rfg;
  struct rfapi_cfg *hc;
  int i;

  if (!bgp)
    {
      zlog_debug ("%s: No BGP process is configured", __func__);
      return;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      zlog_debug ("%s: rfapi not configured", __func__);
      return;
    }

  for (afi = AFI_IP; afi < AFI_MAX; afi++)
    {
      /*
       * Loop over nve groups
       */
      for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->nve_groups_sequential,
                                 n, rfg))
        {

          if (rfg->plist_export_bgp_name[afi])
            {
              rfg->plist_export_bgp[afi] =
                prefix_list_lookup (afi, rfg->plist_export_bgp_name[afi]);
            }
          if (rfg->plist_export_zebra_name[afi])
            {
              rfg->plist_export_zebra[afi] =
                prefix_list_lookup (afi, rfg->plist_export_zebra_name[afi]);
            }
          for (i = 0; i < ZEBRA_ROUTE_MAX; ++i)
            {
              if (rfg->plist_redist_name[i][afi])
                {
                  rfg->plist_redist[i][afi] =
                    prefix_list_lookup (afi, rfg->plist_redist_name[i][afi]);
                }
            }

          vnc_direct_bgp_reexport_group_afi (bgp, rfg, afi);
          /* TBD vnc_zebra_reexport_group_afi(bgp, rfg, afi); */
        }

      /*
       * RH config, too
       */
      if (hc->plist_export_bgp_name[afi])
        {
          hc->plist_export_bgp[afi] =
            prefix_list_lookup (afi, hc->plist_export_bgp_name[afi]);
        }
      if (hc->plist_export_zebra_name[afi])
        {
          hc->plist_export_zebra[afi] =
            prefix_list_lookup (afi, hc->plist_export_zebra_name[afi]);
        }

      for (i = 0; i < ZEBRA_ROUTE_MAX; ++i)
        {
          if (hc->plist_redist_name[i][afi])
            {
              hc->plist_redist[i][afi] =
                prefix_list_lookup (afi, hc->plist_redist_name[i][afi]);
            }
        }

    }

  vnc_direct_bgp_reexport (bgp, AFI_IP);
  vnc_direct_bgp_reexport (bgp, AFI_IP6);

  /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP); */
  /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP6); */

  vnc_redistribute_prechange (bgp);
  vnc_redistribute_postchange (bgp);
}

/*
 * respond to changes in the global route map configuration
 */
void
vnc_routemap_update (struct bgp *bgp, const char *unused)
{
  struct listnode *n;
  struct rfapi_nve_group_cfg *rfg;
  struct rfapi_cfg *hc;
  int i;

  zlog_debug ("%s(arg=%s)", __func__, unused);

  if (!bgp)
    {
      zlog_debug ("%s: No BGP process is configured", __func__);
      return;
    }

  if (!(hc = bgp->rfapi_cfg))
    {
      zlog_debug ("%s: rfapi not configured", __func__);
      return;
    }

  /*
   * Loop over nve groups
   */
  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->nve_groups_sequential, n, rfg))
    {

      if (rfg->routemap_export_bgp_name)
        {
          rfg->routemap_export_bgp =
            route_map_lookup_by_name (rfg->routemap_export_bgp_name);
        }
      if (rfg->routemap_export_zebra_name)
        {
          rfg->routemap_export_bgp =
            route_map_lookup_by_name (rfg->routemap_export_zebra_name);
        }
      for (i = 0; i < ZEBRA_ROUTE_MAX; ++i)
        {
          if (rfg->routemap_redist_name[i])
            {
              rfg->routemap_redist[i] =
                route_map_lookup_by_name (rfg->routemap_redist_name[i]);
            }
        }

      vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP);
      vnc_direct_bgp_reexport_group_afi (bgp, rfg, AFI_IP6);
      /* TBD vnc_zebra_reexport_group_afi(bgp, rfg, afi); */
    }

  /*
   * RH config, too
   */
  if (hc->routemap_export_bgp_name)
    {
      hc->routemap_export_bgp =
        route_map_lookup_by_name (hc->routemap_export_bgp_name);
    }
  if (hc->routemap_export_zebra_name)
    {
      hc->routemap_export_bgp =
        route_map_lookup_by_name (hc->routemap_export_zebra_name);
    }
  for (i = 0; i < ZEBRA_ROUTE_MAX; ++i)
    {
      if (hc->routemap_redist_name[i])
        {
          hc->routemap_redist[i] =
            route_map_lookup_by_name (hc->routemap_redist_name[i]);
        }
    }

  vnc_direct_bgp_reexport (bgp, AFI_IP);
  vnc_direct_bgp_reexport (bgp, AFI_IP6);

  /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP); */
  /* TBD vnc_zebra_rh_reexport(bgp, AFI_IP6); */

  vnc_redistribute_prechange (bgp);
  vnc_redistribute_postchange (bgp);

  zlog_debug ("%s done", __func__);
}

static void
vnc_routemap_event (route_map_event_t type,     /* ignored */
                    const char *rmap_name)      /* ignored */
{
  struct listnode *mnode, *mnnode;
  struct bgp *bgp;

  zlog_debug ("%s(event type=%d)", __func__, type);
  if (bm->bgp == NULL)          /* may be called during cleanup */
    return;

  for (ALL_LIST_ELEMENTS (bm->bgp, mnode, mnnode, bgp))
    vnc_routemap_update (bgp, rmap_name);

  zlog_debug ("%s: done", __func__);
}

/*-------------------------------------------------------------------------
 *			nve-group
 *-----------------------------------------------------------------------*/


DEFUN (vnc_nve_group,
       vnc_nve_group_cmd,
       "vnc nve-group NAME",
       VNC_CONFIG_STR "Configure a NVE group\n" "Group name\n")
{
  struct rfapi_nve_group_cfg *rfg;
  struct bgp *bgp = vty->index;
  struct listnode *node, *nnode;
  struct rfapi_rfg_name *rfgn;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* Search for name */
  rfg = rfapi_group_lookup_byname (bgp, argv[0]);

  if (!rfg)
    {
      rfg = rfapi_group_new ();
      if (!rfg)
        {
          /* Error out of memory */
          vty_out (vty, "Can't allocate memory for NVE group%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
      rfg->name = strdup (argv[0]);
      /* add to tail of list */
      listnode_add (bgp->rfapi_cfg->nve_groups_sequential, rfg);

      /* Copy defaults from struct rfapi_cfg */
      rfg->rd = bgp->rfapi_cfg->default_rd;
      if (bgp->rfapi_cfg->flags & BGP_VNC_CONFIG_L2RD)
        {
          rfg->l2rd = bgp->rfapi_cfg->default_l2rd;
          rfg->flags |= RFAPI_RFG_L2RD;
        }
      rfg->rd = bgp->rfapi_cfg->default_rd;
      rfg->response_lifetime = bgp->rfapi_cfg->default_response_lifetime;

      if (bgp->rfapi_cfg->default_rt_export_list)
        {
          rfg->rt_export_list =
            ecommunity_dup (bgp->rfapi_cfg->default_rt_export_list);
        }

      if (bgp->rfapi_cfg->default_rt_import_list)
        {
          rfg->rt_import_list =
            ecommunity_dup (bgp->rfapi_cfg->default_rt_import_list);
          rfg->rfapi_import_table =
            rfapiImportTableRefAdd (bgp, rfg->rt_import_list);
        }

      /*
       * If a redist nve group was named but the group was not defined,
       * make the linkage now
       */
      if (!bgp->rfapi_cfg->rfg_redist)
        {
          if (bgp->rfapi_cfg->rfg_redist_name &&
              !strcmp (bgp->rfapi_cfg->rfg_redist_name, rfg->name))
            {

              vnc_redistribute_prechange (bgp);
              bgp->rfapi_cfg->rfg_redist = rfg;
              vnc_redistribute_postchange (bgp);

            }
        }

      /*
       * Same treatment for bgp-direct export group
       */
      for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                              node, nnode, rfgn))
        {

          if (!strcmp (rfgn->name, rfg->name))
            {
              rfgn->rfg = rfg;
              vnc_direct_bgp_add_group (bgp, rfg);
              break;
            }
        }

      /*
       * Same treatment for zebra export group
       */
      for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->rfg_export_zebra_l,
                              node, nnode, rfgn))
        {

          zlog_debug ("%s: ezport zebra: checking if \"%s\" == \"%s\"",
                      __func__, rfgn->name, rfg->name);
          if (!strcmp (rfgn->name, rfg->name))
            {
              rfgn->rfg = rfg;
              vnc_zebra_add_group (bgp, rfg);
              break;
            }
        }
    }

  /*
   * XXX subsequent calls will need to make sure this item is still
   * in the linked list and has the same name
   */
  VTY_PUSH_CONTEXT_SUB (BGP_VNC_NVE_GROUP_NODE, rfg);

  return CMD_SUCCESS;
}

static void
bgp_rfapi_delete_nve_group (
  struct vty			*vty,    /* NULL = no output */
  struct bgp			*bgp,
  struct rfapi_nve_group_cfg	*rfg)
{
  struct list *orphaned_nves = NULL;
  struct listnode *node, *nnode;

  /*
   * If there are currently-open NVEs that belong to this group,
   * zero out their references to this group structure.
   */
  if (rfg->nves)
    {
      struct rfapi_descriptor *rfd;
      orphaned_nves = list_new ();
      while ((rfd = listnode_head (rfg->nves)))
        {
          rfd->rfg = NULL;
          listnode_delete (rfg->nves, rfd);
          listnode_add (orphaned_nves, rfd);
        }
      list_delete (rfg->nves);
      rfg->nves = NULL;
    }

  /* delete it */
  free (rfg->name);
  if (rfg->rfapi_import_table)
    rfapiImportTableRefDelByIt (bgp, rfg->rfapi_import_table);
  if (rfg->rt_import_list)
    ecommunity_free (&rfg->rt_import_list);
  if (rfg->rt_export_list)
    ecommunity_free (&rfg->rt_export_list);

  if (rfg->vn_node)
    {
      rfg->vn_node->info = NULL;
      route_unlock_node (rfg->vn_node); /* frees */
    }
  if (rfg->un_node)
    {
      rfg->un_node->info = NULL;
      route_unlock_node (rfg->un_node); /* frees */
    }
  if (rfg->rfp_cfg)
    XFREE (MTYPE_RFAPI_RFP_GROUP_CFG, rfg->rfp_cfg);
  listnode_delete (bgp->rfapi_cfg->nve_groups_sequential, rfg);

  QOBJ_UNREG (rfg);
  XFREE (MTYPE_RFAPI_GROUP_CFG, rfg);

  /*
   * Attempt to reassign the orphaned nves to a new group. If
   * a NVE can not be reassigned, its rfd->rfg will remain NULL
   * and it will become a zombie until released by rfapi_close().
   */
  if (orphaned_nves)
    {
      struct rfapi_descriptor *rfd;

      for (ALL_LIST_ELEMENTS (orphaned_nves, node, nnode, rfd))
        {
          /*
           * 1. rfapi_close() equivalent except:
           *          a. don't free original descriptor
           *          b. remember query list
           *          c. remember advertised route list
           * 2. rfapi_open() equivalent except:
           *          a. reuse original descriptor
           * 3. rfapi_register() on remembered advertised route list
           * 4. rfapi_query on rememebred query list
           */

          int rc;

          rc = rfapi_reopen (rfd, bgp);

          if (!rc)
            {
              list_delete_node (orphaned_nves, node);
              if (vty)
                vty_out (vty, "WARNING: reassigned NVE vn=");
              rfapiPrintRfapiIpAddr (vty, &rfd->vn_addr);
              if (vty)
                vty_out (vty, " un=");
              rfapiPrintRfapiIpAddr (vty, &rfd->un_addr);
              if (vty)
                vty_out (vty, " to new group \"%s\"%s", rfd->rfg->name,
                         VTY_NEWLINE);

            }
        }

      for (ALL_LIST_ELEMENTS_RO (orphaned_nves, node, rfd))
        {
          if (vty)
            vty_out (vty, "WARNING: orphaned NVE vn=");
          rfapiPrintRfapiIpAddr (vty, &rfd->vn_addr);
          if (vty)
            vty_out (vty, " un=");
          rfapiPrintRfapiIpAddr (vty, &rfd->un_addr);
          if (vty)
            vty_out (vty, "%s", VTY_NEWLINE);
        }
      list_delete (orphaned_nves);
    }
}

static int
bgp_rfapi_delete_named_nve_group (
  struct vty *vty,      /* NULL = no output */
  struct bgp *bgp,
  const char *rfg_name)        /* NULL = any */
{
  struct rfapi_nve_group_cfg *rfg = NULL;
  struct listnode *node, *nnode;
  struct rfapi_rfg_name *rfgn;

  /* Search for name */
  if (rfg_name)
    {
      rfg = rfapi_group_lookup_byname (bgp, rfg_name);
      if (!rfg)
        {
          if (vty)
            vty_out (vty, "No NVE group named \"%s\"%s", rfg_name,
                     VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  /*
   * If this group is the redist nve group, unlink it
   */
  if (rfg_name == NULL || bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_prechange (bgp);
      bgp->rfapi_cfg->rfg_redist = NULL;
      vnc_redistribute_postchange (bgp);
    }


  /*
   * remove reference from bgp direct export list
   */
  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                             node, rfgn))
    {
      if (rfg_name == NULL || !strcmp (rfgn->name, rfg_name))
        {
          rfgn->rfg = NULL;
          /* remove exported routes from this group */
          vnc_direct_bgp_del_group (bgp, rfg);
          break;
        }
    }

  /*
   * remove reference from zebra export list
   */
  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_zebra_l, node, rfgn))
    {

      if (rfg_name == NULL || !strcmp (rfgn->name, rfg_name))
        {
          rfgn->rfg = NULL;
          /* remove exported routes from this group */
          vnc_zebra_del_group (bgp, rfg);
          break;
        }
    }
  if (rfg)
    bgp_rfapi_delete_nve_group (vty, bgp, rfg);
  else                          /* must be delete all */
    for (ALL_LIST_ELEMENTS
         (bgp->rfapi_cfg->nve_groups_sequential, node, nnode, rfg))
      bgp_rfapi_delete_nve_group (vty, bgp, rfg);
  return CMD_SUCCESS;
}

DEFUN (vnc_no_nve_group,
       vnc_no_nve_group_cmd,
       "no vnc nve-group NAME",
       NO_STR
       VNC_CONFIG_STR
       "Configure a NVE group\n"
       "Group name\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  return bgp_rfapi_delete_named_nve_group (vty, bgp, argv[0]);
}

DEFUN (vnc_nve_group_prefix,
       vnc_nve_group_prefix_cmd,
       "prefix (vn|un) (A.B.C.D/M|X:X::X:X/M)",
       "Specify prefixes matching NVE VN or UN interfaces\n"
       "VN prefix\n"
       "UN prefix\n"
       "IPv4 prefix\n"
       "IPv6 prefix\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct prefix p;
  int afi;
  struct route_table *rt;
  struct route_node *rn;
  int is_un_prefix = 0;

  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!str2prefix (argv[1], &p))
    {
      vty_out (vty, "Malformed prefix \"%s\"%s", argv[1], VTY_NEWLINE);
      return CMD_WARNING;
    }

  afi = family2afi (p.family);
  if (!afi)
    {
      vty_out (vty, "Unsupported address family%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (*(argv[0]) == 'u')
    {
      rt = &(bgp->rfapi_cfg->nve_groups_un[afi]);
      is_un_prefix = 1;
    }
  else
    {
      rt = &(bgp->rfapi_cfg->nve_groups_vn[afi]);
    }

  rn = route_node_get (rt, &p); /* NB locks node */
  if (rn->info)
    {
      /*
       * There is already a group with this prefix
       */
      route_unlock_node (rn);
      if (rn->info != rfg)
        {
          /*
           * different group name: fail
           */
          vty_out (vty, "nve group \"%s\" already has \"%s\" prefix %s%s",
                   ((struct rfapi_nve_group_cfg *) (rn->info))->name,
                   argv[0], argv[1], VTY_NEWLINE);
          return CMD_WARNING;
        }
      else
        {
          /*
           * same group name: it's already in the correct place
           * in the table, so we're done.
           *
           * Implies rfg->(vn|un)_prefix is already correct.
           */
          return CMD_SUCCESS;
        }
    }

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_prechange (bgp);
    }

  /* New prefix, new node */

  if (is_un_prefix)
    {

      /* detach rfg from previous route table location */
      if (rfg->un_node)
        {
          rfg->un_node->info = NULL;
          route_unlock_node (rfg->un_node);     /* frees */
        }
      rfg->un_node = rn;        /* back ref */
      rfg->un_prefix = p;

    }
  else
    {

      /* detach rfg from previous route table location */
      if (rfg->vn_node)
        {
          rfg->vn_node->info = NULL;
          route_unlock_node (rfg->vn_node);     /* frees */
        }
      rfg->vn_node = rn;        /* back ref */
      rfg->vn_prefix = p;
    }

  /* attach */
  rn->info = rfg;

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_postchange (bgp);
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_rt_import,
       vnc_nve_group_rt_import_cmd,
       "rt import .RTLIST",
       "Specify route targets\n"
       "Import filter\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  int rc;
  struct listnode *node;
  struct rfapi_rfg_name *rfgn;
  int is_export_bgp = 0;
  int is_export_zebra = 0;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rc = set_ecom_list (vty, argc, argv, &rfg->rt_import_list);
  if (rc != CMD_SUCCESS)
    return rc;

  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                             node, rfgn))
    {

      if (rfgn->rfg == rfg)
        {
          is_export_bgp = 1;
          break;
        }
    }

  if (is_export_bgp)
    vnc_direct_bgp_del_group (bgp, rfg);

  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_zebra_l, node, rfgn))
    {

      if (rfgn->rfg == rfg)
        {
          is_export_zebra = 1;
          break;
        }
    }

  if (is_export_zebra)
    vnc_zebra_del_group (bgp, rfg);

  /*
   * stop referencing old import table, now reference new one
   */
  if (rfg->rfapi_import_table)
    rfapiImportTableRefDelByIt (bgp, rfg->rfapi_import_table);
  rfg->rfapi_import_table = rfapiImportTableRefAdd (bgp, rfg->rt_import_list);

  if (is_export_bgp)
    vnc_direct_bgp_add_group (bgp, rfg);

  if (is_export_zebra)
    vnc_zebra_add_group (bgp, rfg);

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_rt_export,
       vnc_nve_group_rt_export_cmd,
       "rt export .RTLIST",
       "Specify route targets\n"
       "Export filter\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  int rc;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_prechange (bgp);
    }

  rc = set_ecom_list (vty, argc, argv, &rfg->rt_export_list);

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_postchange (bgp);
    }

  return rc;
}

DEFUN (vnc_nve_group_rt_both,
       vnc_nve_group_rt_both_cmd,
       "rt both .RTLIST",
       "Specify route targets\n"
       "Export+import filters\n"
       "Space separated route target list (A.B.C.D:MN|EF:OPQR|GHJK:MN)\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  int rc;
  int is_export_bgp = 0;
  int is_export_zebra = 0;
  struct listnode *node;
  struct rfapi_rfg_name *rfgn;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rc = set_ecom_list (vty, argc, argv, &rfg->rt_import_list);
  if (rc != CMD_SUCCESS)
    return rc;

  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_direct_bgp_l,
                             node, rfgn))
    {

      if (rfgn->rfg == rfg)
        {
          is_export_bgp = 1;
          break;
        }
    }

  if (is_export_bgp)
    vnc_direct_bgp_del_group (bgp, rfg);

  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->rfg_export_zebra_l, node, rfgn))
    {

      if (rfgn->rfg == rfg)
        {
          is_export_zebra = 1;
          break;
        }
    }

  if (is_export_zebra)
    {
      zlog_debug ("%s: is_export_zebra", __func__);
      vnc_zebra_del_group (bgp, rfg);
    }

  /*
   * stop referencing old import table, now reference new one
   */
  if (rfg->rfapi_import_table)
    rfapiImportTableRefDelByIt (bgp, rfg->rfapi_import_table);
  rfg->rfapi_import_table = rfapiImportTableRefAdd (bgp, rfg->rt_import_list);

  if (is_export_bgp)
    vnc_direct_bgp_add_group (bgp, rfg);

  if (is_export_zebra)
    vnc_zebra_add_group (bgp, rfg);

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_prechange (bgp);
    }

  rc = set_ecom_list (vty, argc, argv, &rfg->rt_export_list);

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_postchange (bgp);
    }

  return rc;

}

DEFUN (vnc_nve_group_l2rd,
       vnc_nve_group_l2rd_cmd,
       "l2rd (ID|auto:vn)",
       "Specify default Local Nve ID value to use in RD for L2 routes\n"
       "Fixed value 1-255\n"
       "use the low-order octet of the NVE's VN address\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "auto:vn"))
    {
      rfg->l2rd = 0;
    }
  else
    {
      char *end = NULL;
      unsigned long value_l = strtoul (argv[0], &end, 10);
      uint8_t value = value_l & 0xff;

      if (!*(argv[0]) || *end)
        {
          vty_out (vty, "%% Malformed l2 nve ID \"%s\"%s", argv[0],
                   VTY_NEWLINE);
          return CMD_WARNING;
        }
      if ((value_l < 1) || (value_l > 0xff))
        {
          vty_out (vty,
                   "%% Malformed l2 nve id (must be greater than 0 and less than %u%s",
                   0x100, VTY_NEWLINE);
          return CMD_WARNING;
        }

      rfg->l2rd = value;
    }
  rfg->flags |= RFAPI_RFG_L2RD;

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_no_l2rd,
       vnc_nve_group_no_l2rd_cmd,
       "no l2rd",
       NO_STR
       "Specify default Local Nve ID value to use in RD for L2 routes\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rfg->l2rd = 0;
  rfg->flags &= ~RFAPI_RFG_L2RD;

  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_rd,
       vnc_nve_group_rd_cmd,
       "rd ASN:nn_or_IP-address:nn",
       "Specify route distinguisher\n"
       "Route Distinguisher (<as-number>:<number> | <ip-address>:<number> | auto:vn:<number> )\n")
{
  int ret;
  struct prefix_rd prd;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strncmp (argv[0], "auto:vn:", 8))
    {
      /*
       * use AF_UNIX to designate automatically-assigned RD
       * auto:vn:nn where nn is a 2-octet quantity
       */
      char *end = NULL;
      uint32_t value32 = strtoul (argv[0] + 8, &end, 10);
      uint16_t value = value32 & 0xffff;

      if (!*(argv[0] + 5) || *end)
        {
          vty_out (vty, "%% Malformed rd%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
      if (value32 > 0xffff)
        {
          vty_out (vty, "%% Malformed rd (must be less than %u%s",
                   0x0ffff, VTY_NEWLINE);
          return CMD_WARNING;
        }

      memset (&prd, 0, sizeof (prd));
      prd.family = AF_UNIX;
      prd.prefixlen = 64;
      prd.val[0] = (RD_TYPE_IP >> 8) & 0x0ff;
      prd.val[1] = RD_TYPE_IP & 0x0ff;
      prd.val[6] = (value >> 8) & 0x0ff;
      prd.val[7] = value & 0x0ff;

    }
  else
    {

      ret = str2prefix_rd (argv[0], &prd);
      if (!ret)
        {
          vty_out (vty, "%% Malformed rd%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_prechange (bgp);
    }

  rfg->rd = prd;

  if (bgp->rfapi_cfg->rfg_redist == rfg)
    {
      vnc_redistribute_postchange (bgp);
    }
  return CMD_SUCCESS;
}

DEFUN (vnc_nve_group_responselifetime,
       vnc_nve_group_responselifetime_cmd,
       "response-lifetime (LIFETIME|infinite)",
       "Specify response lifetime\n"
       "Response lifetime in seconds\n" "Infinite response lifetime\n")
{
  unsigned int rspint;
  VTY_DECLVAR_CONTEXT_SUB(rfapi_nve_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  struct rfapi_descriptor *rfd;
  struct listnode *hdnode;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->nve_groups_sequential, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current NVE group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!strcmp (argv[0], "infinite"))
    {
      rspint = RFAPI_INFINITE_LIFETIME;
    }
  else
    {
      VTY_GET_INTEGER ("Response Lifetime", rspint, argv[0]);
    }

  rfg->response_lifetime = rspint;
  rfg->flags |= RFAPI_RFG_RESPONSE_LIFETIME;
  if (rfg->nves)
    for (ALL_LIST_ELEMENTS_RO (rfg->nves, hdnode, rfd))
      rfd->response_lifetime = rspint;
  return CMD_SUCCESS;
}

/*
 * Sigh. This command, like exit-address-family, is a hack to deal
 * with the lack of rigorous level control in the command handler. 
 * TBD fix command handler.
 */
DEFUN (exit_vnc,
       exit_vnc_cmd,
       "exit-vnc",
       "Exit VNC configuration mode\n")
{
  if (vty->node == BGP_VNC_DEFAULTS_NODE ||
      vty->node == BGP_VNC_NVE_GROUP_NODE ||
      vty->node == BGP_VNC_L2_GROUP_NODE)
    {

      vty->node = BGP_NODE;
    }
  return CMD_SUCCESS;
}

static struct cmd_node bgp_vnc_defaults_node = {
  BGP_VNC_DEFAULTS_NODE,
  "%s(config-router-vnc-defaults)# ",
  1
};

static struct cmd_node bgp_vnc_nve_group_node = {
  BGP_VNC_NVE_GROUP_NODE,
  "%s(config-router-vnc-nve-group)# ",
  1
};

/*-------------------------------------------------------------------------
 *			vnc-l2-group
 *-----------------------------------------------------------------------*/


DEFUN (vnc_l2_group,
       vnc_l2_group_cmd,
       "vnc l2-group NAME",
       VNC_CONFIG_STR "Configure a L2 group\n" "Group name\n")
{
  struct rfapi_l2_group_cfg *rfg;
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* Search for name */
  rfg = rfapi_l2_group_lookup_byname (bgp, argv[0]);

  if (!rfg)
    {
      rfg = rfapi_l2_group_new ();
      if (!rfg)
        {
          /* Error out of memory */
          vty_out (vty, "Can't allocate memory for L2 group%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
      rfg->name = strdup (argv[0]);
      /* add to tail of list */
      listnode_add (bgp->rfapi_cfg->l2_groups, rfg);
    }

  /*
   * XXX subsequent calls will need to make sure this item is still
   * in the linked list and has the same name
   */
  VTY_PUSH_CONTEXT_SUB (BGP_VNC_L2_GROUP_NODE, rfg);
  return CMD_SUCCESS;
}

static void
bgp_rfapi_delete_l2_group (
  struct vty			*vty,     /* NULL = no output */
  struct bgp			*bgp,
  struct rfapi_l2_group_cfg	*rfg)
{
  /* delete it */
  free (rfg->name);
  if (rfg->rt_import_list)
    ecommunity_free (&rfg->rt_import_list);
  if (rfg->rt_export_list)
    ecommunity_free (&rfg->rt_export_list);
  if (rfg->labels)
    list_delete (rfg->labels);
  if (rfg->rfp_cfg)
    XFREE (MTYPE_RFAPI_RFP_GROUP_CFG, rfg->rfp_cfg);
  listnode_delete (bgp->rfapi_cfg->l2_groups, rfg);

  rfapi_l2_group_del (rfg);
}

static int
bgp_rfapi_delete_named_l2_group (
  struct vty *vty,       /* NULL = no output */
  struct bgp *bgp,
  const char *rfg_name) /* NULL = any */
{
  struct rfapi_l2_group_cfg *rfg = NULL;
  struct listnode *node, *nnode;

  /* Search for name */
  if (rfg_name)
    {
      rfg = rfapi_l2_group_lookup_byname (bgp, rfg_name);
      if (!rfg)
        {
          if (vty)
            vty_out (vty, "No L2 group named \"%s\"%s", rfg_name,
                     VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  if (rfg)
    bgp_rfapi_delete_l2_group (vty, bgp, rfg);
  else                          /* must be delete all */
    for (ALL_LIST_ELEMENTS (bgp->rfapi_cfg->l2_groups, node, nnode, rfg))
      bgp_rfapi_delete_l2_group (vty, bgp, rfg);
  return CMD_SUCCESS;
}

DEFUN (vnc_no_l2_group,
       vnc_no_l2_group_cmd,
       "no vnc l2-group NAME",
       NO_STR
       VNC_CONFIG_STR
       "Configure a L2 group\n"
       "Group name\n")
{
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  return bgp_rfapi_delete_named_l2_group (vty, bgp, argv[0]);
}


DEFUN (vnc_l2_group_lni,
       vnc_l2_group_lni_cmd,
       "logical-network-id <0-4294967295>",
       "Specify Logical Network ID associated with group\n"
       "value\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_l2_group_cfg, rfg);
  struct bgp *bgp = vty->index;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->l2_groups, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current L2 group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  VTY_GET_INTEGER ("logical-network-id", rfg->logical_net_id, argv[0]);

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_labels,
       vnc_l2_group_labels_cmd,
       "labels .LABELLIST",
       "Specify label values associated with group\n"
       "Space separated list of label values <0-1048575>\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_l2_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  struct list *ll;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->l2_groups, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current L2 group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  ll = rfg->labels;
  if (ll == NULL)
    {
      ll = list_new ();
      rfg->labels = ll;
    }
  for (; argc; --argc, ++argv)
    {
      uint32_t label;
      VTY_GET_INTEGER_RANGE ("Label value", label, argv[0], 0, 1048575);
      if (!listnode_lookup (ll, (void *) (uintptr_t) label))
        listnode_add (ll, (void *) (uintptr_t) label);
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_no_labels,
       vnc_l2_group_no_labels_cmd,
       "no labels .LABELLIST",
       NO_STR
       "Remove label values associated with L2 group\n"
       "Specify label values associated with L2 group\n"
       "Space separated list of label values <0-1048575>\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_l2_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  struct list *ll;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->l2_groups, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current L2 group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  ll = rfg->labels;
  if (ll == NULL)
    {
      vty_out (vty, "Label no longer associated with group%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  for (; argc; --argc, ++argv)
    {
      uint32_t label;
      VTY_GET_INTEGER_RANGE ("Label value", label, argv[0], 0, 1048575);
      listnode_delete (ll, (void *) (uintptr_t) label);
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_rt,
       vnc_l2_group_rt_cmd,
       "rt (both|export|import) ASN:nn_or_IP-address:nn",
       "Specify route targets\n"
       "Export+import filters\n"
       "Export filters\n"
       "Import filters\n"
       "A route target\n")
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_l2_group_cfg, rfg);
  struct bgp *bgp = vty->index;
  int rc = CMD_SUCCESS;
  int do_import = 0;
  int do_export = 0;

  switch (argv[0][0])
    {
    case 'b':
      do_export = 1;            /* fall through */
    case 'i':
      do_import = 1;
      break;
    case 'e':
      do_export = 1;
      break;
    default:
      vty_out (vty, "Unknown option, %s%s", argv[0], VTY_NEWLINE);
      return CMD_ERR_NO_MATCH;
    }
  argc--;
  argv++;
  if (argc < 1)
    return CMD_ERR_INCOMPLETE;

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* make sure it's still in list */
  if (!listnode_lookup (bgp->rfapi_cfg->l2_groups, rfg))
    {
      /* Not in list anymore */
      vty_out (vty, "Current L2 group no longer exists%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (do_import)
    rc = set_ecom_list (vty, argc, argv, &rfg->rt_import_list);
  if (rc == CMD_SUCCESS && do_export)
    rc = set_ecom_list (vty, argc, argv, &rfg->rt_export_list);
  return rc;
}


static struct cmd_node bgp_vnc_l2_group_node = {
  BGP_VNC_L2_GROUP_NODE,
  "%s(config-router-vnc-l2-group)# ",
  1
};

static struct rfapi_l2_group_cfg *
bgp_rfapi_get_group_by_lni_label (
  struct bgp	*bgp,
  uint32_t	logical_net_id,
  uint32_t	label)
{
  struct rfapi_l2_group_cfg *rfg;
  struct listnode *node;

  if (bgp->rfapi_cfg->l2_groups == NULL)        /* not the best place for this */
    return NULL;

  label = label & 0xfffff;      /* label is 20 bits! */

  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->l2_groups, node, rfg))
    {
      if (rfg->logical_net_id == logical_net_id)
        {
          struct listnode *lnode;
          void *data;
          for (ALL_LIST_ELEMENTS_RO (rfg->labels, lnode, data))
            if (((uint32_t) ((uintptr_t) data)) == label)
              {                 /* match! */
                return rfg;
              }
        }
    }
  return NULL;
}

struct list *
bgp_rfapi_get_labellist_by_lni_label (
  struct bgp	*bgp,
  uint32_t	logical_net_id,
  uint32_t	label)
{
  struct rfapi_l2_group_cfg *rfg;
  rfg = bgp_rfapi_get_group_by_lni_label (bgp, logical_net_id, label);
  if (rfg)
    {
      return rfg->labels;
    }
  return NULL;
}

struct ecommunity *
bgp_rfapi_get_ecommunity_by_lni_label (
  struct bgp	*bgp,
  uint32_t	is_import,
  uint32_t	logical_net_id,
  uint32_t	label)
{
  struct rfapi_l2_group_cfg *rfg;
  rfg = bgp_rfapi_get_group_by_lni_label (bgp, logical_net_id, label);
  if (rfg)
    {
      if (is_import)
        return rfg->rt_import_list;
      else
        return rfg->rt_export_list;
    }
  return NULL;
}

void
bgp_rfapi_cfg_init (void)
{
  /* main bgpd code does not use this hook, but vnc does */
  route_map_event_hook (vnc_routemap_event);

  install_node (&bgp_vnc_defaults_node, NULL);
  install_node (&bgp_vnc_nve_group_node, NULL);
  install_node (&bgp_vnc_l2_group_node, NULL);
  install_default (BGP_VNC_DEFAULTS_NODE);
  install_default (BGP_VNC_NVE_GROUP_NODE);
  install_default (BGP_VNC_L2_GROUP_NODE);

  /*
   * Add commands
   */
  install_element (BGP_NODE, &vnc_defaults_cmd);
  install_element (BGP_NODE, &vnc_nve_group_cmd);
  install_element (BGP_NODE, &vnc_no_nve_group_cmd);
  install_element (BGP_NODE, &vnc_l2_group_cmd);
  install_element (BGP_NODE, &vnc_no_l2_group_cmd);
  install_element (BGP_NODE, &vnc_advertise_un_method_cmd);
  install_element (BGP_NODE, &vnc_export_mode_cmd);

  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_rt_import_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_rt_export_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_rt_both_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_rd_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_l2rd_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_no_l2rd_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &vnc_defaults_responselifetime_cmd);
  install_element (BGP_VNC_DEFAULTS_NODE, &exit_vnc_cmd);

  install_element (BGP_NODE, &vnc_redistribute_protocol_cmd);
  install_element (BGP_NODE, &vnc_no_redistribute_protocol_cmd);
  install_element (BGP_NODE, &vnc_redistribute_nvegroup_cmd);
  install_element (BGP_NODE, &vnc_redistribute_no_nvegroup_cmd);
  install_element (BGP_NODE, &vnc_redistribute_lifetime_cmd);
  install_element (BGP_NODE, &vnc_redistribute_rh_roo_localadmin_cmd);
  install_element (BGP_NODE, &vnc_redistribute_mode_cmd);
  install_element (BGP_NODE, &vnc_redistribute_bgp_exterior_cmd);

  install_element (BGP_NODE, &vnc_redist_bgpdirect_no_prefixlist_cmd);
  install_element (BGP_NODE, &vnc_redist_bgpdirect_prefixlist_cmd);
  install_element (BGP_NODE, &vnc_redist_bgpdirect_no_routemap_cmd);
  install_element (BGP_NODE, &vnc_redist_bgpdirect_routemap_cmd);

  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_redist_bgpdirect_no_prefixlist_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_redist_bgpdirect_prefixlist_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_redist_bgpdirect_no_routemap_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_redist_bgpdirect_routemap_cmd);

  install_element (BGP_NODE, &vnc_export_nvegroup_cmd);
  install_element (BGP_NODE, &vnc_no_export_nvegroup_cmd);
  install_element (BGP_NODE, &vnc_nve_export_prefixlist_cmd);
  install_element (BGP_NODE, &vnc_nve_export_routemap_cmd);
  install_element (BGP_NODE, &vnc_nve_export_no_prefixlist_cmd);
  install_element (BGP_NODE, &vnc_nve_export_no_routemap_cmd);

  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_l2rd_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_no_l2rd_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_prefix_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_rt_import_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_rt_export_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_rt_both_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &vnc_nve_group_rd_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_responselifetime_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_export_prefixlist_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_export_routemap_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_export_no_prefixlist_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE,
                   &vnc_nve_group_export_no_routemap_cmd);
  install_element (BGP_VNC_NVE_GROUP_NODE, &exit_vnc_cmd);

  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_lni_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_labels_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_no_labels_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_rt_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &exit_vnc_cmd);
}

struct rfapi_cfg *
bgp_rfapi_cfg_new (struct rfapi_rfp_cfg *cfg)
{
  struct rfapi_cfg *h;
  int afi;

  h =
    (struct rfapi_cfg *) XCALLOC (MTYPE_RFAPI_CFG, sizeof (struct rfapi_cfg));
  assert (h);

  h->nve_groups_sequential = list_new ();
  assert (h->nve_groups_sequential);

  for (afi = AFI_IP; afi < AFI_MAX; afi++)
    {
      /* ugly, to deal with addition of delegates, part of 0.99.24.1 merge */
      h->nve_groups_vn[afi].delegate = route_table_get_default_delegate ();
      h->nve_groups_un[afi].delegate = route_table_get_default_delegate ();
    }
  h->default_response_lifetime = BGP_VNC_DEFAULT_RESPONSE_LIFETIME_DEFAULT;
  h->rfg_export_direct_bgp_l = list_new ();
  h->rfg_export_zebra_l = list_new ();
  h->resolve_nve_roo_local_admin =
    BGP_VNC_CONFIG_RESOLVE_NVE_ROO_LOCAL_ADMIN_DEFAULT;

  SET_FLAG (h->flags, BGP_VNC_CONFIG_FLAGS_DEFAULT);

  if (cfg == NULL)
    {
      h->rfp_cfg.download_type = RFAPI_RFP_DOWNLOAD_PARTIAL;
      h->rfp_cfg.ftd_advertisement_interval =
        RFAPI_RFP_CFG_DEFAULT_FTD_ADVERTISEMENT_INTERVAL;
      h->rfp_cfg.holddown_factor = RFAPI_RFP_CFG_DEFAULT_HOLDDOWN_FACTOR;
      h->rfp_cfg.use_updated_response = 0;
      h->rfp_cfg.use_removes = 0;
    }
  else
    {
      h->rfp_cfg.download_type = cfg->download_type;
      h->rfp_cfg.ftd_advertisement_interval = cfg->ftd_advertisement_interval;
      h->rfp_cfg.holddown_factor = cfg->holddown_factor;
      h->rfp_cfg.use_updated_response = cfg->use_updated_response;
      h->rfp_cfg.use_removes = cfg->use_removes;
      if (cfg->use_updated_response)
        h->flags &= ~BGP_VNC_CONFIG_CALLBACK_DISABLE;
      else
        h->flags |= BGP_VNC_CONFIG_CALLBACK_DISABLE;
      if (cfg->use_removes)
        h->flags &= ~BGP_VNC_CONFIG_RESPONSE_REMOVAL_DISABLE;
      else
        h->flags |= BGP_VNC_CONFIG_RESPONSE_REMOVAL_DISABLE;
    }
  return h;
}

void
bgp_rfapi_cfg_destroy (struct bgp *bgp, struct rfapi_cfg *h)
{
  if (h == NULL)
    return;

  bgp_rfapi_delete_named_nve_group (NULL, bgp, NULL);
  bgp_rfapi_delete_named_l2_group (NULL, bgp, NULL);
  if (h->l2_groups != NULL)
    list_delete (h->l2_groups);
  list_delete (h->nve_groups_sequential);
  list_delete (h->rfg_export_direct_bgp_l);
  list_delete (h->rfg_export_zebra_l);
  if (h->default_rt_export_list)
    ecommunity_free (&h->default_rt_export_list);
  if (h->default_rt_import_list)
    ecommunity_free (&h->default_rt_import_list);
  if (h->default_rfp_cfg)
    XFREE (MTYPE_RFAPI_RFP_GROUP_CFG, h->default_rfp_cfg);
  XFREE (MTYPE_RFAPI_CFG, h);

}

int
bgp_rfapi_cfg_write (struct vty *vty, struct bgp *bgp)
{
  struct listnode *node, *nnode;
  struct rfapi_nve_group_cfg *rfg;
  struct rfapi_cfg *hc = bgp->rfapi_cfg;
  struct rfapi_rfg_name *rfgn;
  int write = 0;
  afi_t afi;
  int type;

  if (hc->flags & BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP)
    {
      vty_out (vty, " vnc advertise-un-method encap-safi%s", VTY_NEWLINE);
      write++;
    }

  {                             /* was based on listen ports */
    /* for now allow both old and new */
    if (bgp->rfapi->rfp_methods.cfg_cb)
      write += (bgp->rfapi->rfp_methods.cfg_cb) (vty, bgp->rfapi->rfp);

    if (write)
      vty_out (vty, "!%s", VTY_NEWLINE);

    if (hc->l2_groups)
      {
        struct rfapi_l2_group_cfg *rfg = NULL;
        struct listnode *gnode;
        for (ALL_LIST_ELEMENTS_RO (hc->l2_groups, gnode, rfg))
          {
            struct listnode *lnode;
            void *data;
            ++write;
            vty_out (vty, " vnc l2-group %s%s", rfg->name, VTY_NEWLINE);
            if (rfg->logical_net_id != 0)
              vty_out (vty, "   logical-network-id %u%s", rfg->logical_net_id,
                       VTY_NEWLINE);
            if (rfg->labels != NULL && listhead (rfg->labels) != NULL)
              {
                vty_out (vty, "   labels ");
                for (ALL_LIST_ELEMENTS_RO (rfg->labels, lnode, data))
                  {
                    vty_out (vty, "%hu ", (uint16_t) ((uintptr_t) data));
                  }
                vty_out (vty, "%s", VTY_NEWLINE);
              }

            if (rfg->rt_import_list && rfg->rt_export_list &&
                ecommunity_cmp (rfg->rt_import_list, rfg->rt_export_list))
              {
                char *b = ecommunity_ecom2str (rfg->rt_import_list,
                                               ECOMMUNITY_FORMAT_ROUTE_MAP);
                vty_out (vty, "   rt both %s%s", b, VTY_NEWLINE);
                XFREE (MTYPE_ECOMMUNITY_STR, b);
              }
            else
              {
                if (rfg->rt_import_list)
                  {
                    char *b = ecommunity_ecom2str (rfg->rt_import_list,
                                                   ECOMMUNITY_FORMAT_ROUTE_MAP);
                    vty_out (vty, "  rt import %s%s", b, VTY_NEWLINE);
                    XFREE (MTYPE_ECOMMUNITY_STR, b);
                  }
                if (rfg->rt_export_list)
                  {
                    char *b = ecommunity_ecom2str (rfg->rt_export_list,
                                                   ECOMMUNITY_FORMAT_ROUTE_MAP);
                    vty_out (vty, "  rt export %s%s", b, VTY_NEWLINE);
                    XFREE (MTYPE_ECOMMUNITY_STR, b);
                  }
              }
            if (bgp->rfapi->rfp_methods.cfg_group_cb)
              write +=
                (bgp->rfapi->rfp_methods.cfg_group_cb) (vty,
                                                        bgp->rfapi->rfp,
                                                        RFAPI_RFP_CFG_GROUP_L2,
                                                        rfg->name,
                                                        rfg->rfp_cfg);
            vty_out (vty, "   exit-vnc%s", VTY_NEWLINE);
            vty_out (vty, "!%s", VTY_NEWLINE);
          }
      }

    if (hc->default_rd.family ||
        hc->default_response_lifetime ||
        hc->default_rt_import_list ||
        hc->default_rt_export_list || hc->nve_groups_sequential->count)
      {


        ++write;
        vty_out (vty, " vnc defaults%s", VTY_NEWLINE);

        if (hc->default_rd.prefixlen)
          {
            char buf[BUFSIZ];
            buf[0] = buf[BUFSIZ - 1] = 0;

            if (AF_UNIX == hc->default_rd.family)
              {
                uint16_t value = 0;

                value = ((hc->default_rd.val[6] << 8) & 0x0ff00) |
                  (hc->default_rd.val[7] & 0x0ff);

                vty_out (vty, "  rd auto:vn:%d%s", value, VTY_NEWLINE);

              }
            else
              {

                if (!prefix_rd2str (&hc->default_rd, buf, BUFSIZ) ||
                    !buf[0] || buf[BUFSIZ - 1])
                  {

                    vty_out (vty, "!Error: Can't convert rd%s", VTY_NEWLINE);
                  }
                else
                  {
                    vty_out (vty, "  rd %s%s", buf, VTY_NEWLINE);
                  }
              }
          }
        if (hc->default_response_lifetime)
          {
            vty_out (vty, "  response-lifetime ");
            if (hc->default_response_lifetime != UINT32_MAX)
              vty_out (vty, "%d", hc->default_response_lifetime);
            else
              vty_out (vty, "infinite");
            vty_out (vty, "%s", VTY_NEWLINE);
          }
        if (hc->default_rt_import_list && hc->default_rt_export_list &&
            ecommunity_cmp (hc->default_rt_import_list,
                            hc->default_rt_export_list))
          {
            char *b = ecommunity_ecom2str (hc->default_rt_import_list,
                                           ECOMMUNITY_FORMAT_ROUTE_MAP);
            vty_out (vty, "  rt both %s%s", b, VTY_NEWLINE);
            XFREE (MTYPE_ECOMMUNITY_STR, b);
          }
        else
          {
            if (hc->default_rt_import_list)
              {
                char *b = ecommunity_ecom2str (hc->default_rt_import_list,
                                               ECOMMUNITY_FORMAT_ROUTE_MAP);
                vty_out (vty, "  rt import %s%s", b, VTY_NEWLINE);
                XFREE (MTYPE_ECOMMUNITY_STR, b);
              }
            if (hc->default_rt_export_list)
              {
                char *b = ecommunity_ecom2str (hc->default_rt_export_list,
                                               ECOMMUNITY_FORMAT_ROUTE_MAP);
                vty_out (vty, "  rt export %s%s", b, VTY_NEWLINE);
                XFREE (MTYPE_ECOMMUNITY_STR, b);
              }
          }
        if (bgp->rfapi->rfp_methods.cfg_group_cb)
          write +=
            (bgp->rfapi->rfp_methods.cfg_group_cb) (vty,
                                                    bgp->rfapi->rfp,
                                                    RFAPI_RFP_CFG_GROUP_DEFAULT,
                                                    NULL,
                                                    bgp->rfapi_cfg->default_rfp_cfg);
        vty_out (vty, "  exit-vnc%s", VTY_NEWLINE);
        vty_out (vty, "!%s", VTY_NEWLINE);
      }

    for (ALL_LIST_ELEMENTS (hc->nve_groups_sequential, node, nnode, rfg))
      {
        ++write;
        vty_out (vty, " vnc nve-group %s%s", rfg->name, VTY_NEWLINE);

        if (rfg->vn_prefix.family && rfg->vn_node)
          {
            char buf[BUFSIZ];
            buf[0] = buf[BUFSIZ - 1] = 0;

            prefix2str (&rfg->vn_prefix, buf, BUFSIZ);
            if (!buf[0] || buf[BUFSIZ - 1])
              {
                vty_out (vty, "!Error: Can't convert prefix%s", VTY_NEWLINE);
              }
            else
              {
                vty_out (vty, "  prefix %s %s%s", "vn", buf, VTY_NEWLINE);
              }
          }

        if (rfg->un_prefix.family && rfg->un_node)
          {
            char buf[BUFSIZ];
            buf[0] = buf[BUFSIZ - 1] = 0;
            prefix2str (&rfg->un_prefix, buf, BUFSIZ);
            if (!buf[0] || buf[BUFSIZ - 1])
              {
                vty_out (vty, "!Error: Can't convert prefix%s", VTY_NEWLINE);
              }
            else
              {
                vty_out (vty, "  prefix %s %s%s", "un", buf, VTY_NEWLINE);
              }
          }


        if (rfg->rd.prefixlen)
          {
            char buf[BUFSIZ];
            buf[0] = buf[BUFSIZ - 1] = 0;

            if (AF_UNIX == rfg->rd.family)
              {

                uint16_t value = 0;

                value = ((rfg->rd.val[6] << 8) & 0x0ff00) |
                  (rfg->rd.val[7] & 0x0ff);

                vty_out (vty, "  rd auto:vn:%d%s", value, VTY_NEWLINE);

              }
            else
              {

                if (!prefix_rd2str (&rfg->rd, buf, BUFSIZ) ||
                    !buf[0] || buf[BUFSIZ - 1])
                  {

                    vty_out (vty, "!Error: Can't convert rd%s", VTY_NEWLINE);
                  }
                else
                  {
                    vty_out (vty, "  rd %s%s", buf, VTY_NEWLINE);
                  }
              }
          }
        if (rfg->flags & RFAPI_RFG_RESPONSE_LIFETIME)
          {
            vty_out (vty, "  response-lifetime ");
            if (rfg->response_lifetime != UINT32_MAX)
              vty_out (vty, "%d", rfg->response_lifetime);
            else
              vty_out (vty, "infinite");
            vty_out (vty, "%s", VTY_NEWLINE);
          }

        if (rfg->rt_import_list && rfg->rt_export_list &&
            ecommunity_cmp (rfg->rt_import_list, rfg->rt_export_list))
          {
            char *b = ecommunity_ecom2str (rfg->rt_import_list,
                                           ECOMMUNITY_FORMAT_ROUTE_MAP);
            vty_out (vty, "  rt both %s%s", b, VTY_NEWLINE);
            XFREE (MTYPE_ECOMMUNITY_STR, b);
          }
        else
          {
            if (rfg->rt_import_list)
              {
                char *b = ecommunity_ecom2str (rfg->rt_import_list,
                                               ECOMMUNITY_FORMAT_ROUTE_MAP);
                vty_out (vty, "  rt import %s%s", b, VTY_NEWLINE);
                XFREE (MTYPE_ECOMMUNITY_STR, b);
              }
            if (rfg->rt_export_list)
              {
                char *b = ecommunity_ecom2str (rfg->rt_export_list,
                                               ECOMMUNITY_FORMAT_ROUTE_MAP);
                vty_out (vty, "  rt export %s%s", b, VTY_NEWLINE);
                XFREE (MTYPE_ECOMMUNITY_STR, b);
              }
          }

        /*
         * route filtering: prefix-lists and route-maps
         */
        for (afi = AFI_IP; afi < AFI_MAX; ++afi)
          {

            const char *afistr = (afi == AFI_IP) ? "ipv4" : "ipv6";

            if (rfg->plist_export_bgp_name[afi])
              {
                vty_out (vty, "  export bgp %s prefix-list %s%s",
                         afistr, rfg->plist_export_bgp_name[afi],
                         VTY_NEWLINE);
              }
            if (rfg->plist_export_zebra_name[afi])
              {
                vty_out (vty, "  export zebra %s prefix-list %s%s",
                         afistr, rfg->plist_export_zebra_name[afi],
                         VTY_NEWLINE);
              }
            /*
             * currently we only support redist plists for bgp-direct.
             * If we later add plist support for redistributing other
             * protocols, we'll need to loop over protocols here
             */
            if (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi])
              {
                vty_out (vty, "  redistribute bgp-direct %s prefix-list %s%s",
                         afistr,
                         rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi],
                         VTY_NEWLINE);
              }
            if (rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT_EXT][afi])
              {
                vty_out (vty,
                         "  redistribute bgp-direct-to-nve-groups %s prefix-list %s%s",
                         afistr,
                         rfg->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT_EXT]
                         [afi], VTY_NEWLINE);
              }
          }

        if (rfg->routemap_export_bgp_name)
          {
            vty_out (vty, "  export bgp route-map %s%s",
                     rfg->routemap_export_bgp_name, VTY_NEWLINE);
          }
        if (rfg->routemap_export_zebra_name)
          {
            vty_out (vty, "  export zebra route-map %s%s",
                     rfg->routemap_export_zebra_name, VTY_NEWLINE);
          }
        if (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT])
          {
            vty_out (vty, "  redistribute bgp-direct route-map %s%s",
                     rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT],
                     VTY_NEWLINE);
          }
        if (rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT_EXT])
          {
            vty_out (vty,
                     "  redistribute bgp-direct-to-nve-groups route-map %s%s",
                     rfg->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT_EXT],
                     VTY_NEWLINE);
          }
        if (bgp->rfapi->rfp_methods.cfg_group_cb)
          write +=
            (bgp->rfapi->rfp_methods.cfg_group_cb) (vty,
                                                    bgp->rfapi->rfp,
                                                    RFAPI_RFP_CFG_GROUP_NVE,
                                                    rfg->name, rfg->rfp_cfg);
        vty_out (vty, "  exit-vnc%s", VTY_NEWLINE);
        vty_out (vty, "!%s", VTY_NEWLINE);
      }
  }                             /* have listen ports */

  /*
   * route export to other protocols
   */
  if (VNC_EXPORT_BGP_GRP_ENABLED (hc))
    {
      vty_out (vty, " vnc export bgp mode group-nve%s", VTY_NEWLINE);
    }
  else if (VNC_EXPORT_BGP_RH_ENABLED (hc))
    {
      vty_out (vty, " vnc export bgp mode registering-nve%s", VTY_NEWLINE);
    }
  else if (VNC_EXPORT_BGP_CE_ENABLED (hc))
    {
      vty_out (vty, " vnc export bgp mode ce%s", VTY_NEWLINE);
    }

  if (VNC_EXPORT_ZEBRA_GRP_ENABLED (hc))
    {
      vty_out (vty, " vnc export zebra mode group-nve%s", VTY_NEWLINE);
    }
  else if (VNC_EXPORT_ZEBRA_RH_ENABLED (hc))
    {
      vty_out (vty, " vnc export zebra mode registering-nve%s", VTY_NEWLINE);
    }

  if (hc->rfg_export_direct_bgp_l)
    {
      for (ALL_LIST_ELEMENTS (hc->rfg_export_direct_bgp_l, node, nnode, rfgn))
        {

          vty_out (vty, " vnc export bgp group-nve group %s%s",
                   rfgn->name, VTY_NEWLINE);
        }
    }

  if (hc->rfg_export_zebra_l)
    {
      for (ALL_LIST_ELEMENTS (hc->rfg_export_zebra_l, node, nnode, rfgn))
        {

          vty_out (vty, " vnc export zebra group-nve group %s%s",
                   rfgn->name, VTY_NEWLINE);
        }
    }


  if (hc->rfg_redist_name)
    {
      vty_out (vty, " vnc redistribute nve-group %s%s",
               hc->rfg_redist_name, VTY_NEWLINE);
    }
  if (hc->redist_lifetime)
    {
      vty_out (vty, " vnc redistribute lifetime %d%s",
               hc->redist_lifetime, VTY_NEWLINE);
    }
  if (hc->resolve_nve_roo_local_admin !=
      BGP_VNC_CONFIG_RESOLVE_NVE_ROO_LOCAL_ADMIN_DEFAULT)
    {

      vty_out (vty, " vnc redistribute resolve-nve roo-ec-local-admin %d%s",
               hc->resolve_nve_roo_local_admin, VTY_NEWLINE);
    }

  if (hc->redist_mode)          /* ! default */
    {
      const char *s = "";

      switch (hc->redist_mode)
        {
        case VNC_REDIST_MODE_PLAIN:
          s = "plain";
          break;
        case VNC_REDIST_MODE_RFG:
          s = "nve-group";
          break;
        case VNC_REDIST_MODE_RESOLVE_NVE:
          s = "resolve-nve";
          break;
        }
      if (s)
        {
          vty_out (vty, " vnc redistribute mode %s%s", s, VTY_NEWLINE);
        }
    }

  /*
   * route filtering: prefix-lists and route-maps
   */
  for (afi = AFI_IP; afi < AFI_MAX; ++afi)
    {

      const char *afistr = (afi == AFI_IP) ? "ipv4" : "ipv6";

      if (hc->plist_export_bgp_name[afi])
        {
          vty_out (vty, " vnc export bgp %s prefix-list %s%s",
                   afistr, hc->plist_export_bgp_name[afi], VTY_NEWLINE);
        }
      if (hc->plist_export_zebra_name[afi])
        {
          vty_out (vty, " vnc export zebra %s prefix-list %s%s",
                   afistr, hc->plist_export_zebra_name[afi], VTY_NEWLINE);
        }
      if (hc->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi])
        {
          vty_out (vty, " vnc redistribute bgp-direct %s prefix-list %s%s",
                   afistr, hc->plist_redist_name[ZEBRA_ROUTE_BGP_DIRECT][afi],
                   VTY_NEWLINE);
        }
    }

  if (hc->routemap_export_bgp_name)
    {
      vty_out (vty, " vnc export bgp route-map %s%s",
               hc->routemap_export_bgp_name, VTY_NEWLINE);
    }
  if (hc->routemap_export_zebra_name)
    {
      vty_out (vty, " vnc export zebra route-map %s%s",
               hc->routemap_export_zebra_name, VTY_NEWLINE);
    }
  if (hc->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT])
    {
      vty_out (vty, " vnc redistribute bgp-direct route-map %s%s",
               hc->routemap_redist_name[ZEBRA_ROUTE_BGP_DIRECT], VTY_NEWLINE);
    }

  for (afi = AFI_IP; afi < AFI_MAX; ++afi)
    {
      for (type = 0; type < ZEBRA_ROUTE_MAX; ++type)
        {
          if (hc->redist[afi][type])
            {
              if (type == ZEBRA_ROUTE_BGP_DIRECT_EXT &&
                  hc->redist_bgp_exterior_view_name)
                {
                  vty_out (vty, " vnc redistribute %s %s view %s%s",
                           ((afi == AFI_IP) ? "ipv4" : "ipv6"),
                           zebra_route_string (type),
                           hc->redist_bgp_exterior_view_name, VTY_NEWLINE);
                }
              else
                {
                  vty_out (vty, " vnc redistribute %s %s%s",
                           ((afi == AFI_IP) ? "ipv4" : "ipv6"),
                           zebra_route_string (type), VTY_NEWLINE);
                }
            }
        }
    }
  return write;
}

void
bgp_rfapi_show_summary (struct bgp *bgp, struct vty *vty)
{
  struct rfapi_cfg *hc = bgp->rfapi_cfg;
  int afi, type, redist = 0;
  char tmp[40];
  if (hc == NULL)
    return;

  vty_out (vty, "%-39s %-19s %s%s", "VNC Advertise method:",
           (hc->flags & BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP
            ? "Encapsulation SAFI" : "Tunnel Encap attribute"),
           ((hc->flags & BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP) ==
            (BGP_VNC_CONFIG_ADV_UN_METHOD_ENCAP &
             BGP_VNC_CONFIG_FLAGS_DEFAULT) ? "(default)" : ""), VTY_NEWLINE);
  /* export */
  vty_out (vty, "%-39s ", "Export from VNC:");
  /*
   * route export to other protocols
   */
  if (VNC_EXPORT_BGP_GRP_ENABLED (hc))
    {
      redist++;
      vty_out (vty, "ToBGP Groups={");
      if (hc->rfg_export_direct_bgp_l)
        {
          int cnt = 0;
          struct listnode *node, *nnode;
          struct rfapi_rfg_name *rfgn;
          for (ALL_LIST_ELEMENTS (hc->rfg_export_direct_bgp_l,
                                  node, nnode, rfgn))
            {
              if (cnt++ != 0)
                vty_out (vty, ",");

              vty_out (vty, "%s", rfgn->name);
            }
        }
      vty_out (vty, "}");
    }
  else if (VNC_EXPORT_BGP_RH_ENABLED (hc))
    {
      redist++;
      vty_out (vty, "ToBGP {Registering NVE}");
      /* note filters, route-maps not shown */
    }
  else if (VNC_EXPORT_BGP_CE_ENABLED (hc))
    {
      redist++;
      vty_out (vty, "ToBGP {NVE connected router:%d}",
               hc->resolve_nve_roo_local_admin);
      /* note filters, route-maps not shown */
    }

  if (VNC_EXPORT_ZEBRA_GRP_ENABLED (hc))
    {
      redist++;
      vty_out (vty, "%sToZebra Groups={", (redist == 1 ? "" : " "));
      if (hc->rfg_export_direct_bgp_l)
        {
          int cnt = 0;
          struct listnode *node, *nnode;
          struct rfapi_rfg_name *rfgn;
          for (ALL_LIST_ELEMENTS (hc->rfg_export_zebra_l, node, nnode, rfgn))
            {
              if (cnt++ != 0)
                vty_out (vty, ",");
              vty_out (vty, "%s", rfgn->name);
            }
        }
      vty_out (vty, "}");
    }
  else if (VNC_EXPORT_ZEBRA_RH_ENABLED (hc))
    {
      redist++;
      vty_out (vty, "%sToZebra {Registering NVE}", (redist == 1 ? "" : " "));
      /* note filters, route-maps not shown */
    }
  vty_out (vty, "%-19s %s%s", (redist ? "" : "Off"),
           (redist ? "" : "(default)"), VTY_NEWLINE);

  /* Redistribution */
  redist = 0;
  vty_out (vty, "%-39s ", "Redistribution into VNC:");
  for (afi = AFI_IP; afi < AFI_MAX; ++afi)
    {
      for (type = 0; type < ZEBRA_ROUTE_MAX; ++type)
        {
          if (hc->redist[afi][type])
            {
              vty_out (vty, "{%s,%s} ",
                       ((afi == AFI_IP) ? "ipv4" : "ipv6"),
                       zebra_route_string (type));
              redist++;
            }
        }
    }
  vty_out (vty, "%-19s %s%s", (redist ? "" : "Off"),
           (redist ? "" : "(default)"), VTY_NEWLINE);

  vty_out (vty, "%-39s %3u%-16s %s%s", "RFP Registration Hold-Down Factor:",
           hc->rfp_cfg.holddown_factor, "%",
           (hc->rfp_cfg.holddown_factor ==
            RFAPI_RFP_CFG_DEFAULT_HOLDDOWN_FACTOR ? "(default)" : ""),
           VTY_NEWLINE);
  vty_out (vty, "%-39s %-19s %s%s", "RFP Updated responses:",
           (hc->rfp_cfg.use_updated_response == 0 ? "Off" : "On"),
           (hc->rfp_cfg.use_updated_response == 0 ? "(default)" : ""),
           VTY_NEWLINE);
  vty_out (vty, "%-39s %-19s %s%s", "RFP Removal responses:",
           (hc->rfp_cfg.use_removes == 0 ? "Off" : "On"),
           (hc->rfp_cfg.use_removes == 0 ? "(default)" : ""), VTY_NEWLINE);
  vty_out (vty, "%-39s %-19s %s%s", "RFP Full table download:",
           (hc->rfp_cfg.download_type ==
            RFAPI_RFP_DOWNLOAD_FULL ? "On" : "Off"),
           (hc->rfp_cfg.download_type ==
            RFAPI_RFP_DOWNLOAD_PARTIAL ? "(default)" : ""), VTY_NEWLINE);
  sprintf (tmp, "%u seconds", hc->rfp_cfg.ftd_advertisement_interval);
  vty_out (vty, "%-39s %-19s %s%s", "    Advertisement Interval:", tmp,
           (hc->rfp_cfg.ftd_advertisement_interval ==
            RFAPI_RFP_CFG_DEFAULT_FTD_ADVERTISEMENT_INTERVAL
            ? "(default)" : ""), VTY_NEWLINE);
  vty_out (vty, "%-39s %d seconds%s", "Default RFP response lifetime:",
           hc->default_response_lifetime, VTY_NEWLINE);
  vty_out (vty, "%s", VTY_NEWLINE);
  return;
}

struct rfapi_cfg *
bgp_rfapi_get_config (struct bgp *bgp)
{
  struct rfapi_cfg *hc = NULL;
  if (bgp == NULL)
    bgp = bgp_get_default ();
  if (bgp != NULL)
    hc = bgp->rfapi_cfg;
  return hc;
}

#endif /* ENABLE_BGP_VNC */