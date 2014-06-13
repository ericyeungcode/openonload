/*
** Copyright 2005-2013  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/* README!
 *
 * Please do not add any new '#include's here without first talking to
 * David Riddoch.  I want to limit and document dependencies of this module
 * on other parts of Onload.  
 */
#include "oof_impl.h"
#include <ci/net/ipv4.h>
#include <onload/oof_interface.h>
#include <onload/oof_socket.h>
#include <onload/debug.h>
#include "oo_hw_filter.h"
#include "tcp_filters_internal.h"


/* If the number of sockets sharing a wild-match filter exceeds this value,
 * then the wild-match filter will be kept even after the socket that
 * created the filter is closed.
 *
 * e.g. Create a listening socket, accept 200 connections, close listening
 * socket.  The accepted sockets will continue to share a single wild-match
 * filter until the number of them drops below [oof_shared_keep_thresh], at
 * which point they will each get their own full-match filter and the wild
 * filter will be freed.
 */
int oof_shared_keep_thresh = 100;

/* If the number of sockets sharing a wild-match filter exceeds this value,
 * then the wild-match filter will be kept even when a new wild-match
 * socket needs the filter to point to a different stack.
 *
 * e.g. Create a listening socket, accept 300 connections, close listening
 * socket, create a new listening socket in a separate app.  The accepted
 * sockets will continue to share use the wild-match filter until the
 * number of them drops below [oof_shared_steal_thresh], at which point
 * they will each get their own full-match filter and the wild filter will
 * be pointed at the new wild socket.
 */
int oof_shared_steal_thresh = 200;


static struct oof_manager* the_manager;

/* This mask tracks which hwports are up.  Unicast filters are usually
 * installed on all interfaces that are up and mapped into the
 * corresponding stack and not unavailable (see below).
 */
static unsigned oof_hwports_up;

/* This mask tracks which hwports are unavailable because they are members
 * of an unacceleratable bond.  ie. Filters should not be used with
 * unavailable hwports because traffic arriving on them goes via the kernel
 * stack.
 */
static unsigned oof_hwports_available = (unsigned) -1;


#define IPF_LOG(...)  OO_DEBUG_IPF(ci_log(__VA_ARGS__))


#define SK_FMT             "%d:%d"
#define SK_PRI_ARGS(skf)   oof_cb_stack_id(oof_cb_socket_stack(skf)),   \
                           oof_cb_socket_id(skf)

#define FSK_FMT            "%s: "SK_FMT" "
#define FSK_PRI_ARGS(skf)  __FUNCTION__, SK_PRI_ARGS(skf)

#define TRIPLE_FMT         "%s "IPPORT_FMT
#define TRIPLE_ARGS(proto, ip, port)                    \
    FMT_PROTOCOL(proto), IPPORT_ARG((ip), (port))

#define QUIN_FMT           "%s "IPPORT_FMT" "IPPORT_FMT
#define QUIN_ARGS(proto, ip1, port1, ip2, port2)                        \
    FMT_PROTOCOL(proto), IPPORT_ARG((ip1), (port1)), IPPORT_ARG((ip2), (port2))

#define SK_ADDR_FMT        QUIN_FMT
#define SK_ADDR_ARGS(skf)                                       \
    QUIN_ARGS(skf->sf_local_port->lp_protocol,                  \
              skf->sf_laddr, skf->sf_local_port->lp_lport,      \
              skf->sf_raddr, skf->sf_rport)

#define SK_WILD_ADDR_FMT   TRIPLE_FMT
#define SK_WILD_ADDR_ARGS(skf)                                  \
    TRIPLE_ARGS(skf->sf_local_port->lp_protocol,                \
                skf->sf_laddr, skf->sf_local_port->lp_lport)


static void
oof_mcast_filter_list_free(ci_dllist* mcast_filters);

static void
oof_socket_mcast_install(struct oof_manager* fm, struct oof_socket* skf);

static void
oof_socket_mcast_remove(struct oof_manager* fm, struct oof_socket* skf,
                        ci_dllist* mcast_filters);

static unsigned
oof_mcast_filter_unconflicted_hwports(struct oof_local_port* lp,
                                      struct oof_mcast_filter* mf);

/**********************************************************************
***********************************************************************
**********************************************************************/

static int __oof_hw_filter_set(struct oof_socket* skf,
                               struct oo_hw_filter* oofilter,
                               struct tcp_helper_resource_s* trs, int protocol,
                               unsigned saddr, int sport,
                               unsigned daddr, int dport,
                               unsigned hwport_mask, int fail_is_error,
                               const char* caller)
{
  int rc;

  rc = oo_hw_filter_set(oofilter, trs, protocol, saddr, sport,
                            daddr, dport, hwport_mask);
  if( rc == 0 ) {
    IPF_LOG(FSK_FMT "FILTER "QUIN_FMT, caller, SK_PRI_ARGS(skf),
            QUIN_ARGS(protocol, daddr, dport, saddr, sport));
    oof_dl_filter_set(oofilter,
                      oof_cb_stack_id(oof_cb_socket_stack(skf)),
                      protocol, saddr, sport, daddr, dport);
  }
  else if( rc == -EACCES ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "FILTER "QUIN_FMT" blocked by firewall",
                        caller, SK_PRI_ARGS(skf),
                        QUIN_ARGS(protocol, daddr, dport, saddr, sport)));
  }
  else if( fail_is_error ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: FILTER "QUIN_FMT" failed (%d)",
                        caller, SK_PRI_ARGS(skf),
                        QUIN_ARGS(protocol, daddr, dport, saddr, sport), rc));
  }
  else {
    IPF_LOG(FSK_FMT "ERROR: FILTER "QUIN_FMT" failed (%d)", caller,
            SK_PRI_ARGS(skf), QUIN_ARGS(protocol,daddr,dport,saddr,sport), rc);
  }
  return rc;
}


static void __oof_hw_filter_clear_full(struct oof_socket* skf,
                                       const char* caller)
{
  oof_dl_filter_del(&skf->sf_full_match_filter);
  oo_hw_filter_clear(&skf->sf_full_match_filter);
  IPF_LOG(FSK_FMT "CLEAR "SK_ADDR_FMT,
          caller, SK_PRI_ARGS(skf), SK_ADDR_ARGS(skf));
}


static void __oof_hw_filter_clear_wild(struct oof_local_port* lp,
                                       struct oof_local_port_addr* lpa,
                                       unsigned laddr,
                                       const char* caller)
{
  if( lpa->lpa_filter.trs != NULL ) {
    IPF_LOG("%s: CLEAR "TRIPLE_FMT" stack=%d", caller,
            TRIPLE_ARGS(lp->lp_protocol, laddr, lp->lp_lport),
            oof_cb_stack_id(lpa->lpa_filter.trs));
    oof_dl_filter_del(&lpa->lpa_filter);
    oo_hw_filter_clear(&lpa->lpa_filter);
  }
}


static void __oof_hw_filter_move(struct oof_socket* skf,
                                 struct oof_local_port* lp,
                                 struct oof_local_port_addr* lpa,
                                 unsigned laddr, unsigned hwport_mask,
                                 const char* caller)
{
  IPF_LOG(FSK_FMT "MOVE "TRIPLE_FMT" from stack %d", caller, SK_PRI_ARGS(skf),
          TRIPLE_ARGS(lp->lp_protocol, laddr, lp->lp_lport),
          oof_cb_stack_id(lpa->lpa_filter.trs));
  oo_hw_filter_update(&lpa->lpa_filter, oof_cb_socket_stack(skf),
                      lp->lp_protocol, 0, 0, laddr, lp->lp_lport,
                      hwport_mask);
}


#define oof_hw_filter_set(skf, f, s, p, sa, sp, da, dp, pp, fie)        \
      __oof_hw_filter_set((skf), (f), (s), (p), (sa), (sp), (da),       \
                          (dp), (pp), (fie), __FUNCTION__)

#define oof_hw_filter_clear_full(skf)                   \
      __oof_hw_filter_clear_full((skf), __FUNCTION__)

#define oof_hw_filter_clear_wild(lp, lpa, laddr)                        \
      __oof_hw_filter_clear_wild((lp), (lpa), (laddr), __FUNCTION__)

#define oof_hw_filter_move(skf, lp, lpa, laddr, hwports)                \
  __oof_hw_filter_move((skf), (lp), (lpa), (laddr), (hwports), __FUNCTION__)


static void oof_sw_insert_fail(struct oof_socket* skf,
                               const char* func, int rc)
{
  /* Currently just log and continue in these cases.  Possible responses:
   * (1) Mark the interface for no further acceleration.  (2) Remove some
   * "non-critical" filters such as UDP to make space.
   */
  OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: "SK_ADDR_FMT" could not "
                      "add s/w filter (%d)", func, SK_PRI_ARGS(skf),
                      SK_ADDR_ARGS(skf), rc));
}

/**********************************************************************
***********************************************************************
**********************************************************************/

static void
oof_local_port_addr_init(struct oof_local_port_addr* lpa)
{
  oo_hw_filter_init(&lpa->lpa_filter);
  ci_dllist_init(&lpa->lpa_semi_wild_socks);
  ci_dllist_init(&lpa->lpa_full_socks);
  lpa->lpa_n_full_sharers = 0;
}




static void
oof_local_port_free(struct oof_manager* fm, struct oof_local_port* lp)
{
  ci_assert(lp->lp_refs == 0);
  ci_assert(ci_dllist_is_empty(&lp->lp_wild_socks));
  ci_assert(ci_dllist_is_empty(&lp->lp_mcast_filters));
  ci_assert(fm->fm_local_addr_n >= 0);

#ifndef NDEBUG
  {
    ci_irqlock_state_t lock_flags;
    int la_i;
    ci_irqlock_lock(&fm->fm_lock, &lock_flags);
    for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
      struct oof_local_port_addr* lpa = &lp->lp_addr[la_i];
      ci_assert(lpa->lpa_filter.trs == NULL);
      ci_assert(ci_dllist_is_empty(&lpa->lpa_semi_wild_socks));
      ci_assert(ci_dllist_is_empty(&lpa->lpa_full_socks));
    }
    ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  }
#endif
  ci_free(lp->lp_addr);
  ci_free(lp);
}


static struct oof_local_port*
oof_local_port_alloc(struct oof_manager* fm, int protocol, int lport)
{
  struct oof_local_port* lp;
  int la_i;

  ci_assert(fm->fm_local_addr_n >= 0);

  lp = CI_ALLOC_OBJ(struct oof_local_port);
  if( lp == NULL ) 
    return NULL;
  lp->lp_addr = CI_ALLOC_ARRAY(struct oof_local_port_addr, 
                               fm->fm_local_addr_max);
  if( lp->lp_addr == NULL ) {
    ci_free(lp);
    return NULL;
  }
  
  lp->lp_lport = lport;
  lp->lp_protocol = protocol;
  lp->lp_refs = 0;
  ci_dllist_init(&lp->lp_wild_socks);
  ci_dllist_init(&lp->lp_mcast_filters);
  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i )
    oof_local_port_addr_init(&lp->lp_addr[la_i]);
  return lp;
}

/**********************************************************************
***********************************************************************
**********************************************************************/

void
oof_socket_ctor(struct oof_socket* skf)
{
  skf->sf_local_port = NULL;
  oo_hw_filter_init(&skf->sf_full_match_filter);
  ci_dllist_init(&skf->sf_mcast_memberships);
}


void
oof_socket_dtor(struct oof_socket* skf)
{
  ci_assert(skf->sf_local_port == NULL);
  ci_assert(skf->sf_full_match_filter.trs == NULL);
  ci_assert(ci_dllist_is_empty(&skf->sf_mcast_memberships));
}


static struct oof_socket*
oof_socket_at_head(ci_dllist* list)
{
  if( ci_dllist_is_empty(list) )
    return NULL;
  else
    return CI_CONTAINER(struct oof_socket, sf_lp_link, ci_dllist_head(list));
}


static struct oof_socket*
oof_socket_list_find_matching_stack(ci_dllist* list,
                                    struct tcp_helper_resource_s* stack)
{
  struct oof_socket* skf;
  CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link, list)
    if( oof_cb_socket_stack(skf) == stack )
      return skf;
  return NULL;
}


static int
oof_socket_is_first_in_stack(ci_dllist* list, struct oof_socket* skf,
                             struct tcp_helper_resource_s* stack)
{
  return skf == oof_socket_list_find_matching_stack(list, stack);
}


static int
oof_socket_is_first_in_same_stack(ci_dllist* list, struct oof_socket* skf)
{
  /* Return true if [skf] is the first socket in the list, considering only
   * sockets in the same stack as [skf].
   */
  return oof_socket_is_first_in_stack(list, skf, oof_cb_socket_stack(skf));
}

/**********************************************************************
***********************************************************************
**********************************************************************/

static int
lp_hash(int protocol, int lport)
{
  return (protocol + lport) & OOF_LOCAL_PORT_TBL_MASK;
}


struct oof_manager*
oof_manager_alloc(unsigned local_addr_max)
{
  struct oof_manager* fm;
  int hash;

  ci_assert(the_manager == NULL);

  fm = CI_ALLOC_OBJ(struct oof_manager);
  if( fm == NULL )
    return NULL;
  fm->fm_local_addrs = CI_ALLOC_ARRAY(struct oof_local_addr, local_addr_max);
  if( fm->fm_local_addrs == NULL ) {
    ci_free(fm);
    return NULL;
  }

  ci_irqlock_ctor(&fm->fm_lock);
  fm->fm_local_addr_n = 0;
  fm->fm_local_addr_max = local_addr_max;
  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    ci_dllist_init(&fm->fm_local_ports[hash]);
  ci_dllist_init(&fm->fm_mcast_laddr_socks);
  the_manager = fm;
  return fm;
}


void
oof_manager_free(struct oof_manager* fm)
{
  int hash;
  ci_assert(ci_dllist_is_empty(&fm->fm_mcast_laddr_socks));
  ci_assert(fm == the_manager);
  the_manager = NULL;
  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    ci_assert(ci_dllist_is_empty(&fm->fm_local_ports[hash]));
  ci_irqlock_dtor(&fm->fm_lock);
  ci_free(fm->fm_local_addrs);
  ci_free(fm);
}


static int
oof_manager_addr_find(struct oof_manager* fm, unsigned laddr)
{
  int la_i;

  ci_assert(fm->fm_local_addr_n >= 0);
  ci_irqlock_check_locked(&fm->fm_lock);

  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i )
    if( fm->fm_local_addrs[la_i].la_laddr == laddr )
      return la_i;
  return -1;
}


static void
__oof_manager_addr_add(struct oof_manager* fm, unsigned laddr, unsigned ifindex)
{
  struct tcp_helper_resource_s* skf_stack;
  struct oof_local_port_addr* lpa;
  struct oof_local_port* lp;
  struct oof_local_addr* la;
  struct oof_local_interface* li;
  struct oof_socket* skf;
  int hash, la_i, is_new, is_active;

  ci_assert(laddr != 0);
  ci_assert(! CI_IP_IS_MULTICAST(laddr));
  ci_irqlock_check_locked(&fm->fm_lock);

  /* Duplicate? */
  la_i = oof_manager_addr_find(fm, laddr);
  if( la_i >= 0 ) {
    la = &fm->fm_local_addrs[la_i];
    is_active = ci_dllist_not_empty(&la->la_active_ifs);
    CI_DLLIST_FOR_EACH2(struct oof_local_interface, li, li_addr_link, 
                        &la->la_active_ifs) {
      if( li->li_ifindex == ifindex )
        break;
    }
    if( li == NULL ) {
      li = CI_ALLOC_OBJ(struct oof_local_interface);
      if( li == NULL ) {
        ci_log("%s: ERROR: "IP_FMT" couldn't allocate space for ifindex %d",
               __FUNCTION__, IP_ARG(laddr), ifindex);
        return; 
      }
      li->li_ifindex = ifindex;
      ci_dllist_push(&la->la_active_ifs, &li->li_addr_link);
    }
    if( is_active )
      /* This local address is already active, nothing further to do. */
      return;
    is_new = 0;
  }
  else {
    /* New entry in local address table. */
    la_i = oof_manager_addr_find(fm, 0);
    if( la_i < 0 ) {
      if( fm->fm_local_addr_n ==  fm->fm_local_addr_max ) {
        ci_log("%s: ERROR: "IP_FMT" overflows local address table",
               __FUNCTION__, IP_ARG(laddr));
        return;
      }
      la_i = fm->fm_local_addr_n;
      ++fm->fm_local_addr_n;
    }
    la = &fm->fm_local_addrs[la_i];
    la->la_laddr = laddr;
    la->la_sockets = 0;
    ci_dllist_init(&la->la_active_ifs);
    li = CI_ALLOC_OBJ(struct oof_local_interface);
    if( li == NULL ) {
      ci_log("%s: ERROR: "IP_FMT" couldn't allocate space for ifindex %d",
             __FUNCTION__, IP_ARG(laddr), ifindex);
      la->la_laddr = 0;
      return; 
    }
    li->li_ifindex = ifindex;
    ci_dllist_push(&la->la_active_ifs, &li->li_addr_link);
    is_new = 1;
  }

  /* Add new filters, and set new filters for wildcard users. */

  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash]) {
      lpa = &lp->lp_addr[la_i];
      if( is_new )
        oof_local_port_addr_init(lpa);
      /* Add h/w filter for wild sockets. */
      skf = NULL;
      if( ci_dllist_not_empty(&lpa->lpa_semi_wild_socks) ) {
        ci_assert(!is_new);
        skf = oof_socket_at_head(&lpa->lpa_semi_wild_socks);
      }
      else if( ci_dllist_not_empty(&lp->lp_wild_socks) ) {
        skf = oof_socket_at_head(&lp->lp_wild_socks);
      }
      if( skf != NULL )
        oof_hw_filter_set(skf, &lpa->lpa_filter, oof_cb_socket_stack(skf),
                          lp->lp_protocol, 0, 0, laddr, lp->lp_lport,
                          oof_hwports_available & oof_hwports_up, 1);
      /* Add h/w filters for full-match sockets. */
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lpa->lpa_full_socks) {
        ci_assert(!is_new);
        skf_stack = oof_cb_socket_stack(skf);
        if( lpa->lpa_filter.trs == skf_stack )
          ++lpa->lpa_n_full_sharers;
        else
          oof_hw_filter_set(skf, &skf->sf_full_match_filter, skf_stack,
                            lp->lp_protocol, skf->sf_raddr, skf->sf_rport,
                            skf->sf_laddr, lp->lp_lport,
                            oof_hwports_available & oof_hwports_up, 1);
      }
      /* Add s/w filters for wild sockets. */
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lp->lp_wild_socks)
        if( oof_socket_is_first_in_same_stack(&lp->lp_wild_socks, skf) ) {
          int rc = oof_cb_sw_filter_insert(skf, laddr, lp->lp_lport, 0, 0,
                                           lp->lp_protocol);
          if( rc != 0 ) {
            oof_sw_insert_fail(skf, __FUNCTION__, rc);
            /* Remove the corresponding hardware filters so that traffic
             * continues to reach the socket, albeit without acceleration.
             * BUT don't do that if existing TCP connections are using the
             * hardware filter.
             */
            if( lp->lp_protocol == IPPROTO_UDP ||
                lpa->lpa_n_full_sharers == 0 )
              oof_hw_filter_clear_wild(lp, lpa, laddr);
          }
        }
    }
}


void
oof_manager_addr_add(struct oof_manager* fm, unsigned laddr, unsigned ifindex)
{
  ci_irqlock_state_t lock_flags;
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  IPF_LOG("%s: "IP_FMT, __FUNCTION__, IP_ARG(laddr));
  __oof_manager_addr_add(fm, laddr, ifindex);
  IPF_LOG("%s: "IP_FMT" done", __FUNCTION__, IP_ARG(laddr));
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
}


static void
oof_manager_addr_dead(struct oof_manager* fm, struct oof_local_addr* la)
{
  /* Disable/remove table entry.  We can't be bothered to deal with
   * shuffling table entries here, so just mark the entry as free.
   */
  ci_assert(la->la_sockets == 0);
  ci_assert( ci_dllist_is_empty(&la->la_active_ifs) );
  la->la_laddr = 0;
}


static void
__oof_manager_addr_del(struct oof_manager* fm, unsigned laddr, unsigned ifindex)
{
  struct oof_local_port_addr* lpa;
  struct oof_local_port* lp;
  struct oof_local_addr* la;
  struct oof_local_interface* li;
  struct oof_local_interface* li_tmp;
  struct oof_socket* skf;
  int hash, la_i;

  ci_assert(laddr != 0);
  ci_irqlock_check_locked(&fm->fm_lock);

  la_i = oof_manager_addr_find(fm, laddr);
  if( la_i < 0 )
    /* We never added this address, possibly due to overflow. */
    return;
  la = &fm->fm_local_addrs[la_i];

  if( ci_dllist_is_empty(&la->la_active_ifs) ) {
    /* Unused, so don't need do anything */
    return;
  }

  CI_DLLIST_FOR_EACH3(struct oof_local_interface, li, li_addr_link, 
                      &la->la_active_ifs, li_tmp) {
    if( li->li_ifindex == ifindex ) {
      ci_dllist_remove(&li->li_addr_link);
      ci_free(li);
    }
  }

  if( ci_dllist_not_empty(&la->la_active_ifs) ) {
    /* Not yet, unused, so don't do anything yet */
    return;
  }

  /* Address is disabled; remove filters. */
  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash]) {
      lpa = &lp->lp_addr[la_i];
      /* Remove h/w filters that use [laddr]. */
      oof_hw_filter_clear_wild(lp, lpa, la->la_laddr);
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lpa->lpa_full_socks)
        oof_hw_filter_clear_full(skf);
      lpa->lpa_n_full_sharers = 0;
      /* Remove s/w filters for wild sockets. */
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lp->lp_wild_socks)
        if( oof_socket_is_first_in_same_stack(&lp->lp_wild_socks, skf) )
          oof_cb_sw_filter_remove(skf, laddr, lp->lp_lport, 0, 0,
                                  lp->lp_protocol);
    }

  if( la->la_sockets )
    return;

  /* Address is no longer in use by any socket. */
#ifndef NDEBUG
  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash]) {
      lpa = &lp->lp_addr[la_i];
      ci_assert(ci_dllist_is_empty(&lpa->lpa_semi_wild_socks));
      ci_assert(ci_dllist_is_empty(&lpa->lpa_full_socks));
      ci_assert(lpa->lpa_filter.trs == NULL);
    }
#endif

  oof_manager_addr_dead(fm, la);
}


void
oof_manager_addr_del(struct oof_manager* fm, unsigned laddr, unsigned ifindex)
{
  ci_irqlock_state_t lock_flags;
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  IPF_LOG("%s: "IP_FMT, __FUNCTION__, IP_ARG(laddr));
  __oof_manager_addr_del(fm, laddr, ifindex);
  IPF_LOG("%s: "IP_FMT" done", __FUNCTION__, IP_ARG(laddr));
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
}

/**********************************************************************
***********************************************************************
**********************************************************************/

static void
oof_manager_update_all_filters(struct oof_manager* fm)
{
  /* Invoked when physical interfaces come and go.  We add and remove
   * hardware filters to ensure that we don't receive packets through
   * interfaces that are down.  (At time of writing nothing in the net
   * driver or hardware stops packets being delivered when the interface is
   * administratively down).
   */
  struct oof_local_port_addr* lpa;
  struct oof_mcast_filter* mf;
  struct oof_local_port* lp;
  struct oof_socket* skf;
  unsigned laddr, hwport_mask;
  int hash, la_i;

  /* Find all filters potentially affected by a change in the set of
   * hwports, and modify the set of ports filtered as needed.
   */
  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash]) {
      /* Find and update unicast filters. */
      for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
        lpa = &lp->lp_addr[la_i];
        laddr = fm->fm_local_addrs[la_i].la_laddr;
        if( lpa->lpa_filter.trs != NULL )
          oo_hw_filter_update(&lpa->lpa_filter, lpa->lpa_filter.trs,
                              lp->lp_protocol, 0, 0, laddr, lp->lp_lport,
                              oof_hwports_available & oof_hwports_up);
        CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                            &lpa->lpa_full_socks)
          if( skf->sf_full_match_filter.trs != NULL )
            oo_hw_filter_update(&skf->sf_full_match_filter,
                                skf->sf_full_match_filter.trs,
                                lp->lp_protocol, skf->sf_raddr, skf->sf_rport,
                                skf->sf_laddr, lp->lp_lport,
                                oof_hwports_available & oof_hwports_up);
      }
      /* Find and update multicast filters. */
      CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                          &lp->lp_mcast_filters)
        if( mf->mf_filter.trs != NULL ) {
          hwport_mask = oof_mcast_filter_unconflicted_hwports(lp, mf);
          hwport_mask &= oof_hwports_up & oof_hwports_available;
          oo_hw_filter_update(&mf->mf_filter, mf->mf_filter.trs,
                              lp->lp_protocol, 0, 0,
                              mf->mf_maddr, lp->lp_lport, hwport_mask);
        }
    }
}


void oof_hwport_up_down(int hwport, int up)
{
  /* A physical interface has gone up or down.
   *
   * Caller must *not* hold the control-plane lock.
   */
  struct oof_manager* fm = the_manager;
  ci_irqlock_state_t cicp_lock_state;
  ci_irqlock_state_t lock_flags;
  oof_cb_cicp_lock(&cicp_lock_state);
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  if( up )
    oof_hwports_up |= 1 << hwport;
  else
    oof_hwports_up &= ~(1 << hwport);
  IPF_LOG("%s: %s hwport=%d mask=%x",
          __FUNCTION__, up?"UP":"DOWN", hwport, oof_hwports_up);
  oof_manager_update_all_filters(fm);
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);
}


void oof_hwport_un_available(int hwport, int available)
{
  /* A physical interface is (or isn't) unavailable because it is a member
   * of an unacceleratable bond.  ie. We should(n't) install filters on
   * this hwport.
   *
   * Caller must hold the control-plane lock.
   */
  struct oof_manager* fm = the_manager;
  ci_irqlock_state_t lock_flags;
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  if( available )
    oof_hwports_available |= 1 << hwport;
  else
    oof_hwports_available &= ~(1 << hwport);
  IPF_LOG("%s: %s hwport=%d mask=%x",
          __FUNCTION__, available?"AVAIL":"UNAVAIL",
               hwport, oof_hwports_available);
  oof_manager_update_all_filters(fm);
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
}

/**********************************************************************
***********************************************************************
**********************************************************************/

static struct oof_local_port*
oof_local_port_find(struct oof_manager* fm, int protocol, int lport)
{
  struct oof_local_port* lp;
  ci_irqlock_check_locked(&fm->fm_lock);
  CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                      &fm->fm_local_ports[lp_hash(protocol, lport)])
    if( lp->lp_protocol == protocol && lp->lp_lport == lport )
      return lp;
  return NULL;
}


static struct oof_local_port*
oof_local_port_get(struct oof_manager* fm, int protocol, int lport)
{
  ci_irqlock_state_t lock_flags;
  struct oof_local_port* new_lp = NULL;
  struct oof_local_port* lp;

  while( 1 ) {
    ci_irqlock_lock(&fm->fm_lock, &lock_flags);
    lp = oof_local_port_find(fm, protocol, lport);
    if( lp == NULL && new_lp ) {
      lp = new_lp;
      ci_dllist_push_tail(&fm->fm_local_ports[lp_hash(protocol, lport)],
                          &lp->lp_manager_link);
      new_lp = NULL;
    }
    if( lp != NULL )
      ++lp->lp_refs;
    ci_irqlock_unlock(&fm->fm_lock, &lock_flags);

    if( lp != NULL )
      break;

    new_lp = oof_local_port_alloc(fm, protocol, lport);
    if( new_lp == NULL ) {
      OO_DEBUG_ERR(ci_log("%s: ERROR: out of memory", __FUNCTION__));
      return NULL;
    }
  }

  if( new_lp != NULL )
    oof_local_port_free(fm, new_lp);

  return lp;
}


static struct oof_socket*
oof_wild_socket(struct oof_local_port* lp, struct oof_local_port_addr* lpa)
{
  struct oof_socket* skf;
  skf = oof_socket_at_head(&lpa->lpa_semi_wild_socks);
  if( skf == NULL )
    skf = oof_socket_at_head(&lp->lp_wild_socks);
  return skf;
}


static struct oof_socket*
oof_wild_socket_matching_stack(struct oof_local_port* lp,
                               struct oof_local_port_addr* lpa,
                               struct tcp_helper_resource_s* stack)
{
  struct oof_socket* skf;
  skf = oof_socket_list_find_matching_stack(&lpa->lpa_semi_wild_socks, stack);
  if( skf == NULL )
    skf = oof_socket_list_find_matching_stack(&lp->lp_wild_socks, stack);
  return skf;
}


static void
oof_full_socks_del_hw_filters(struct oof_manager* fm,
                              struct oof_local_port* lp,
                              struct oof_local_port_addr* lpa)
{
  struct tcp_helper_resource_s* stack = lpa->lpa_filter.trs;
  struct oof_socket* skf;

  ci_irqlock_check_locked(&fm->fm_lock);

  CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                      &lpa->lpa_full_socks) {
    if( oof_cb_socket_stack(skf) != stack )
      continue;
    if( skf->sf_full_match_filter.trs == NULL )
      continue;
    oof_hw_filter_clear_full(skf);
    ++lpa->lpa_n_full_sharers;
  }
}


static int
oof_full_socks_add_hw_filters(struct oof_manager* fm,
                              struct oof_local_port* lp,
                              struct oof_local_port_addr* lpa)
{
  /* For each full-match socket that is relying on the filter associated
   * with [lpa], try to insert a full-match filter.  Called when the filter
   * associated with [lpa] is about to be removed or pointed at a different
   * stack.
   */
  struct tcp_helper_resource_s* filter_stack = lpa->lpa_filter.trs;
  struct oof_socket* skf_tmp;
  struct oof_socket* skf;
  int rc = 0;

  ci_irqlock_check_locked(&fm->fm_lock);

  if( filter_stack == NULL ) {
    OO_DEBUG_ERR(ci_log("%s: ERROR: %s:%d has no filter",
                        __FUNCTION__, FMT_PROTOCOL(lp->lp_protocol),
                        FMT_PORT(lp->lp_lport)));
    return -EINVAL;
  }

  CI_DLLIST_FOR_EACH3(struct oof_socket, skf, sf_lp_link,
                      &lpa->lpa_full_socks, skf_tmp) {
    if( oof_cb_socket_stack(skf) != filter_stack )
      continue;
    if( skf->sf_full_match_filter.trs != NULL )
      continue;
    rc = oof_hw_filter_set(skf, &skf->sf_full_match_filter, filter_stack,
                           lp->lp_protocol, skf->sf_raddr, skf->sf_rport,
                           skf->sf_laddr, lp->lp_lport,
                           oof_hwports_available & oof_hwports_up, 0);
    if( rc < 0 ) {
      oof_full_socks_del_hw_filters(fm, lp, lpa);
      break;
    }
    ci_assert(lpa->lpa_n_full_sharers > 0);
    --lpa->lpa_n_full_sharers;
  }

  return rc;
}


/* Reasons why fixup_wild() is called. */
enum fixup_wild_why {
  fuw_del_full,
  fuw_del_wild,
  fuw_add_wild,
  fuw_udp_connect,
};


static void
oof_local_port_addr_fixup_wild(struct oof_manager* fm,
                               struct oof_local_port* lp,
                               struct oof_local_port_addr* lpa,
                               unsigned laddr, enum fixup_wild_why why)
{
  struct tcp_helper_resource_s* skf_stack;
  struct oof_socket* skf;
  int rc, skf_has_filter;
  int unshare_full_match;
  int thresh;

  /* Decide whether we need to insert full-match filters for sockets that
   * are currently sharing a wild filter.
   */
  skf = oof_wild_socket(lp, lpa);
  unshare_full_match = lpa->lpa_n_full_sharers > 0;
  if( skf == NULL ) {
    thresh = oof_shared_keep_thresh;
    skf_stack = NULL;
  }
  else {
    thresh = oof_shared_steal_thresh;
    skf_stack = oof_cb_socket_stack(skf);
    if( lpa->lpa_filter.trs == skf_stack )
      /* The existing filter points at the correct stack, so no need to add
       * filters for full-match sockets in that stack.
       */
      unshare_full_match = 0;
  }
  if( unshare_full_match && lpa->lpa_n_full_sharers > thresh ) {
    /* There are lots of sockets still using this wild filter.  We choose
     * not to transfer them all to their own full-match filters, as that
     * would consume lots of h/w resources.  This new socket will have to
     * wait until the filter is freed up.
     *
     * This is not really an error, as user can change
     * oof_shared_[keep|steal]_thresh if they don't like it.  We emit a log
     * message by default (when wild filter is added or removed), as
     * otherwise it can be tricky to see what is going on.
     */
    if( (oo_debug_bits & __OO_DEBUGIPF__) ||
        ((oo_debug_bits & __OO_DEBUGERR__) && why != fuw_del_full) ) {
      ci_log("%s: "TRIPLE_FMT" shared by %d socks in stack %d (thresh=%d "
             "reason=%d)", __FUNCTION__,
             TRIPLE_ARGS(lp->lp_protocol, laddr, lp->lp_lport),
             lpa->lpa_n_full_sharers, oof_cb_stack_id(lpa->lpa_filter.trs),
             thresh, (int) why);
      if( skf != NULL )
        ci_log("%s: WARNING: "SK_FMT" "SK_WILD_ADDR_FMT" will not yet receive "
               "traffic", __FUNCTION__, SK_PRI_ARGS(skf),
               SK_WILD_ADDR_ARGS(skf));
    }
    unshare_full_match = 0;
  }

  if( unshare_full_match ) {
    rc = oof_full_socks_add_hw_filters(fm, lp, lpa);
    if( rc < 0 ) {
      if( (oo_debug_bits & __OO_DEBUGIPF__) ||
          ((oo_debug_bits & __OO_DEBUGERR__) && why != fuw_del_full) ) {
        ci_log("%s: %s"TRIPLE_FMT" unable to free wild filter (%d sharers "
               "in stack %d, rc=%d reason=%d)", __FUNCTION__,
               skf == NULL ? "":"ERROR: ",
               TRIPLE_ARGS(lp->lp_protocol, laddr, lp->lp_lport),
               lpa->lpa_n_full_sharers, oof_cb_stack_id(lpa->lpa_filter.trs),
               rc, (int) why);
        if( skf != NULL )
          ci_log("%s: WARNING: "SK_FMT" "SK_WILD_ADDR_FMT" will not yet "
                 "receive traffic", __FUNCTION__, SK_PRI_ARGS(skf),
                 SK_WILD_ADDR_ARGS(skf));
      }
    }
  }

  if( skf != NULL ) {
    skf_has_filter = 0;
    if( lpa->lpa_filter.trs == NULL ) {
      ci_assert(lpa->lpa_n_full_sharers == 0);
      rc = oof_hw_filter_set(skf, &lpa->lpa_filter, skf_stack, lp->lp_protocol,
                             0, 0, laddr, lp->lp_lport,
                             oof_hwports_available & oof_hwports_up, 1);
      skf_has_filter = rc == 0;
    }
    else if( lpa->lpa_filter.trs != skf_stack && lpa->lpa_n_full_sharers==0 ) {
      oof_hw_filter_move(skf, lp, lpa, laddr,
                         oof_hwports_available & oof_hwports_up);
      ci_assert(lpa->lpa_filter.trs == skf_stack);
      skf_has_filter = 1;
    }
    if( skf_has_filter )
      oof_full_socks_del_hw_filters(fm, lp, lpa);
  }
  else if( lpa->lpa_n_full_sharers == 0 ) {
    oof_hw_filter_clear_wild(lp, lpa, laddr);
  }
}


static void
oof_local_port_fixup_wild(struct oof_manager* fm, struct oof_local_port* lp,
                          enum fixup_wild_why why)
{
  struct oof_local_addr* la;
  int la_i;
  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
    la = &fm->fm_local_addrs[la_i];
    if( ci_dllist_not_empty(&la->la_active_ifs) )
      oof_local_port_addr_fixup_wild(fm, lp, &lp->lp_addr[la_i],
                                     la->la_laddr, why);
  }
}


/* Fixme: most callers of oof_cb_sw_filter_insert and
 * oof_socket_add_full_sw do not check rc. */
static int
oof_socket_add_full_sw(struct oof_socket* skf)
{
  return oof_cb_sw_filter_insert(skf, skf->sf_laddr,
                                 skf->sf_local_port->lp_lport,
                                 skf->sf_raddr, skf->sf_rport,
                                 skf->sf_local_port->lp_protocol);
}


static void
oof_socket_del_full_sw(struct oof_socket* skf)
{
  struct oof_local_port* lp = skf->sf_local_port;
  oof_cb_sw_filter_remove(skf, skf->sf_laddr, lp->lp_lport,
                          skf->sf_raddr, skf->sf_rport, lp->lp_protocol);
}


static void
oof_socket_del_full(struct oof_manager* fm, struct oof_socket* skf,
                    struct oof_local_port_addr* lpa)
{
  ci_dllist_remove(&skf->sf_lp_link);
  oof_socket_del_full_sw(skf);
  if( skf->sf_full_match_filter.trs != NULL ) {
    oof_hw_filter_clear_full(skf);
  }
  else if( oof_cb_socket_stack(skf) == lpa->lpa_filter.trs ) {
    ci_assert(lpa->lpa_n_full_sharers > 0);
    --lpa->lpa_n_full_sharers;
    oof_local_port_addr_fixup_wild(fm, skf->sf_local_port, lpa, skf->sf_laddr,
                                   fuw_del_full);
  }
}


static int
oof_socket_add_full_hw(struct oof_manager* fm, struct oof_socket* skf,
                       struct oof_local_port_addr* lpa)
{
  struct tcp_helper_resource_s* skf_stack = oof_cb_socket_stack(skf);
  int rc;
  if( lpa->lpa_filter.trs != skf_stack ) {
    struct oof_local_port* lp = skf->sf_local_port;
    rc = oof_hw_filter_set(skf, &skf->sf_full_match_filter, skf_stack,
                           lp->lp_protocol, skf->sf_raddr, skf->sf_rport,
                           skf->sf_laddr, lp->lp_lport,
                           oof_hwports_available & oof_hwports_up, 1);
    if( rc < 0 ) {
      /* I think there are the following ways this can fail:
       *
       * - Out of memory (ENOMEM).
       * - Out of space in h/w filter table (EBUSY).
       * - Clash in h/w filter table (EEXIST).
       *
       * Is this where we get to if two sockets try to bind/connect to the
       * same 5-tuple?
       *
       * ?? TODO: Handle the various errors elegantly.
       */
      if( rc == -EBUSY )
        return rc;
      else
        return -EADDRNOTAVAIL;
    }
  }
  else {
    /* Share the existing wildcard filter for h/w demux. */
    ++lpa->lpa_n_full_sharers;
    IPF_LOG(FSK_FMT "SHARE "SK_ADDR_FMT, FSK_PRI_ARGS(skf), SK_ADDR_ARGS(skf));
  }
  return 0;
}


static int
__oof_socket_add_wild(struct oof_manager* fm, struct oof_socket* skf,
                      struct oof_local_port_addr* lpa, unsigned laddr)
{
  struct tcp_helper_resource_s* skf_stack = oof_cb_socket_stack(skf);
  struct oof_local_port* lp = skf->sf_local_port;
  struct oof_socket* other_skf;
  int rc;

  other_skf = oof_wild_socket_matching_stack(lp, lpa, skf_stack);
  if( other_skf != NULL )
    oof_cb_sw_filter_remove(other_skf, laddr, lp->lp_lport,
                            0, 0, lp->lp_protocol);
  rc = oof_cb_sw_filter_insert(skf, laddr, lp->lp_lport, 0, 0,
                               lp->lp_protocol);
  if( rc != 0 )
    return rc;

  if( lpa->lpa_filter.trs == NULL ) {
    rc = oof_hw_filter_set(skf, &lpa->lpa_filter, skf_stack, lp->lp_protocol,
                           0, 0, laddr, lp->lp_lport,
                           oof_hwports_available & oof_hwports_up, 1);
    if( rc != 0 )
      oof_cb_sw_filter_remove(skf, laddr, lp->lp_lport, 0, 0,
                              lp->lp_protocol);
    return rc;
  }
  else if( lpa->lpa_filter.trs != skf_stack ) {
    /* H/w filter already exists but points to a different stack.  This is
     * fixed if necessary in oof_local_port_addr_fixup_wild().
     */
    OO_DEBUG_IPF(other_skf = oof_wild_socket(lp, lpa);
                 if( other_skf != NULL )
                   ci_log(FSK_FMT "STEAL "TRIPLE_FMT" from "SK_FMT,
                          FSK_PRI_ARGS(skf),
                          TRIPLE_ARGS(lp->lp_protocol, laddr, lp->lp_lport),
                          SK_PRI_ARGS(other_skf)));
  }
  return 0;
}


static void
oof_socket_steal_or_add_wild(struct oof_manager* fm, struct oof_socket* skf)
{
  struct oof_local_port* lp = skf->sf_local_port;
  struct tcp_helper_resource_s* skf_stack;
  struct oof_local_port_addr* lpa;
  struct oof_local_addr* la;
  int la_i;

  ci_assert(skf->sf_raddr == 0);
  ci_assert(skf->sf_laddr == 0);

  skf_stack = oof_cb_socket_stack(skf);

  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
    la = &fm->fm_local_addrs[la_i];
    if( ci_dllist_is_empty(&la->la_active_ifs) )
      /* Entry invalid or address disabled. */
      continue;
    lpa = &lp->lp_addr[la_i];
    if( oof_socket_list_find_matching_stack(&lpa->lpa_semi_wild_socks,
                                            skf_stack) == NULL )
      /* Fixme: propogate error */
      __oof_socket_add_wild(fm, skf, lpa, la->la_laddr);
  }
}


static int
__oof_socket_add(struct oof_manager* fm, struct oof_socket* skf)
{
  struct oof_local_port* lp = skf->sf_local_port;
  struct oof_local_port_addr* lpa;
  struct oof_local_addr* la;
  int rc = 0, la_i;

  if( skf->sf_laddr ) {
    la_i = oof_manager_addr_find(fm, skf->sf_laddr);
    if( la_i < 0 ) {
      if( CI_IP_IS_MULTICAST(skf->sf_laddr) ) {
        /* Local address is bound to multicast address.  We don't insert
         * any filters in this case.  Socket will get accelerated traffic
         * iff it does IP_ADD_MEMBERSHIP.  (NB. In practice this cannot be
         * a full-match add, as that goes via oof_udp_connect()).
         */
        IPF_LOG(FSK_FMT IP_FMT" multicast -- not filtered",
                FSK_PRI_ARGS(skf), IP_ARG(skf->sf_laddr));
        ci_dllist_push(&fm->fm_mcast_laddr_socks, &skf->sf_lp_link);
        return 0;
      }
      OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: laddr="IP_FMT" not local",
                          FSK_PRI_ARGS(skf), IP_ARG(skf->sf_laddr)));
      return -EINVAL;
    }
    lpa = &lp->lp_addr[la_i];
    la = &fm->fm_local_addrs[la_i];
    if( skf->sf_raddr ) {
      if( (rc = oof_socket_add_full_sw(skf)) != 0 )
        return rc;
      if( (rc = oof_socket_add_full_hw(fm, skf, lpa)) != 0 ) {
        oof_socket_del_full_sw(skf);
        return rc;
      }
      ci_dllist_push(&lpa->lpa_full_socks, &skf->sf_lp_link);
    }
    else {
      rc = __oof_socket_add_wild(fm, skf, lpa, skf->sf_laddr);
      if( rc != 0 )
        return rc;
      ci_dllist_push(&lpa->lpa_semi_wild_socks, &skf->sf_lp_link);
      oof_local_port_addr_fixup_wild(fm, lp, lpa, skf->sf_laddr, fuw_add_wild);
    }
    ++la->la_sockets;
  }
  else {
    oof_socket_steal_or_add_wild(fm, skf);
    ci_dllist_push(&lp->lp_wild_socks, &skf->sf_lp_link);
    oof_local_port_fixup_wild(fm, lp, fuw_add_wild);
  }

  return 0;
}


int
oof_socket_add(struct oof_manager* fm, struct oof_socket* skf,
               int protocol, unsigned laddr, int lport,
               unsigned raddr, int rport)
{
  ci_irqlock_state_t lock_flags;
  struct oof_local_port* lp;
  int rc;

  IPF_LOG(FSK_FMT QUIN_FMT, FSK_PRI_ARGS(skf),
          QUIN_ARGS(protocol, laddr, lport, raddr, rport));

  lp = oof_local_port_get(fm, protocol, lport);
  if( lp == NULL ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: out of memory", FSK_PRI_ARGS(skf)));
    return -ENOMEM;
  }

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);

  rc = -EINVAL;
  if( lport == 0 || ((raddr || rport) && ! (raddr && rport)) ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: bad "IPPORT_FMT" "IPPORT_FMT,
                        FSK_PRI_ARGS(skf), IPPORT_ARG(laddr, lport),
                        IPPORT_ARG(raddr, rport)));
    goto fail;
  }
  if( skf->sf_local_port != NULL ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: already bound to "SK_ADDR_FMT,
                        FSK_PRI_ARGS(skf), SK_ADDR_ARGS(skf)));
    goto fail;
  }

  skf->sf_laddr = laddr;
  skf->sf_raddr = raddr;
  skf->sf_rport = rport;
  skf->sf_local_port = lp;
  rc = __oof_socket_add(fm, skf);
  if( rc < 0 ) {
    skf->sf_local_port = NULL;
    goto fail;
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  if( ci_dllist_not_empty(&skf->sf_mcast_memberships) )
    oof_socket_mcast_install(fm, skf);
  return 0;

 fail:
  if( --lp->lp_refs == 0 )
    ci_dllist_remove(&lp->lp_manager_link);
  else
    lp = NULL;
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  if( lp != NULL )
    oof_local_port_free(fm, lp);
  return rc;
}


static void
__oof_socket_del_wild(struct oof_socket* skf,
                      struct tcp_helper_resource_s* skf_stack,
                      struct oof_local_port_addr* lpa, unsigned laddr)
{
  struct oof_local_port* lp = skf->sf_local_port;
  struct oof_socket* other_skf;

  oof_cb_sw_filter_remove(skf, laddr, lp->lp_lport, 0, 0, lp->lp_protocol);
  other_skf = oof_wild_socket_matching_stack(lp, lpa, skf_stack);
  if( other_skf != NULL ) {
    int rc = oof_cb_sw_filter_insert(other_skf, laddr, lp->lp_lport,
                                     0, 0, lp->lp_protocol);
    if( rc != 0 )
      oof_sw_insert_fail(other_skf, __FUNCTION__, rc);
  }
}


static void
oof_socket_del_semi_wild(struct oof_manager* fm, struct oof_socket* skf,
                         struct oof_local_port_addr* lpa)
{
  struct tcp_helper_resource_s* skf_stack;
  int hidden;

  skf_stack = oof_cb_socket_stack(skf);
  hidden = ! oof_socket_is_first_in_stack(&lpa->lpa_semi_wild_socks,
                                          skf, skf_stack);
  ci_dllist_remove(&skf->sf_lp_link);
  if( ! hidden ) {
    __oof_socket_del_wild(skf, skf_stack, lpa, skf->sf_laddr);
    oof_local_port_addr_fixup_wild(fm, skf->sf_local_port, lpa,
                                   skf->sf_laddr, fuw_del_wild);
  }
}


static void
oof_socket_del_wild(struct oof_manager* fm, struct oof_socket* skf)
{
  struct tcp_helper_resource_s* skf_stack;
  struct oof_local_port* lp = skf->sf_local_port;
  struct oof_local_port_addr* lpa;
  struct oof_local_addr* la;
  int hidden, la_i;

  skf_stack = oof_cb_socket_stack(skf);
  hidden = ! oof_socket_is_first_in_stack(&lp->lp_wild_socks, skf, skf_stack);
  ci_dllist_remove(&skf->sf_lp_link);
  if( hidden )
    return;

  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
    la = &fm->fm_local_addrs[la_i];
    if(  ci_dllist_is_empty(&la->la_active_ifs) )
      /* Entry invalid or address disabled. */
      continue;
    lpa = &lp->lp_addr[la_i];
    if( oof_socket_list_find_matching_stack(&lpa->lpa_semi_wild_socks,
                                            skf_stack) == NULL )
      __oof_socket_del_wild(skf, skf_stack, lpa, la->la_laddr);
  }
}


void
oof_socket_del(struct oof_manager* fm, struct oof_socket* skf)
{
  struct oof_local_port* lp;
  struct oof_local_port_addr* lpa;
  ci_irqlock_state_t lock_flags;
  struct oof_local_addr* la;
  ci_dllist mcast_filters;
  int la_i;
  ci_irqlock_state_t cicp_lock_state;

  ci_dllist_init(&mcast_filters);

  /* Have to take the CICP lock here because we must take it before
   * the fm lock, and we might need it to convert ifindex into hwport_mask
   * for oo_hw_filter_set()
   */
  oof_cb_cicp_lock(&cicp_lock_state);

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  if( (lp = skf->sf_local_port) != NULL ) {
    IPF_LOG(FSK_FMT QUIN_FMT, FSK_PRI_ARGS(skf),
            QUIN_ARGS(lp->lp_protocol, skf->sf_laddr, lp->lp_lport,
                      skf->sf_raddr, skf->sf_rport));

    oof_socket_mcast_remove(fm, skf, &mcast_filters);

    if( CI_IP_IS_MULTICAST(skf->sf_laddr) ) {
      ci_dllist_remove(&skf->sf_lp_link);
      if( skf->sf_full_match_filter.trs != NULL ) {
        /* Undo path for oof_udp_connect_mcast_laddr(). */
        oof_socket_del_full_sw(skf);
        oof_hw_filter_clear_full(skf);
      }
    }
    else if( skf->sf_laddr ) {
      la_i = oof_manager_addr_find(fm, skf->sf_laddr);
      ci_assert(la_i >= 0 && la_i < fm->fm_local_addr_n);
      lpa = &lp->lp_addr[la_i];
      la = &fm->fm_local_addrs[la_i];
      if( skf->sf_raddr )
        oof_socket_del_full(fm, skf, lpa);
      else
        oof_socket_del_semi_wild(fm, skf, lpa);
      ci_assert(la->la_sockets > 0);
      if( --la->la_sockets == 0 &&  ci_dllist_is_empty(&la->la_active_ifs) )
        oof_manager_addr_dead(fm, la);
    }
    else {
      oof_socket_del_wild(fm, skf);
      oof_local_port_fixup_wild(fm, skf->sf_local_port, fuw_del_wild);
    }

    skf->sf_local_port = NULL;
    ci_assert(lp->lp_refs > 0);
    if( --lp->lp_refs == 0 )
      ci_dllist_remove(&lp->lp_manager_link);
    else
      lp = NULL;
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);

  if( lp != NULL )
    oof_local_port_free(fm, lp);
  oof_mcast_filter_list_free(&mcast_filters);
}


static int
oof_udp_connect_mcast_laddr(struct oof_manager* fm, struct oof_socket* skf,
                            unsigned laddr, unsigned raddr, int rport)
{
  /* We're connecting a UDP socket, and local address is a multicast
   * address.  We try to insert a full-match filter.  There is a chance
   * that it is not needed (because a wild-match multicast filter already
   * exists for this stack) but I'm not prepared to deal with that case.
   */
  struct oof_local_port* lp = skf->sf_local_port;
  unsigned laddr_old;
  int rc;

  IPF_LOG(FSK_FMT "%s "IPPORT_FMT" => "IPPORT_FMT" "IPPORT_FMT" multicast",
          FSK_PRI_ARGS(skf), FMT_PROTOCOL(lp->lp_protocol),
          IPPORT_ARG(skf->sf_laddr, lp->lp_lport),
          IPPORT_ARG(laddr, lp->lp_lport), IPPORT_ARG(raddr, rport));

  ci_irqlock_check_locked(&fm->fm_lock);
  ci_assert(CI_IP_IS_MULTICAST(skf->sf_laddr));
  ci_assert(CI_IP_IS_MULTICAST(laddr));
  ci_assert(skf->sf_full_match_filter.trs == NULL);

  laddr_old = skf->sf_laddr;
  skf->sf_laddr = laddr;
  skf->sf_raddr = raddr;
  skf->sf_rport = rport;
  rc = oof_socket_add_full_sw(skf);
  if( rc != 0 )
    goto fail1;
  rc = oof_hw_filter_set(skf, &skf->sf_full_match_filter,
                         oof_cb_socket_stack(skf), lp->lp_protocol,
                         raddr, rport, laddr, lp->lp_lport,
                         oof_hwports_available & oof_hwports_up, 1);
  if( rc != 0 )
    goto fail2;
  return 0;

 fail2:
  oof_cb_sw_filter_remove(skf, laddr, skf->sf_local_port->lp_lport,
                          raddr, rport, skf->sf_local_port->lp_protocol);
 fail1:
  skf->sf_laddr = laddr_old;
  skf->sf_raddr = 0;
  skf->sf_rport = 0;
  return rc;
}


int
oof_udp_connect(struct oof_manager* fm, struct oof_socket* skf,
                unsigned laddr, unsigned raddr, int rport)
{
  /* Special case for UDP connect().  We don't want to del() then add(), as
   * there may be an interval when there are no filters installed and
   * packets will go to the wrong place.
   */
  struct tcp_helper_resource_s* skf_stack;
  ci_irqlock_state_t lock_flags;
  struct oof_local_port_addr* lpa;
  struct oof_local_addr* la;
  struct oof_local_port* lp;
  unsigned laddr_old;
  int la_i_old = 0;  /* prevent silly compiler warning */
  int rc, la_i_new;
  int hidden;

  if( laddr == 0 || raddr == 0 || rport == 0 ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: bad laddr="IP_FMT" raddr="IP_FMT
                        " rport=%d", FSK_PRI_ARGS(skf), IP_ARG(laddr),
                        IP_ARG(raddr), FMT_PORT(rport)));
    return -EINVAL;
  }

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  lp = skf->sf_local_port;
  rc = -EINVAL;
  if( lp == NULL ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: no local port", FSK_PRI_ARGS(skf)));
    goto unlock_out;
  }
  if( lp->lp_protocol != IPPROTO_UDP || skf->sf_raddr ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: protocol=%s remote="IPPORT_FMT,
                        FSK_PRI_ARGS(skf), FMT_PROTOCOL(lp->lp_protocol),
                        IPPORT_ARG(skf->sf_raddr, skf->sf_rport)));
    goto unlock_out;
  }
  la_i_new = oof_manager_addr_find(fm, laddr);
  if( la_i_new < 0 ) {
    if( CI_IP_IS_MULTICAST(laddr) && CI_IP_IS_MULTICAST(skf->sf_laddr) ) {
      rc = oof_udp_connect_mcast_laddr(fm, skf, laddr, raddr, rport);
      if( rc < 0 )
        goto unlock_out;
      else
        goto unlock_mcast_out;
    }
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: laddr="IP_FMT" not local",
                        FSK_PRI_ARGS(skf), IP_ARG(laddr)));
    goto unlock_out;
  }

  IPF_LOG(FSK_FMT "%s "IPPORT_FMT" => "IPPORT_FMT" "IPPORT_FMT,
          FSK_PRI_ARGS(skf), FMT_PROTOCOL(lp->lp_protocol),
          IPPORT_ARG(skf->sf_laddr, lp->lp_lport),
          IPPORT_ARG(laddr, lp->lp_lport), IPPORT_ARG(raddr, rport));

  /* First half of adding as full-match.  May or may not insert full-match
   * h/w filter.  We mustn't install s/w filter until we've removed the
   * existing s/w filter else we can confuse the filter table (which
   * requires that a socket be inserted only once for a given laddr).
   */
  laddr_old = skf->sf_laddr;
  skf->sf_laddr = laddr;
  skf->sf_raddr = raddr;
  skf->sf_rport = rport;
  rc = oof_socket_add_full_hw(fm, skf, &lp->lp_addr[la_i_new]);
  if( rc < 0 )
    goto fail_reset_skf;

  /* Remove wild s/w filters.  May delete or move wild h/w filters, and may
   * insert new full-match h/w filter.  Must not "fixup" the wildcard
   * filters yet as [skf] does not yet look like a full-match socket, so
   * state is not sufficiently consistent.
   */
  skf->sf_laddr = laddr_old;
  skf->sf_raddr = 0;
  skf->sf_rport = 0;
  if( laddr_old ) {
    la_i_old = oof_manager_addr_find(fm, laddr_old);
    ci_assert(la_i_old >= 0 && la_i_old < fm->fm_local_addr_n);
    lpa = &lp->lp_addr[la_i_old];
    skf_stack = oof_cb_socket_stack(skf);
    hidden = ! oof_socket_is_first_in_stack(&lpa->lpa_semi_wild_socks,
                                            skf, skf_stack);
    ci_dllist_remove(&skf->sf_lp_link);
    if( ! hidden )
      __oof_socket_del_wild(skf, skf_stack, lpa, laddr_old);
  }
  else {
    oof_socket_del_wild(fm, skf);
  }

  /* Finish making [skf] into a proper full-match socket. */
  skf->sf_laddr = laddr;
  skf->sf_raddr = raddr;
  skf->sf_rport = rport;
  rc = oof_cb_sw_filter_insert(skf, laddr, lp->lp_lport, raddr, rport,
                               lp->lp_protocol);
  if( rc != 0 ) {
    /* NB. We haven't reset the socket to its previous state here.  We
     * leave it looking like a full-match, but with all filters missing.
     * Calling code should hand socket over to kernel, so this inconsistent
     * state should not matter much.
     */
    oof_sw_insert_fail(skf, __FUNCTION__, rc);
    oof_hw_filter_clear_full(skf);
    goto unlock_out;
  }
  ci_dllist_push(&lp->lp_addr[la_i_new].lpa_full_socks, &skf->sf_lp_link);
  ++fm->fm_local_addrs[la_i_new].la_sockets;

  /* Sort out of the h/w filter(s).  This step may insert a new full-match
   * h/w filter, and may delete or move the wild h/w filter(s).
   */
  if( laddr_old ) {
    oof_local_port_addr_fixup_wild(fm, lp, &lp->lp_addr[la_i_old],
                                   laddr_old, fuw_udp_connect);
    la = &fm->fm_local_addrs[la_i_old];
    if( --la->la_sockets == 0 && ci_dllist_is_empty(&la->la_active_ifs) )
      oof_manager_addr_dead(fm, la);
  }
  else {
    oof_local_port_fixup_wild(fm, lp, fuw_udp_connect);
  }

 unlock_mcast_out:
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  if( ci_dllist_not_empty(&skf->sf_mcast_memberships) )
    oof_socket_mcast_install(fm, skf);
  return 0;

 fail_reset_skf:
  skf->sf_laddr = laddr_old;
  skf->sf_raddr = 0;
  skf->sf_rport = 0;
 unlock_out:
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  return rc;
}

/**********************************************************************
***********************************************************************
**********************************************************************/

/* Do not install multicast filter if socket has its own full-match h/w
 * filter.
 *
 * If local address is bound, then the socket can only receive packets
 * addressed to that local address.
 */
#define OOF_NEED_MCAST_FILTER(skf, maddr)                       \
  ((skf)->sf_full_match_filter.trs == NULL &&                   \
   ((skf)->sf_laddr == 0 || (skf)->sf_laddr == (maddr)))


static struct oof_mcast_member*
oof_mcast_member_list_get(ci_dllist* mm_list)
{
  ci_assert(ci_dllist_not_empty(mm_list));
  return CI_CONTAINER(struct oof_mcast_member, mm_socket_link,
                      ci_dllist_pop(mm_list));
}


static void
oof_mcast_member_list_free(ci_dllist* mm_list)
{
  while( ci_dllist_not_empty(mm_list) )
    ci_free(oof_mcast_member_list_get(mm_list));
}


static struct oof_mcast_filter*
oof_mcast_filter_list_get(ci_dllist* mcast_filters)
{
  ci_assert(ci_dllist_not_empty(mcast_filters));
  return CI_CONTAINER(struct oof_mcast_filter, mf_lp_link,
                      ci_dllist_pop(mcast_filters));
}


static void
oof_mcast_filter_list_free(ci_dllist* mcast_filters)
{
  while( ci_dllist_not_empty(mcast_filters) )
    ci_free(oof_mcast_filter_list_get(mcast_filters));
}


static int
oof_socket_has_maddr_filter(struct oof_socket* skf, unsigned maddr)
{
  struct oof_mcast_member* mm;
  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                      &skf->sf_mcast_memberships)
    if( mm->mm_maddr == maddr && mm->mm_filter != NULL )
      return 1;
  return 0;
}


static struct oof_mcast_member*
oof_mcast_member_alloc(struct oof_socket* skf, unsigned maddr,
                       int ifindex, unsigned hwport_mask)
{
  struct oof_mcast_member* mm;
  mm = CI_ALLOC_OBJ(struct oof_mcast_member);
  if( mm != NULL ) {
    mm->mm_filter = NULL;
    mm->mm_socket = skf;
    mm->mm_maddr = maddr;
    mm->mm_ifindex = ifindex;
    mm->mm_hwport_mask = hwport_mask;
  }
  return mm;
}


static const char*
oof_mcast_member_state(struct oof_mcast_member* mm)
{
  unsigned hwports_got;
  const char* s;
  hwports_got = oo_hw_filter_hwports(&mm->mm_filter->mf_filter);
  if( mm->mm_hwport_mask ) {
    if( (hwports_got & mm->mm_hwport_mask) == mm->mm_hwport_mask )
      s = "ACCELERATED";
    else if( hwports_got & mm->mm_hwport_mask )
      s = "PARTIALLY_ACCELERATED";
    else
      s = "KERNEL";
  }
  else
    s = "NO_ACCELERATABLE_PORTS";
  return s;
}


static void
oof_mcast_filter_init(struct oof_mcast_filter* mf, unsigned maddr)
{
  oo_hw_filter_init(&mf->mf_filter);
  mf->mf_maddr = maddr;
  mf->mf_hwport_mask = 0;
  ci_dllist_init(&mf->mf_memberships);
}


static unsigned
oof_mcast_filter_hwport_mask(struct oof_mcast_filter* mf)
{
  struct oof_mcast_member* mm;
  unsigned hwport_mask = 0;
  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_filter_link,
                      &mf->mf_memberships)
    hwport_mask |= mm->mm_hwport_mask;
  return hwport_mask;
}


static struct oof_mcast_filter*
oof_local_port_find_mcast_filter(struct oof_local_port* lp,
                                 struct tcp_helper_resource_s* stack,
                                 unsigned maddr)
{
  struct oof_mcast_filter* mf;
  CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                      &lp->lp_mcast_filters)
    if( mf->mf_filter.trs == stack && mf->mf_maddr == maddr )
      break;
  return mf;
}


/* Find out whether there are any hwports that [mf] can install filters on.
 * ie. We're looking for hwports that no other stack wants to install the
 * same multicast filter on.
 */
static unsigned
oof_mcast_filter_unconflicted_hwports(struct oof_local_port* lp,
                                      struct oof_mcast_filter* mf)
{
  unsigned hwport_mask = mf->mf_hwport_mask;
  struct oof_mcast_filter* mf2;
  CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf2, mf_lp_link,
                      &lp->lp_mcast_filters)
    if( mf2 != mf && mf2->mf_maddr == mf->mf_maddr &&
        mf2->mf_filter.trs != mf->mf_filter.trs )
      hwport_mask &= ~mf2->mf_hwport_mask;
  return hwport_mask;
}


static void
oof_mcast_install(struct oof_manager* fm, struct oof_mcast_member* mm,
                  ci_dllist* mcast_filters)
{
  struct oof_socket* skf = mm->mm_socket;
  struct tcp_helper_resource_s* skf_stack = oof_cb_socket_stack(skf);
  struct oof_local_port* lp = skf->sf_local_port;
  unsigned install_hwport_mask;
  struct oof_mcast_filter* mf;
  int rc;

  ci_assert(lp != NULL);
  ci_assert(OOF_NEED_MCAST_FILTER(skf, mm->mm_maddr));
  ci_irqlock_check_locked(&fm->fm_lock);

  /* Install a software filter if this socket doesn't already have a filter
   * for this maddr.  (This happens if the socket joins the same group on
   * more than one interface).
   */
  if( ! oof_socket_has_maddr_filter(skf, mm->mm_maddr) ) {
    rc = oof_cb_sw_filter_insert(skf, mm->mm_maddr, lp->lp_lport,
                                 0, 0, lp->lp_protocol);
    if( rc != 0 )
      return; /* SW filter failed: do not insert HW */
  }

  /* Find filters that conflict with the one we want to install.  ie. Those
   * with same maddr+port and overlapping hwports, and not pointing at the
   * same stack.  Remove hardware filters that conflict.
   *
   * NB. This will not be needed in Huntington.
   */
  CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                      &lp->lp_mcast_filters)
    if( mm->mm_maddr == mf->mf_maddr && mf->mf_filter.trs != skf_stack ) {
      if( mm->mm_hwport_mask & mf->mf_hwport_mask ) {
        IPF_LOG(FSK_FMT "CONFLICT: maddr="IPPORT_FMT" if=%d hwports=%x "
                "AND stack=%d hwports=%x", FSK_PRI_ARGS(skf),
                IPPORT_ARG(mm->mm_maddr, lp->lp_lport), mm->mm_ifindex,
                mm->mm_hwport_mask, oof_cb_stack_id(mf->mf_filter.trs),
                mf->mf_hwport_mask);
        oo_hw_filter_clear_hwports(&mf->mf_filter, mm->mm_hwport_mask);
      }
    }

  mf = oof_local_port_find_mcast_filter(lp, skf_stack, mm->mm_maddr);
  if( mf == NULL ) {
    mf = oof_mcast_filter_list_get(mcast_filters);
    oof_mcast_filter_init(mf, mm->mm_maddr);
    ci_dllist_push(&lp->lp_mcast_filters, &mf->mf_lp_link);
  }

  mm->mm_filter = mf;
  ci_dllist_push(&mf->mf_memberships, &mm->mm_filter_link);
  mf->mf_hwport_mask |= mm->mm_hwport_mask;
  install_hwport_mask = oof_mcast_filter_unconflicted_hwports(lp, mf);
  rc = oo_hw_filter_update(&mf->mf_filter, skf_stack, lp->lp_protocol,
                           0, 0, mf->mf_maddr, lp->lp_lport,
                           install_hwport_mask);
  if( rc != 0 )
    /* We didn't get all of the filters we wanted, but traffic should
     * still get there via the kernel stack.
     */
    OO_DEBUG_ERR(ci_log(FSK_FMT "mcast hw filter error: maddr="IPPORT_FMT
                        " if=%d wanted=%x,%x install=%x got=%x",
                        FSK_PRI_ARGS(skf),
                        IPPORT_ARG(mm->mm_maddr, lp->lp_lport),
                        mm->mm_ifindex, mm->mm_hwport_mask, mf->mf_hwport_mask,
                        install_hwport_mask,
                        oo_hw_filter_hwports(&mf->mf_filter)));
}


static void
oof_mcast_remove(struct oof_manager* fm, struct oof_mcast_member* mm,
                 ci_dllist* mcast_filters)
{
  struct oof_mcast_filter* mf = mm->mm_filter;
  struct oof_socket* skf = mm->mm_socket;
  struct oof_local_port* lp = skf->sf_local_port;
  unsigned hwport_mask;
  int rc;

  ci_assert(mm->mm_filter != NULL);
  ci_assert(ci_dllist_not_empty(&mf->mf_memberships));
  ci_assert(mf->mf_maddr == mm->mm_maddr);

  mm->mm_filter = NULL;
  ci_dllist_remove(&mm->mm_filter_link);
  if( ci_dllist_is_empty(&mf->mf_memberships) ) {
    oo_hw_filter_clear(&mf->mf_filter);
    IPF_LOG(FSK_FMT "CLEAR "IPPORT_FMT, FSK_PRI_ARGS(skf),
            IPPORT_ARG(mm->mm_maddr, lp->lp_lport));
    ci_dllist_remove(&mf->mf_lp_link);
    ci_dllist_push(mcast_filters, &mf->mf_lp_link);
  }
  else {
    mf->mf_hwport_mask = oof_mcast_filter_hwport_mask(mf);
    hwport_mask = oof_mcast_filter_unconflicted_hwports(lp, mf);
    oo_hw_filter_update(&mf->mf_filter, mf->mf_filter.trs, lp->lp_protocol,
                        0, 0, mf->mf_maddr, lp->lp_lport, hwport_mask);
  }

  /* Is it now possible to insert filters to accelerate this group for
   * another stack?
   */
  CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                      &lp->lp_mcast_filters)
    if( mf->mf_maddr == mm->mm_maddr ) {
      unsigned got_hwport_mask;
      got_hwport_mask = oo_hw_filter_hwports(&mf->mf_filter);
      if( mf->mf_hwport_mask != got_hwport_mask ) {
        hwport_mask = oof_mcast_filter_unconflicted_hwports(lp, mf);
        if( hwport_mask != got_hwport_mask ) {
          IPF_LOG(FSK_FMT "maddr="IPPORT_FMT" if=%d MODIFY stack=%d wanted=%x "
                  "had=%x install=%x", FSK_PRI_ARGS(skf),
                  IPPORT_ARG(mm->mm_maddr, lp->lp_lport), mm->mm_ifindex,
                  oof_cb_stack_id(mf->mf_filter.trs), mf->mf_hwport_mask,
                  got_hwport_mask, hwport_mask);
          rc = oo_hw_filter_update(&mf->mf_filter, mf->mf_filter.trs,
                                   lp->lp_protocol, 0, 0, mf->mf_maddr,
                                   lp->lp_lport, hwport_mask);
          if( rc != 0 )
            OO_DEBUG_ERR(ci_log("%s: mcast hw filter error: maddr="IPPORT_FMT
                                " wanted=%x install=%x got=%x", __FUNCTION__,
                                IPPORT_ARG(mf->mf_maddr, lp->lp_lport),
                                mf->mf_hwport_mask, hwport_mask,
                                oo_hw_filter_hwports(&mf->mf_filter)));
        }
      }
    }

  /* Remove software filter if no filters remain for maddr. */
  if( ! oof_socket_has_maddr_filter(skf, mm->mm_maddr) )
    oof_cb_sw_filter_remove(skf, mm->mm_maddr, lp->lp_lport,
                            0, 0, lp->lp_protocol);
}


static void
oof_mcast_update(struct oof_manager* fm, struct oof_local_port *lp,
                 struct oof_mcast_filter* mf, int ifindex)
{
  unsigned install_hwport_mask, before_hwport_mask;
  int rc;

  before_hwport_mask = oo_hw_filter_hwports(&mf->mf_filter);
  install_hwport_mask = oof_mcast_filter_unconflicted_hwports(lp, mf);
  if( install_hwport_mask != before_hwport_mask ) {
    rc = oo_hw_filter_update(&mf->mf_filter, mf->mf_filter.trs, lp->lp_protocol,
                             0, 0, mf->mf_maddr, lp->lp_lport,
                             install_hwport_mask);
    IPF_LOG("%s: UPDATE "IPPORT_FMT" if=%d hwports before=%x wanted=%x "
            "install=%x after=%x", __FUNCTION__,
            IPPORT_ARG(mf->mf_maddr, lp->lp_lport),
            ifindex, before_hwport_mask, mf->mf_hwport_mask,
            install_hwport_mask, oo_hw_filter_hwports(&mf->mf_filter));
  }
}


void
oof_mcast_update_filters(struct oof_manager* fm, int ifindex)
{
  ci_irqlock_state_t lock_flags;
  struct oof_local_port* lp;
  struct oof_mcast_filter* mf;
  struct oof_mcast_member* mm;
  unsigned hwport_mask;
  int rc, hash, touched;

  /* Caller must hold the CICP_LOCK. */
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);

  if( (rc = oof_cb_get_hwport_mask(ifindex, &hwport_mask)) != 0 ) {
    OO_DEBUG_ERR(ci_log("%s: ERROR: oof_cb_get_hwport_mask(%d) failed rc=%d",
                        __FUNCTION__, ifindex, rc));
    goto out;
  }

  IPF_LOG("%s: if=%u hwports=%x", __FUNCTION__, ifindex, hwport_mask);

  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash]) {
      /* Need to update mf_hwport_mask in all filters first for
       * oof_mcast_filter_unconflicted_hwports() to give correct results.
       */
      touched = 0;
      CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                          &lp->lp_mcast_filters)
        CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_filter_link,
                            &mf->mf_memberships)
          if( mm->mm_ifindex == ifindex ) {
            mm->mm_hwport_mask = hwport_mask;
            mf->mf_hwport_mask = oof_mcast_filter_hwport_mask(mf);
            touched = 1;
          }
      if( touched )
        CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                            &lp->lp_mcast_filters)
          oof_mcast_update(fm, lp, mf, ifindex);
    }

 out:
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
}


int
oof_socket_mcast_add(struct oof_manager* fm, struct oof_socket* skf,
                     unsigned maddr, int ifindex)
{
  ci_irqlock_state_t lock_flags;
  ci_irqlock_state_t cicp_lock_state;
  struct oof_mcast_member* new_mm;
  struct oof_mcast_filter* mf;
  struct oof_mcast_member* mm;
  ci_dllist mcast_filters;
  unsigned hwport_mask;
  int rc;

  IPF_LOG(FSK_FMT "maddr="IP_FMT" if=%d",
          FSK_PRI_ARGS(skf), IP_ARG(maddr), ifindex);

  ci_dllist_init(&mcast_filters);
  new_mm = NULL;
  if( ! CI_IP_IS_MULTICAST(maddr) ) {
    OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: maddr="IP_FMT,
                        FSK_PRI_ARGS(skf), IP_ARG(maddr)));
    rc = -EINVAL;
    goto out;
  }

  oof_cb_cicp_lock(&cicp_lock_state);
  hwport_mask = 0;
  rc = oof_cb_get_hwport_mask(ifindex, &hwport_mask);
  oof_cb_cicp_unlock(&cicp_lock_state);
  if( rc != 0 || hwport_mask == 0 ) {
    IPF_LOG(FSK_FMT "ERROR: no hwports for if=%d rc=%d",
            FSK_PRI_ARGS(skf), ifindex, rc);
    /* Carry on -- we may get hwports later due to cplane changes. */
  }

  new_mm = oof_mcast_member_alloc(skf, maddr, ifindex, hwport_mask);
  if( new_mm == NULL )
    goto out_of_memory;
  if( (mf = CI_ALLOC_OBJ(struct oof_mcast_filter)) == NULL )
    goto out_of_memory;
  ci_dllist_push(&mcast_filters, &mf->mf_lp_link);

  /* Have to take the CICP lock here because we must take it before
   * the fm lock, and we might need it to convert ifindex into hwport_mask
   * for oo_hw_filter_set()
   */
  oof_cb_cicp_lock(&cicp_lock_state);

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                      &skf->sf_mcast_memberships)
    if( mm->mm_maddr == maddr && mm->mm_ifindex == ifindex )
      break;
  if( mm == NULL ) {  /* NB. Ignore duplicates. */
    mm = new_mm;
    new_mm = NULL;
    ci_dllist_push(&skf->sf_mcast_memberships, &mm->mm_socket_link);
    if( skf->sf_local_port != NULL && OOF_NEED_MCAST_FILTER(skf, maddr) )
      oof_mcast_install(fm, mm, &mcast_filters);
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);
  rc = 0;

 out:
  if( new_mm )
    ci_free(new_mm);
  oof_mcast_filter_list_free(&mcast_filters);
  return rc;

 out_of_memory:
  OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: out of memory", FSK_PRI_ARGS(skf)));
  rc = -ENOMEM;
  goto out;
}


void
oof_socket_mcast_del(struct oof_manager* fm, struct oof_socket* skf,
                     unsigned maddr, int ifindex)
{
  ci_irqlock_state_t lock_flags;
  ci_irqlock_state_t cicp_lock_state;
  struct oof_mcast_member* mm;
  ci_dllist mcast_filters;

  IPF_LOG(FSK_FMT "maddr="IP_FMT, FSK_PRI_ARGS(skf), IP_ARG(maddr));

  ci_dllist_init(&mcast_filters);

  /* Have to take the CICP lock here because we must take it before
   * the fm lock, and we might need it to convert ifindex into hwport_mask
   * for oo_hw_filter_set()
   */
  oof_cb_cicp_lock(&cicp_lock_state);

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                      &skf->sf_mcast_memberships)
    if( mm->mm_maddr == maddr && mm->mm_ifindex == ifindex)
      break;
  if( mm != NULL ) {
    ci_dllist_remove(&mm->mm_socket_link);
    if( mm->mm_filter != NULL )
      oof_mcast_remove(fm, mm, &mcast_filters);
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);

  if( mm != NULL )
    ci_free(mm);
  oof_mcast_filter_list_free(&mcast_filters);
}


void
oof_socket_mcast_del_all(struct oof_manager* fm, struct oof_socket* skf)
{
  ci_irqlock_state_t lock_flags;
  ci_irqlock_state_t cicp_lock_state;
  struct oof_mcast_member* mm;
  ci_dllist mf_list, mm_list;

  ci_dllist_init(&mf_list);
  ci_dllist_init(&mm_list);

  /* Have to take the CICP lock here because we must take it before
   * the fm lock, and we might need it to convert ifindex into hwport_mask
   * for oo_hw_filter_set()
   */
  oof_cb_cicp_lock(&cicp_lock_state);

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  while( ci_dllist_not_empty(&skf->sf_mcast_memberships) ) {
    mm = CI_CONTAINER(struct oof_mcast_member, mm_socket_link,
                      ci_dllist_pop(&skf->sf_mcast_memberships));
    if( mm->mm_filter != NULL )
      oof_mcast_remove(fm, mm, &mf_list);
    ci_dllist_push(&mm_list, &mm->mm_socket_link);
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);

  oof_mcast_filter_list_free(&mf_list);
  oof_mcast_member_list_free(&mm_list);
}


static void
oof_socket_mcast_install(struct oof_manager* fm, struct oof_socket* skf)
{
  struct tcp_helper_resource_s* skf_stack = oof_cb_socket_stack(skf);
  ci_irqlock_state_t lock_flags;
  ci_irqlock_state_t cicp_lock_state;
  struct oof_mcast_filter* mf;
  struct oof_mcast_member* mm;
  struct oof_local_port* lp;
  ci_dllist mcast_filters;
  int mf_needed, mf_n;

  /* Calculate how many new filters we'll need to install, and allocate
   * that many.  Slightly complex because we want to allocate with lock
   * dropped.
   */
  ci_dllist_init(&mcast_filters);
  mf_n = 0;
  
  /* Have to take the CICP lock here because we must take it before
   * the fm lock, and we might need it to convert ifindex into hwport_mask
   * for oo_hw_filter_set()
   */
  oof_cb_cicp_lock(&cicp_lock_state);

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  while( 1 ) {
    mf_needed = 0;
    if( (lp = skf->sf_local_port) != NULL ) {
      CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                          &skf->sf_mcast_memberships)
        if( mm->mm_filter == NULL &&
            OOF_NEED_MCAST_FILTER(skf, mm->mm_maddr) &&
            oof_local_port_find_mcast_filter(lp, skf_stack,
                                             mm->mm_maddr) == NULL )
          ++mf_needed;
    }
    if( mf_n >= mf_needed )
      break;
    ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
    oof_cb_cicp_unlock(&cicp_lock_state);

    do {
      if( (mf = CI_ALLOC_OBJ(struct oof_mcast_filter)) == NULL )
        goto out_of_memory;
      ci_dllist_push(&mcast_filters, &mf->mf_lp_link);
    } while( ++mf_n < mf_needed );

    oof_cb_cicp_lock(&cicp_lock_state);
    ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  }

  if( lp != NULL ) {
    CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                        &skf->sf_mcast_memberships) {
      if( mm->mm_filter == NULL ) {
        if( OOF_NEED_MCAST_FILTER(skf, mm->mm_maddr) )
          oof_mcast_install(fm, mm, &mcast_filters);
      }
      else {
        if( ! OOF_NEED_MCAST_FILTER(skf, mm->mm_maddr) )
          oof_mcast_remove(fm, mm, &mcast_filters);
      }
    }
  }

  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);

 out:
  oof_mcast_filter_list_free(&mcast_filters);
  return;

 out_of_memory:
  OO_DEBUG_ERR(ci_log(FSK_FMT "ERROR: out of memory", FSK_PRI_ARGS(skf)));
  goto out;
}


static void
oof_socket_mcast_remove(struct oof_manager* fm, struct oof_socket* skf,
                        ci_dllist* mcast_filters)
{
  struct oof_mcast_member* mm;

  ci_irqlock_check_locked(&fm->fm_lock);

  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                      &skf->sf_mcast_memberships) {
    ci_assert(mm->mm_socket == skf);
    ci_assert(CI_IP_IS_MULTICAST(mm->mm_maddr));
    if( mm->mm_filter != NULL )
      oof_mcast_remove(fm, mm, mcast_filters);
  }
}

/**********************************************************************
***********************************************************************
**********************************************************************/


/**********************************************************************
***********************************************************************
**********************************************************************/

static void
oof_socket_dump_w_lp(const char* pf, struct oof_manager* fm,
                     struct oof_socket* skf,
                     void (*log)(void* opaque, const char* fmt, ...),
                     void* loga)
{
  struct tcp_helper_resource_s* skf_stack = oof_cb_socket_stack(skf);
  struct oof_local_port* lp = skf->sf_local_port;
  struct oof_local_port_addr* lpa;
  struct oof_mcast_filter* mf;
  const char* state = NULL;
  int n_laddr, n_filter, n_mine;
  int la_i;

  ci_irqlock_check_locked(&fm->fm_lock);
  ci_assert(skf->sf_local_port != NULL);

  /* Work out whether the socket can receive any packets. */
  if( skf->sf_full_match_filter.trs != NULL ) {
    state = "ACCELERATED (full)";
  }
  else if( CI_IP_IS_MULTICAST(skf->sf_laddr) ) {
    CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                        &lp->lp_mcast_filters)
      if( mf->mf_maddr == skf->sf_laddr && mf->mf_filter.trs == skf_stack ) {
        if( oo_hw_filter_hwports(&mf->mf_filter) )
          state = "ACCELERATED (multicast laddr)";
        else
          state = "KERNEL (multicast laddr)";
        break;
      }
    if( state == NULL )
      /* Not done IP_ADD_MEMBERSHIP, so won't get packets. */
      state = "UNREACHABLE (need IP_ADD_MEMBERSHIP)";
  }
  else if( skf->sf_laddr ) {
    la_i = oof_manager_addr_find(fm, skf->sf_laddr);
    ci_assert(la_i >= 0 && la_i < fm->fm_local_addr_n);
    lpa = &lp->lp_addr[la_i];
    if( skf->sf_raddr ) {
      if( lpa->lpa_filter.trs == skf_stack )
        state = "ACCELERATED (sharing wild)";
      else if( lpa->lpa_filter.trs == NULL )
        state = "ORPHANED (no filter)";
      else
        state = "ORPHANED (filter points elsewhere)";
    }
    else {
      if( oof_wild_socket(lp, lpa) == skf ) {
        if( lpa->lpa_filter.trs == skf_stack )
          state = "ACCELERATED (wild)";
        else if( lpa->lpa_filter.trs == NULL )
          state = "FILTER_MISSING (not accelerated)";
        else
          state = "!! BAD_FILTER !!";
      }
      else
        state = "HIDDEN";
    }
  }
  else {
    n_laddr = n_filter = n_mine = 0;
    for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i )
      if( ci_dllist_not_empty(&fm->fm_local_addrs[la_i].la_active_ifs) ) {
        ++n_laddr;
        lpa = &lp->lp_addr[la_i];
        if( oof_wild_socket(lp, lpa) == skf )
          ++n_mine;
        if( lpa->lpa_filter.trs == skf_stack )
          ++n_filter;
      }
    if( n_laddr == 0 )
      state = "NO_LOCAL_ADDR";
    else if( n_filter < n_mine )
      state = "FILTERS_MISSING (may not be accelerated)";
    else if( n_mine == 0 )
      state = "HIDDEN";
    else if( n_mine < n_laddr )
      state = "PARTIALLY_HIDDEN";
    else
      state = "ACCELERATED";
  }

  log(loga, "%s: "SK_FMT" "SK_ADDR_FMT" %s", pf,
      SK_PRI_ARGS(skf), SK_ADDR_ARGS(skf), state);
}


void
oof_socket_dump(struct oof_manager* fm, struct oof_socket* skf,
                void (*log)(void* opaque, const char* fmt, ...),
                void* loga)
{
  ci_irqlock_state_t lock_flags;
  struct oof_mcast_member* mm;
  struct oof_mcast_filter* mf;
  unsigned hwports_got;

  ci_irqlock_lock(&fm->fm_lock, &lock_flags);
  if( skf->sf_local_port != NULL )
    oof_socket_dump_w_lp(__FUNCTION__, fm, skf, log, loga);
  CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_socket_link,
                      &skf->sf_mcast_memberships) {
    if( (mf = mm->mm_filter) == NULL ) {
      log(loga, "%s:   maddr="IP_FMT" if=%d hwports=%x NOT_BOUND",
          __FUNCTION__, IP_ARG(mm->mm_maddr), mm->mm_ifindex,
          mm->mm_hwport_mask);
    }
    else {
      hwports_got = oo_hw_filter_hwports(&mf->mf_filter);
      log(loga, "%s:   maddr="IP_FMT" if=%d hwports=%x,%x,%x %s", __FUNCTION__,
          IP_ARG(mm->mm_maddr), mm->mm_ifindex, mm->mm_hwport_mask,
          oof_mcast_filter_unconflicted_hwports(skf->sf_local_port, mf) &
            mm->mm_hwport_mask,
          hwports_got & mm->mm_hwport_mask, oof_mcast_member_state(mm));
    }
  }
  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
}


static void
oof_local_port_dump(struct oof_manager* fm, struct oof_local_port* lp,
                    void (*log)(void* opaque, const char* fmt, ...),
                    void* loga)
{
  unsigned hwports_got, hwports_uc;
  struct oof_local_port_addr* lpa;
  struct oof_mcast_filter* mf;
  struct oof_mcast_member* mm;
  struct oof_local_addr* la;
  struct oof_socket* skf;
  int la_i;

  log(loga, "%s: %s:%d n_refs=%d", __FUNCTION__,
      FMT_PROTOCOL(lp->lp_protocol), FMT_PORT(lp->lp_lport), lp->lp_refs);
  ci_irqlock_check_locked(&fm->fm_lock);

  if( ci_dllist_not_empty(&lp->lp_wild_socks) ) {
    log(loga, "  wild sockets:");
    CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link, &lp->lp_wild_socks)
      oof_socket_dump_w_lp("    ", fm, skf, log, loga);
  }

  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
    la = &fm->fm_local_addrs[la_i];
    lpa = &lp->lp_addr[la_i];
    if( lpa->lpa_filter.trs != NULL )
      log(loga, "  FILTER "IPPORT_FMT" hwports=%x stack=%d",
          IPPORT_ARG(la->la_laddr, lp->lp_lport),
          oo_hw_filter_hwports(&lpa->lpa_filter),
          oof_cb_stack_id(lpa->lpa_filter.trs));
    if( ci_dllist_not_empty(&lpa->lpa_semi_wild_socks) ) {
      log(loga, "  semi-wild sockets:");
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lpa->lpa_semi_wild_socks)
        oof_socket_dump_w_lp("    ", fm, skf, log, loga);
    }
    if( ci_dllist_not_empty(&lpa->lpa_full_socks) ) {
      log(loga, "  full-match sockets:");
      CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                          &lpa->lpa_full_socks)
        oof_socket_dump_w_lp("    ", fm, skf, log, loga);
    }
  }

  if( ci_dllist_not_empty(&lp->lp_mcast_filters) ) {
    log(loga, "  mcast filters:");
    CI_DLLIST_FOR_EACH2(struct oof_mcast_filter, mf, mf_lp_link,
                        &lp->lp_mcast_filters) {
      hwports_got = oo_hw_filter_hwports(&mf->mf_filter);
      hwports_uc = oof_mcast_filter_unconflicted_hwports(lp, mf);
      log(loga, "    maddr="IPPORT_FMT" stack=%d hwports=%x,%x,%x",
          IPPORT_ARG(mf->mf_maddr, lp->lp_lport),
          oof_cb_stack_id(mf->mf_filter.trs), mf->mf_hwport_mask,
          hwports_uc, hwports_got);
      CI_DLLIST_FOR_EACH2(struct oof_mcast_member, mm, mm_filter_link,
                          &mf->mf_memberships)
        log(loga, "      "SK_FMT" "SK_ADDR_FMT" if=%d hwports=%x,%x,%x %s",
            SK_PRI_ARGS(mm->mm_socket), SK_ADDR_ARGS(mm->mm_socket),
            mm->mm_ifindex, mm->mm_hwport_mask,
            hwports_uc & mm->mm_hwport_mask, hwports_got & mm->mm_hwport_mask,
            oof_mcast_member_state(mm));
    }
  }
}


void
oof_manager_dump(struct oof_manager* fm,
                void (*log)(void* opaque, const char* fmt, ...),
                void* loga)
{
  ci_irqlock_state_t cicp_lock_state;
  ci_irqlock_state_t lock_flags;
  struct oof_local_port* lp;
  struct oof_local_addr* la;
  struct oof_socket* skf;
  int la_i, hash;

  oof_cb_cicp_lock(&cicp_lock_state);
  ci_irqlock_lock(&fm->fm_lock, &lock_flags);

  log(loga, "%s: hwports up=%x unavailable=%x local_addr_n=%d", __FUNCTION__,
      oof_hwports_up, ~oof_hwports_available, fm->fm_local_addr_n);

  for( la_i = 0; la_i < fm->fm_local_addr_n; ++la_i ) {
    la = &fm->fm_local_addrs[la_i];
    if( la->la_laddr == 0 )
      continue;
    log(loga, "  "IP_FMT" active=%d sockets=%d", IP_ARG(la->la_laddr),
        ci_dllist_not_empty(&la->la_active_ifs), la->la_sockets);
  }

  if( ci_dllist_not_empty(&fm->fm_mcast_laddr_socks) ) {
    log(loga, "%s: sockets with laddr bound to multicast address:",
        __FUNCTION__);
    CI_DLLIST_FOR_EACH2(struct oof_socket, skf, sf_lp_link,
                        &fm->fm_mcast_laddr_socks)
      oof_socket_dump_w_lp("  ", fm, skf, log, loga);
  }

  for( hash = 0; hash < OOF_LOCAL_PORT_TBL_SIZE; ++hash )
    CI_DLLIST_FOR_EACH2(struct oof_local_port, lp, lp_manager_link,
                        &fm->fm_local_ports[hash])
      oof_local_port_dump(fm, lp, log, loga);

  ci_irqlock_unlock(&fm->fm_lock, &lock_flags);
  oof_cb_cicp_unlock(&cicp_lock_state);
}