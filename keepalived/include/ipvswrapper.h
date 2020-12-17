/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        ipvswrapper.c include file.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#ifndef _IPVSWRAPPER_H
#define _IPVSWRAPPER_H

#include "config.h"

#ifdef _WITH_VRRP_
  #include "vrrp.h"
#endif

#include "libipvs.h"
#include "check_data.h"

#define IPVS_ERROR	0
#define IPVS_SUCCESS	1

#define IPVS_STARTDAEMON	IP_VS_SO_SET_STARTDAEMON
#define IPVS_STOPDAEMON		IP_VS_SO_SET_STOPDAEMON
#define IPVS_FLUSH		IP_VS_SO_SET_FLUSH
#define IPVS_MASTER		IP_VS_STATE_MASTER
#define IPVS_BACKUP		IP_VS_STATE_BACKUP
#define	IPVS_MASTER_BACKUP	(IP_VS_STATE_MASTER | IP_VS_STATE_BACKUP)

#define IPVS_DEF_SCHED		"wlc"

struct lvs_syncd_config {
	const char			*ifname;	/* handle LVS sync daemon state using this */
#ifdef _WITH_VRRP_
	vrrp_t				*vrrp;		/* instance FSM & running on specific interface */
	const char			*vrrp_name;	/* used during configuration and SNMP */
#endif
	unsigned			syncid;		/* 0 .. 255, or PARAMETER_UNSET if not configured */
#ifdef _HAVE_IPVS_SYNCD_ATTRIBUTES_
	uint16_t			sync_maxlen;
	struct sockaddr_storage		mcast_group;
	uint16_t			mcast_port;
	uint8_t				mcast_ttl;
#endif
	bool				daemon_set_reload;
};

/* prototypes */
extern int ipvs_start(void);
extern void ipvs_stop(void);
extern void ipvs_set_timeouts(const ipvs_timeout_t *);
extern void ipvs_flush_cmd(void);
extern virtual_server_group_t *ipvs_get_group_by_name(const char *, list_head_t *) __attribute__ ((pure));
extern void ipvs_group_sync_entry(virtual_server_t *vs, virtual_server_group_entry_t *vsge);
extern void ipvs_group_remove_entry(virtual_server_t *, virtual_server_group_entry_t *);
extern int ipvs_cmd(int, virtual_server_t *, real_server_t *);
extern bool ipvs_syncd_changed(const struct lvs_syncd_config *, const struct lvs_syncd_config *) __attribute__((pure));
extern void ipvs_syncd_cmd(int, const struct lvs_syncd_config *, int, bool);
#ifdef _WITH_VRRP_
extern void ipvs_syncd_master(const struct lvs_syncd_config *);
extern void ipvs_syncd_backup(const struct lvs_syncd_config *);
#endif

/* Refresh statistics at most every 5 seconds */
#define STATS_REFRESH 5
extern void ipvs_update_stats(virtual_server_t * vs);

#endif
