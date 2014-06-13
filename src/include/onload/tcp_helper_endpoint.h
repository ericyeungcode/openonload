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

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author Andrew Rybchenko <Andrew.Rybchenko@oktetlabs.ru>
**  \brief TCP Helper endpoint definitions
**   \date Started at Jul, 29 2004
**    \cop (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_include_ci_driver_efab  */

#ifndef __CI_DRIVER_EFAB_TCP_HELPER_ENDPOINT_H__
#define __CI_DRIVER_EFAB_TCP_HELPER_ENDPOINT_H__

#include <ci/driver/internal.h>
#include <ci/internal/cplane_handle.h>
#include <onload/tcp_helper.h>
#include <onload/oof_hw_filter.h>


extern int efab_tcp_driver_ctor(unsigned max_macs,
                                unsigned max_layer2_interfaces, 
                                unsigned max_routes);
extern void efab_tcp_driver_dtor(void);

ci_inline int
IS_EP_ADDR_SPC_VALID(tcp_helper_endpoint_t * ep)
{
    return ( (ep->addr_spc != CI_ADDR_SPC_INVALID)  &&
             (ep->addr_spc != CI_ADDR_SPC_KERNEL) );
}


ci_inline struct efrm_vi* tcp_helper_rx_vi_rs(tcp_helper_resource_t* thr,
                                                  int intf_i) {
  return thr->nic[intf_i].vi_rs;
}


/*--------------------------------------------------------------------
*
*
*--------------------------------------------------------------------*/

/*!
 * Constructor of the TCP Helper per endpoint data.
 *
 * \param ep	Pointer to endpoint data storage
 * \param thr   Parent TCP helper resource
 * \param id	Endpoint ID
 */
extern void tcp_helper_endpoint_ctor(tcp_helper_endpoint_t *ep, 
                                     tcp_helper_resource_t * thrm, 
                                     int id);

/*!
 * Destructor of the TCP Helper per endpoint data.
 *
 * \param ep	pointer to endpoint data storage
 */
extern void tcp_helper_endpoint_dtor(tcp_helper_endpoint_t *ep);

/*--------------------------------------------------------------------
 *!
 * Called by TCP/IP stack to setup all the filters needed for a 
 * TCP/UDP endpoint. This includes
 *    - hardware IP filters
 *    - filters in the software connection hash table
 *    - filters for NET to CHAR driver comms to support fragments
 *
 * \param ep              endpoint kernel data structure 
 * \param bindto_ifindex  ifindex for SO_BINDTODEVICE
 *                        (ignored unless raddr/rport = 0/0)
 * \param from_tcp_id     block id of listening socket to "borrow" filter from
 *                        (-1 if not required)
 *
 * \return                standard error codes
 *
 *--------------------------------------------------------------------*/

extern int 
tcp_helper_endpoint_set_filters(tcp_helper_endpoint_t* ep,
                                ci_ifid_t bindto_ifindex, oo_sp from_tcp_id);

/*--------------------------------------------------------------------
 *!
 * Clear all filters for an endpoint
 *
 * \param ep              endpoint kernel data structure 
 *
 * \return                standard error codes
 *
 *--------------------------------------------------------------------*/

extern int
tcp_helper_endpoint_clear_filters(tcp_helper_endpoint_t* ep, int no_sw);

/*--------------------------------------------------------------------
 *!
 * Debug op into filter code
 *
 * \param thr             TCP helper resource
 * \param sockp           ID of endpoint
 *
 * \return                standard error codes
 *
 *--------------------------------------------------------------------*/

extern int
tcp_helper_endpoint_filter_dump(tcp_helper_resource_t*, oo_sp sockp,
                                void* buf, int buf_len);

/*--------------------------------------------------------------------
 *!
 * Shutdown endpoint socket
 *
 * \param thr             TCP helper resource
 * \param ep_id           ID of endpoint
 * \param how             How to shutdown the socket
 *
 * \return                standard error codes
 *
 *--------------------------------------------------------------------*/

extern int
tcp_helper_endpoint_shutdown(tcp_helper_resource_t*, oo_sp, int how,
                             ci_uint32 old_state);


extern void
tcp_helper_endpoint_wakeup(tcp_helper_resource_t*, tcp_helper_endpoint_t*);


#endif /* __CI_DRIVER_EFAB_TCP_HELPER_ENDPOINT_H__ */
/*! \cidoxg_end */