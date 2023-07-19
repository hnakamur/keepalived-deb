/*
 *      IP Virtual Server
 *      data structure and functionality definitions
 */

#ifndef _KEEPALIVED_IP_VS_H
#define _KEEPALIVED_IP_VS_H

#include "config.h"

/* System includes */
#include <net/if.h>	/* Force inclusion of net/if.h before linux/if.h */
#include <sys/types.h>
#include <netinet/in.h>
#include <linux/ip_vs.h>
/* Prior to Linux 4.2 have to include linux/in.h and linux/in6.h
 * or linux/netlink.h to include linux/netfilter.h */
#include <linux/netfilter.h>	/* For nf_inet_addr */
#include <stdint.h>

/* The kernel's valid values for a real server weight are 0..INT32_MAX
 * We reserve +/- INT32_MAX for fault state. */
#define IPVS_WEIGHT_MAX		INT32_MAX
#define IPVS_WEIGHT_LIMIT	(IPVS_WEIGHT_MAX)
#define IPVS_WEIGHT_FAULT	(-IPVS_WEIGHT_MAX - 1)

#define IPVS_FWMARK_MAX		UINT32_MAX

#ifdef _WITH_LVS_64BIT_STATS_
struct ip_vs_stats64 {
	__u64	conns;		/* connections scheduled */
	__u64	inpkts;		/* incoming packets */
	__u64	outpkts;	/* outgoing packets */
	__u64	inbytes;	/* incoming bytes */
	__u64	outbytes;	/* outgoing bytes */

	__u64	cps;		/* current connection rate */
	__u64	inpps;		/* current in packet rate */
	__u64	outpps;		/* current out packet rate */
	__u64	inbps;		/* current in byte rate */
	__u64	outbps;		/* current out byte rate */
};
typedef struct ip_vs_stats64 ip_vs_stats_t;
#else
typedef struct ip_vs_stats_user ip_vs_stats_t;
#endif

struct ip_vs_service_app {
	struct ip_vs_service_user user;
	uint16_t		af;
	union nf_inet_addr	nf_addr;
	char			pe_name[IP_VS_PENAME_MAXLEN + 1];
};

struct ip_vs_dest_app {
	struct ip_vs_dest_user	user;
	uint16_t		af;
	union nf_inet_addr	nf_addr;
#ifdef _HAVE_IPVS_TUN_TYPE_
	int			tun_type;
	int			tun_port;
#ifdef _HAVE_IPVS_TUN_CSUM_
	int			tun_flags;
#endif
#endif
};


struct ip_vs_service_entry_app {
	struct ip_vs_service_entry user;
	ip_vs_stats_t		stats;
	uint16_t		af;
	union nf_inet_addr	nf_addr;
	char			pe_name[IP_VS_PENAME_MAXLEN + 1];
};

struct ip_vs_dest_entry_app {
	struct ip_vs_dest_entry user;
	ip_vs_stats_t		stats;
	uint16_t		af;
	union nf_inet_addr	nf_addr;

};

struct ip_vs_get_dests_app {
	uint16_t		af;
	union nf_inet_addr	nf_addr;

	struct {
	/* which service: user fills in these */
	__u16			protocol;
	__be32			addr;		/* virtual address */
	__be16			port;
	__u32			fwmark;		/* firwall mark of service */

	/* number of real servers */
	unsigned int		num_dests;

	/* the real servers */
	struct ip_vs_dest_entry_app	entrytable[];
	} user;
};

/* The argument to IP_VS_SO_GET_SERVICES */
struct ip_vs_get_services_app {
	struct {
	/* number of virtual services */
	unsigned int		num_services;

	/* service table */
	struct ip_vs_service_entry_app entrytable[0];
	} user;
};

/* Make sure we don't have an inconsistent definition */
#if IP_VS_IFNAME_MAXLEN > IFNAMSIZ
	#error The code assumes that IP_VS_IFNAME_MAXLEN <= IFNAMSIZ
#endif

/* The argument to IP_VS_SO_GET_DAEMON */
struct ip_vs_daemon_kern {
	/* sync daemon state (master/backup) */
	int			state;

	/* multicast interface name */
	char			mcast_ifn[IP_VS_IFNAME_MAXLEN];

	/* SyncID we belong to */
	int			syncid;
};

struct ip_vs_daemon_app {
	/* sync daemon state (master/backup) */
	int			state;

	/* multicast interface name */
	char			mcast_ifn[IP_VS_IFNAME_MAXLEN];

	/* SyncID we belong to */
	int			syncid;

#ifdef _HAVE_IPVS_SYNCD_ATTRIBUTES_
	/* UDP Payload Size */
	uint16_t		sync_maxlen;

	/* Multicast Port (base) */
	uint16_t		mcast_port;

	/* Multicast TTL */
	uint8_t			mcast_ttl;

	/* Multicast Address Family */
	uint16_t		mcast_af;

	/* Multicast Address */
	union nf_inet_addr	mcast_group;
#endif
};

#endif	/* _KEEPALIVED_IP_VS_H */
