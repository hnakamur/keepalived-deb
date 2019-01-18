/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        vrrp_nftables.c
 *
 * Author:      Quentin Armitage, <quentin@armitage.org.uk>
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
 * Copyright (C) 2001-2018 Alexandre Cassen, <acassen@gmail.com>
 */

#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
#include <netinet/in.h>

#include <net/if.h>
#ifdef _HAVE_NET_LINUX_IF_H_COLLISION_
#define _LINUX_IF_H
#endif
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>

#include <libmnl/libmnl.h>
#include <libnftnl/table.h>
#include <libnftnl/chain.h>
#include <libnftnl/set.h>
#include <libnftnl/rule.h>
#include <libnftnl/expr.h>

#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#include <errno.h>

#include "vrrp_nftables.h"
#include "logger.h"
#include "vrrp.h"
#include "vrrp_ipaddress.h"
#include "global_data.h"
#include "list.h"


/* The following are from nftables source code (include/datatype.h)
 * and are used for it to determine how to display the entries in
 * the set. */
#define TYPE_IPADDR		7
#define TYPE_IP6ADDR		8
#define TYPE_IFINDEX		20
#define TYPE_ICMPV6_TYPE	29
#define TYPE_IFNAME		41

#define TYPE_BITS               6
#define TYPE_MASK               ((1 << TYPE_BITS) - 1)

static struct mnl_socket *nl;
static uint32_t seq;

static bool ipv4_table_setup;
static bool ipv6_table_setup;
static bool setup_ll_ifname;
static bool setup_ll_ifindex;

#ifdef INCLUDE_UNUSED_CODE
static int
table_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = (const struct nlattr **)data;

	tb[attr->nla_type & NLA_TYPE_MASK] = attr;
	return MNL_CB_OK;
}

static void
new_table(const struct nlmsghdr *nlh)
{
	struct nlattr *tb[NFTA_TABLE_MAX+1] = {};
	struct nfgenmsg *nfg = mnl_nlmsg_get_payload(nlh);

	if (mnl_attr_parse(nlh, sizeof(*nfg), table_cb, tb) < 0) {
		log_message(LOG_INFO, "table parse failed");
		return;
	}

	if (tb[NFTA_TABLE_NAME] && tb[NFTA_TABLE_HANDLE])
		log_message(LOG_INFO, "Table %s: handle %lu", mnl_attr_get_str(tb[NFTA_TABLE_NAME]), be64toh(mnl_attr_get_u64(tb[NFTA_TABLE_HANDLE])));
}

static int
cb_func(const struct nlmsghdr *nlh, void *data)
{
	if (NFNL_SUBSYS_ID(nlh->nlmsg_type) != NFNL_SUBSYS_NFTABLES)
		return 1;
	switch NFNL_MSG_TYPE(nlh->nlmsg_type) {
		case NFT_MSG_NEWTABLE: log_message(LOG_INGO, "%s", "NFT_MSG_NEWTABLE"); new_table(nlh);break;
		case NFT_MSG_NEWCHAIN: log_message(LOG_INGO, "%s", "NFT_MSG_NEWCHAIN"); break;
		case NFT_MSG_NEWSET: log_message(LOG_INGO, "%s", "NFT_MSG_NEWSET"); break;
		case NFT_MSG_NEWRULE: log_message(LOG_INGO, "%s", "NFT_MSG_NEWRULE"); break;
		case NFT_MSG_NEWSETELEM: log_message(LOG_INGO, "%s", "NFT_MSG_NEWSETELEM"); break;
		default: log_message(LOG_INGO, "Unknown msg type"); break;
	}

	return 1;
}
#endif

static void
exchange_nl_msg(struct mnl_nlmsg_batch *batch)
{
	int ret;
	uint32_t portid;
	char buf[MNL_SOCKET_BUFFER_SIZE];

	if (mnl_nlmsg_batch_is_empty(batch))
		return;

	/* mnl_nlmsg_fprintf(fp, (char *)mnl_nlmsg_batch_head(batch), mnl_nlmsg_batch_size(batch), sizeof( struct nfgenmsg)); */
	if (!nl) {
		nl = mnl_socket_open(NETLINK_NETFILTER);
		if (nl == NULL) {
			log_message(LOG_INFO, "mnl_socket_open failed - %d", errno);
			return;
		}

		if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
			log_message(LOG_INFO, "mnl_socket_bind error - %d", errno);
			return;
		}
	}
	portid = mnl_socket_get_portid(nl);

	if (mnl_socket_sendto(nl, mnl_nlmsg_batch_head(batch),
			      mnl_nlmsg_batch_size(batch)) < 0) {
		log_message(LOG_INFO, "mnl_socket_send error - %d", errno);
		return;
	}

	ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	while (ret > 0) {
		/* ret = mnl_cb_run(buf, ret, 0, portid, cb_func, NULL); */
		ret = mnl_cb_run(buf, ret, 0, portid, NULL, NULL);
		if (ret <= 0)
			break;
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	}
	if (ret == -1) {
		log_message(LOG_INFO, "mnl_socket_recvfrom error - %d", errno);
		return;
	}
}

static bool
my_mnl_nlmsg_batch_next(struct mnl_nlmsg_batch *batch)
{
	if (!mnl_nlmsg_batch_next(batch)) {
		exchange_nl_msg(batch);
		mnl_nlmsg_batch_reset(batch);
	}

	return true;
}

static void
add_payload(struct nftnl_rule *r, uint32_t base, uint32_t dreg,
			uint32_t offset, uint32_t len)
{
	struct nftnl_expr *e;

	e = nftnl_expr_alloc("payload");
	if (e == NULL) {
		log_message(LOG_INFO, "expr payload oom error - %d", errno);
		return;
	}

	nftnl_expr_set_u32(e, NFTNL_EXPR_PAYLOAD_BASE, base);
	nftnl_expr_set_u32(e, NFTNL_EXPR_PAYLOAD_DREG, dreg);
	nftnl_expr_set_u32(e, NFTNL_EXPR_PAYLOAD_OFFSET, offset);
	nftnl_expr_set_u32(e, NFTNL_EXPR_PAYLOAD_LEN, len);

	nftnl_rule_add_expr(r, e);
}

static void
add_meta(struct nftnl_rule *r, uint32_t ifindex, uint32_t dreg)
{
	struct nftnl_expr *e;

	e = nftnl_expr_alloc("meta");
	if (e == NULL) {
		log_message(LOG_INFO, "expr payload oom error - %d", errno);
		return;
	}

	nftnl_expr_set_u32(e, NFTNL_EXPR_META_DREG, dreg);
	nftnl_expr_set_u32(e, NFTNL_EXPR_META_KEY, ifindex);

	nftnl_rule_add_expr(r, e);
}

static void
add_lookup(struct nftnl_rule *r, uint32_t base, const char *set_name,
			uint32_t set_id,
#ifndef HAVE_NFTNL_EXPR_LOOKUP_FLAGS
			__attribute__((unused))
#endif
						bool neg)
{
	struct nftnl_expr *e;

	e = nftnl_expr_alloc("lookup");
	if (e == NULL) {
		log_message(LOG_INFO, "expr lookup oom error - %d", errno);
		return;
	}

	nftnl_expr_set_u32(e, NFTNL_EXPR_LOOKUP_SREG, base);
#ifdef HAVE_NFTNL_EXPR_LOOKUP_FLAGS
	if (neg)
		nftnl_expr_set_u32(e, NFTNL_EXPR_LOOKUP_FLAGS, NFT_LOOKUP_F_INV);
#endif
	nftnl_expr_set_str(e, NFTNL_EXPR_LOOKUP_SET, set_name);
	if (set_id)
		nftnl_expr_set_u32(e, NFTNL_EXPR_LOOKUP_SET_ID, set_id);

	nftnl_rule_add_expr(r, e);
}

/* verdict shoud be NF_DROP, NF_ACCEPT, NFT_RETURN, ... */
/* "The nf_tables verdicts share their numeric space with the netfilter verdicts." */
static void
add_immediate(struct nftnl_rule *r, uint32_t verdict, const char *chain)
{
	struct nftnl_expr *e;

	e = nftnl_expr_alloc("immediate");
	if (e == NULL) {
		log_message(LOG_INFO, "expr immediate oom error - %d", errno);
		return;
	}

	nftnl_expr_set_u32(e, NFTNL_EXPR_IMM_DREG, NFT_REG_VERDICT);
	if (chain)
		nftnl_expr_set_str(e, NFTNL_EXPR_IMM_CHAIN, chain);
	nftnl_expr_set_u32(e, NFTNL_EXPR_IMM_VERDICT, verdict);

	nftnl_rule_add_expr(r, e);
}

static void
add_cmp(struct nftnl_rule *r, uint32_t sreg, uint32_t op,
		    const void *data, uint32_t data_len)
{
	struct nftnl_expr *e;

	e = nftnl_expr_alloc("cmp");
	if (e == NULL) {
		log_message(LOG_INFO, "expr cmp oom error - %d", errno);
		return;
	}

	nftnl_expr_set_u32(e, NFTNL_EXPR_CMP_SREG, sreg);
	nftnl_expr_set_u32(e, NFTNL_EXPR_CMP_OP, op);
	nftnl_expr_set(e, NFTNL_EXPR_CMP_DATA, data, data_len);

	nftnl_rule_add_expr(r, e);
}

static void
add_counter(struct nftnl_rule *r)
{
	struct nftnl_expr *e;

	if (!global_data->vrrp_nf_counters)
		return;

	e = nftnl_expr_alloc("counter");
	if (e == NULL) {
		log_message(LOG_INFO, "expr counter oom error - %d", errno);
		return;
	}

	nftnl_rule_add_expr(r, e);
}

static struct nftnl_table
*table_add_parse(uint16_t family, char *table)
{
	struct nftnl_table *t;

	t = nftnl_table_alloc();
	if (t == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_table_set_u32(t, NFTNL_TABLE_FAMILY, family);
	nftnl_table_set_str(t, NFTNL_TABLE_NAME, table);

	return t;
}

static struct
nftnl_chain *chain_add_parse(char *table, char *name)
{
	struct nftnl_chain *t;

	t = nftnl_chain_alloc();
	if (t == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}
	nftnl_chain_set(t, NFTNL_CHAIN_TABLE, table);
	nftnl_chain_set(t, NFTNL_CHAIN_NAME, name);

	return t;
}

/* For an anonymous set use set name "__set%d", and retrieve set_id with:
        set_id = nftnl_set_get_u32(s, NFTNL_SET_ID);
 *
 * To add a rule referencing the set, setname is "__set%d", and set set_id:
	if (set_id)
		nftnl_expr_set_u32(e, NFTNL_EXPR_LOOKUP_SET_ID, set_id);

 * It works similarly for maps
*/
static struct
nftnl_set *setup_set(uint8_t family, const char *table,
				 const char *name, int type)
{
	struct nftnl_set *s = NULL;
	static int set_id = 0;
	int type_copy = type;
	int size = 0;

	s = nftnl_set_alloc();
	if (s == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	while (type_copy) {
		switch (type_copy & TYPE_MASK)
		{
		case TYPE_IPADDR:
			size += sizeof(struct in_addr);
			break;
		case TYPE_IP6ADDR:
			size += sizeof(struct in6_addr);
			break;
		case TYPE_IFINDEX:
			size += sizeof(uint32_t);
			break;
		case TYPE_ICMPV6_TYPE:
			size++;
			break;
		case TYPE_IFNAME:
			size += IFNAMSIZ;
			break;
		default:
			log_message(LOG_INFO, "Unsupported type %d\n", type_copy & TYPE_MASK);
			break;
		}
		type_copy >>= TYPE_BITS;
	}

	nftnl_set_set_str(s, NFTNL_SET_TABLE, table);
	nftnl_set_set_str(s, NFTNL_SET_NAME, name);
	nftnl_set_set_u32(s, NFTNL_SET_FAMILY, family);
	nftnl_set_set_u32(s, NFTNL_SET_KEY_LEN, size);
	/* inet service type, see nftables/include/datatypes.h */
	nftnl_set_set_u32(s, NFTNL_SET_KEY_TYPE, type);
	nftnl_set_set_u32(s, NFTNL_SET_ID, ++set_id);

	return s;
}

static struct
nftnl_rule *setup_rule(uint8_t family, const char *table,
		   const char *chain, const char *handle,
		   const char *set, bool saddr, uint32_t verdict, bool neg)
{
	struct nftnl_rule *r = NULL;
	uint64_t handle_num;

	r = nftnl_rule_alloc();
	if (r == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_rule_set(r, NFTNL_RULE_TABLE, table);
	nftnl_rule_set(r, NFTNL_RULE_CHAIN, chain);
	nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, family);

	if (handle != NULL) {
		handle_num = atoll(handle);
		nftnl_rule_set_u64(r, NFTNL_RULE_POSITION, handle_num);
	}

	/* Use nft --debug mnl to see the netlink message for an nft command.
	 * mnl_nlmsg_fprintf is the function that prints it if
	 * we want to view what we have constructed
	 *
	 * The indentation is added to show the nesting. To indent a nested block,
	 * the number of lines to indent is (length / 4 - 1).
	 *
	----------------	------------------
	|  0000000020  |	| message length |
	| 00016 | R--- |	|  type | flags  |	NFNL_MSG_BATCH_BEGIN | REQUEST
	|  0000000003  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |	family = AF_UNSPEC, version = NFNETLINK_V0 , res_id = NFNL_SUBSYS_NFTABLES
	----------------	------------------
	----------------	------------------
	|  0000000208  |	| message length |
	| 02566 | R--- |	|  type | flags  |	NEWRULE | REQUEST
	|  0000000004  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	|00011|--|00001|	|len |flags| type|	NFTA_RULE_TABLE	nftnl_rule_set_str(r, NFTNL_RULE_TABLE, str);
	| 66 69 6c 74  |	|      data      |	 f i l t
	| 65 72 00 00  |	|      data      |	 e r
	|00018|--|00002|	|len |flags| type|	NFTA_RULE_CHAIN
	| 6b 65 65 70  |	|      data      |	 k e e p
	| 61 6c 69 76  |	|      data      |	 a l i v
	| 65 64 5f 69  |	|      data      |	 e d _ i
	| 6e 00 00 00  |	|      data      |	 n
	|00156|N-|00004|	|len |flags| type|	NFT_RULE_EXPRESSIONS	(see nftnl_rule_nlmsg_build_payload, netlink_gen_expr)
	  |00052|N-|00001|	|len |flags| type|		NFTA_LIST_ELEM | NEST (to add - nftnl_rule_add_expr)
	    |00012|--|00001|	|len |flags| type|	NFTA_EXPR_NAME	(see netlink_gen_payload)
	    | 70 61 79 6c  |	|      data      |	 p a y l
	    | 6f 61 64 00  |	|      data      |	 o a d
	    |00036|N-|00002|	|len |flags| type| 	NFTA_EXPR_DATA | NEST
	      |00008|--|00001|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_DREG
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00002|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_BASE
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00003|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_OFFSET
	      | 00 00 00 10  |	|      data      |
	      |00008|--|00004|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_LEN
	      | 00 00 00 04  |	|      data      |
	  |00052|N-|00001|	|len |flags| type| 		NFTA_LIST_ELEM | NEST (netlink_gen_set_stmt)
	    |00011|--|00001|	|len |flags| type|	NFTA_EXPR_NAME	(see netlink_gen_lookup)
	    | 6c 6f 6f 6b  |	|      data      |	 l o o k
	    | 75 70 00 00  |	|      data      |	 u p
	    |00036|N-|00002|	|len |flags| type|	NFTA_EXPR_DATA | NEST
	      |00008|--|00002|	|len |flags| type|	NFTNL_EXPR_LOOKUP_SREG
	      | 00 00 00 01  |	|      data      |
	      |00015|--|00001|	|len |flags| type|	NFTNL_EXPR_LOOKUP_SET
	      | 6b 65 65 70  |	|      data      |	 k e e p
	      | 61 6c 69 76  |	|      data      |	 a l i v
	      | 65 64 00 00  |	|      data      |	 e d
	      |00008|--|00004|	|len |flags| type|	NFTNL_EXPR_LOOKUP_SET_ID
	      | 00 00 00 01  |	|      data      |
	  |00048|N-|00001|	|len |flags| type|		NFTA_LIST_ELEM | NEST (netlink_get_verdict_stmt from netlink_gen_stmt)
	    |00014|--|00001|	|len |flags| type|	NFTA_EXPR_NAME (see netlink_gen_immediate)
	    | 69 6d 6d 65  |	|      data      |	 i m m e
	    | 64 69 61 74  |	|      data      |	 d i a t
	    | 65 00 00 00  |	|      data      |	 e
	    |00028|N-|00002|	|len |flags| type|	NFTA_EXPR_DATA | NEST
	      |00008|--|00001|	|len |flags| type| NFTNL_EXPR_IMM_DREG
	      | 00 00 00 00  |	|      data      |
	      |00016|N-|00002|	|len |flags| type| NFTNL_EXPR_IMM_VERDICT
	        |00012|N-|00002|	|len |flags| type|
	          |00008|--|00001|	|len |flags| type|
	          | 00 00 00 00  |	|      data      |
	----------------	------------------
	----------------	------------------
	|  0000000020  |	| message length |
	| 00017 | R--- |	|  type | flags  |	NFNL_MSG_BATCH_END | REQUEST
	|  0000000005  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |	family = AF_UNSPEC, version = NFNETLINK_V0 , res_id = NFNL_SUBSYS_NFTABLES
	----------------	------------------
	*/
	if (family == NFPROTO_IPV4)
		add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
			    saddr ? offsetof(struct iphdr, saddr) : offsetof(struct iphdr, daddr), sizeof(uint32_t));
	else
		add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
			    saddr ? offsetof(struct ip6_hdr, ip6_src) : offsetof(struct ip6_hdr, ip6_dst), sizeof(struct in6_addr));

	add_lookup(r, NFT_REG_1, set, 0, neg);

	add_counter(r);

	add_immediate(r, verdict, NULL);

	return r;
}

static struct
nftnl_rule *setup_rule_if(uint8_t family, const char *table,
				   const char *chain, const char *handle,
				   const char *set, bool saddr, bool use_name, uint32_t verdict, bool neg)
{
	struct nftnl_rule *r = NULL;
	uint64_t handle_num;

	/*
	----------------	------------------
	|  0000000264  |	| message length |
	| 02566 | R--- |	|  type | flags  |
	|  0000000004  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 0a 00 00 00  |	|  extra header  |
	|00015|--|00001|	|len |flags| type|
	| 6b 65 65 70  |	|      data      |	 k e e p
	| 61 6c 69 76  |	|      data      |	 a l i v
	| 65 64 00 00  |	|      data      |	 e d
	|00018|--|00002|	|len |flags| type|
	| 69 6e 5f 6c  |	|      data      |	 i n _ l
	| 69 6e 6b 5f  |	|      data      |	 i n k _
	| 6c 6f 63 61  |	|      data      |	 l o c a
	| 6c 00 00 00  |	|      data      |	 l
	|00208|N-|00004|	|len |flags| type| NFT_RULE_EXPRESSIONS
	  |00052|N-|00001|	|len |flags| type| NFTA_LIST_ELEM
	    |00012|--|00001|	|len |flags| type| NFTA_EXPR_NAME
	      | 70 61 79 6c  |	|      data      |	 p a y l
	      | 6f 61 64 00  |	|      data      |	 o a d
	    |00036|N-|00002|	|len |flags| type| NFTA_EXPR_DATA
	      |00008|--|00001|	|len |flags| type| DREG
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00002|	|len |flags| type| BASE
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00003|	|len |flags| type| OFFSET
	      | 00 00 00 18  |	|      data      |
	      |00008|--|00004|	|len |flags| type| LEN
	      | 00 00 00 10  |	|      data      |
	  |00036|N-|00001|	|len |flags| type| NFTA_LIST_ELEM
	    |00009|--|00001|	|len |flags| type| NFTA_EXPR_NAME
	    | 6d 65 74 61  |	|      data      |	 m e t a
	    | 00 00 00 00  |	|      data      |
	    |00020|N-|00002|	|len |flags| type| NFTA_EXPR_DATA
	      |00008|--|00002|	|len |flags| type| NFTA_META_KEY
	      | 00 00 00 06  |	|      data      | NFT_META_IIFNAME
	      |00008|--|00001|	|len |flags| type| NFTA_META_DREG
	      | 00 00 00 02  |	|      data      |NFT_REG_2
	  |00048|N-|00001|	|len |flags| type|
	    |00011|--|00001|	|len |flags| type|
	    | 6c 6f 6f 6b  |	|      data      |	 l o o k
	    | 75 70 00 00  |	|      data      |	 u p
	    |00032|N-|00002|	|len |flags| type|
	      |00008|--|00002|	|len |flags| type|
	      | 00 00 00 01  |	|      data      |
	      |00010|--|00001|	|len |flags| type|
	      | 69 66 5f 6c  |	|      data      |	 i f _ l
	      | 6c 00 00 00  |	|      data      |	 l
	      |00008|--|00004|	|len |flags| type|
	      | 00 00 00 06  |	|      data      |
	  |00020|N-|00001|	|len |flags| type|
	    |00012|--|00001|	|len |flags| type|
	    | 63 6f 75 6e  |	|      data      |	 c o u n
	    | 74 65 72 00  |	|      data      |	 t e r
	    |00004|N-|00002|	|len |flags| type|
	  |00048|N-|00001|	|len |flags| type|
	    |00014|--|00001|	|len |flags| type|
	    | 69 6d 6d 65  |	|      data      |	 i m m e
	    | 64 69 61 74  |	|      data      |	 d i a t
	    | 65 00 00 00  |	|      data      |	 e
	    |00028|N-|00002|	|len |flags| type|
	      |00008|--|00001|	|len |flags| type|
	      | 00 00 00 00  |	|      data      |
	      |00016|N-|00002|	|len |flags| type|
	        |00012|N-|00002|	|len |flags| type|
	          |00008|--|00001|	|len |flags| type|
	          | 00 00 00 00  |	|      data      |
	----------------	------------------
	*/
	r = nftnl_rule_alloc();
	if (r == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_rule_set(r, NFTNL_RULE_TABLE, table);
	nftnl_rule_set(r, NFTNL_RULE_CHAIN, chain);
	nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, family);

	if (handle != NULL) {
		handle_num = atoll(handle);
		nftnl_rule_set_u64(r, NFTNL_RULE_POSITION, handle_num);
	}

	if (family == NFPROTO_IPV4)
		add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
			    saddr ? offsetof(struct iphdr, saddr) : offsetof(struct iphdr, daddr), sizeof(uint32_t));
	else
		add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
			    saddr ? offsetof(struct ip6_hdr, ip6_src) : offsetof(struct ip6_hdr, ip6_dst), sizeof(struct in6_addr));

	if (saddr)
		add_meta(r, use_name ? NFT_META_OIFNAME : NFT_META_OIF, NFT_REG_2);
	else
		add_meta(r, use_name ? NFT_META_IIFNAME : NFT_META_IIF, NFT_REG_2);

	add_lookup(r, NFT_REG_1, set, 0, neg);

	add_counter(r);

	add_immediate(r, verdict, NULL);

	return r;
}

static struct nftnl_rule
*setup_rule_range_goto(uint8_t family, const char *table,
				   const char *chain, const char *handle,
				   const char *chain_dest, bool saddr)
{
	struct nftnl_rule *r = NULL;
	uint64_t handle_num;
	struct in6_addr ip6;

	r = nftnl_rule_alloc();
	if (r == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_rule_set(r, NFTNL_RULE_TABLE, table);
	nftnl_rule_set(r, NFTNL_RULE_CHAIN, chain);
	nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, family);

	/*
	----------------	------------------
	|  0000000020  |	| message length |
	| 00016 | R--- |	|  type | flags  |
	|  0000000003  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |
	----------------	------------------
	----------------	------------------
	|  0000000292  |	| message length |
	| 02566 | R--- |	|  type | flags  |
	|  0000000004  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 0a 00 00 00  |	|  extra header  |
	|00011|--|00001|	|len |flags| type|
	| 66 69 6c 74  |	|      data      |	 f i l t
	| 65 72 00 00  |	|      data      |	 e r
	|00018|--|00002|	|len |flags| type|
	| 6b 65 65 70  |	|      data      |	 k e e p
	| 61 6c 69 76  |	|      data      |	 a l i v
	| 65 64 5f 69  |	|      data      |	 e d _ i
	| 6e 00 00 00  |	|      data      |	 n
	|00240|N-|00004|	|len |flags| type|
	  |00052|N-|00001|	|len |flags| type|
	  |00012|--|00001|	|len |flags| type|
	  | 70 61 79 6c  |	|      data      |	 p a y l
	  | 6f 61 64 00  |	|      data      |	 o a d
	    |00036|N-|00002|	|len |flags| type|     EXPR_DATA
	      |00008|--|00001|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_DREG 1
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00002|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_BASE 1
	      | 00 00 00 01  |	|      data      |	NFT_PAYLOAD_NETWORK_HEADER
	      |00008|--|00003|	|len |flags| type|   NFTNL_EXPR_PAYLOAD_PAYLOAD OFFSET 24
	      | 00 00 00 18  |	|      data      |
	      |00008|--|00004|	|len |flags| type|	NFTNL_EXPR_PAYLOAD_PAYLOAD_LEN 16
	      | 00 00 00 10  |	|      data      |

	    |00056|N-|00001|	|len |flags| type|		LIST_ELEM
	      |00008|--|00001|	|len |flags| type|	NFTNL_EXPR_NAME
	      | 63 6d 70 00  |	|      data      |	 c m p
	      |00044|N-|00002|	|len |flags| type|	EXPR_DATA
	        |00008|--|00001|	|len |flags| type|	NFTNL_EXPR_CMP_SREG = NFTA_CMP_SREG - look at nftnl_expr_***_build
	        | 00 00 00 01  |	|      data      |	NFT_REG_1
	        |00008|--|00002|	|len |flags| type|	NFTNL_EXPR_CMP_OP = NFTA_CMP_OP
	        | 00 00 00 05  |	|      data      |	NFT_CMP_GTE
	        |00024|N-|00003|	|len |flags| type|	NFTNL_EXPR_CMP_DATA = NFTA_CMP_DATA
	          |00020|--|00001|	|len |flags| type| NFTA_DATA_VALUE
	          | fe 80 00 00  |	|      data      |
	          | 00 00 00 00  |	|      data      |
	          | 00 00 00 00  |	|      data      |
	          | 00 00 00 00  |	|      data      |
	|00056|N-|00001|	|len |flags| type|
	    |00008|--|00001|	|len |flags| type|
	    | 63 6d 70 00  |	|      data      |	 c m p
	    |00044|N-|00002|	|len |flags| type|
	      |00008|--|00001|	|len |flags| type|
	      | 00 00 00 01  |	|      data      |
	      |00008|--|00002|	|len |flags| type|
	      | 00 00 00 03  |	|      data      |	NFT_CMP_LTE
	      |00024|N-|00003|	|len |flags| type|
	        |00020|--|00001|	|len |flags| type|
	        | fe bf 00 00  |	|      data      |
	        | 00 00 00 00  |	|      data      |
	        | 00 00 00 00  |	|      data      |
	      | 00 00 ff ff  |	|      data      |
	|00072|N-|00001|	|len |flags| type| NFTA_LIST_ELEM
	    |00014|--|00001|	|len |flags| type|	NFTA_EXPR_NAME
	    | 69 6d 6d 65  |	|      data      |	 i m m e
	    | 64 69 61 74  |	|      data      |	 d i a t
	    | 65 00 00 00  |	|      data      |	 e

	    |00052|N-|00002|	|len |flags| type| NFTA_EXPR_DATA
	      |00008|--|00001|	|len |flags| type| NFTA_IMMEDIATE_DREG
	      | 00 00 00 00  |	|      data      | NFT_REG_VERDICT

	      |00040|N-|00002|	|len |flags| type| NFTNL_EXPR_IMM_VERDICT
	        |00036|N-|00002|	|len |flags| type| NFTA_DATA_VERDICT
	          |00008|--|00001|	|len |flags| type| NFTA_VERDICT_CODE
	          | ff ff ff fc  |	|      data      | NFT_GOTO
	          |00021|--|00002|	|len |flags| type| NFTA_???
	          | 6b 65 65 70  |	|      data      |	 k e e p
	          | 61 6c 69 76  |	|      data      |	 a l i v
	          | 65 64 5f 69  |	|      data      |	 e d _ i
	          | 6e 5f 6c 6c  |	|      data      |	 n _ l l
	          | 00 00 00 00  |	|      data      |
	----------------	------------------
	----------------	------------------
	|  0000000020  |	| message length |
	| 00017 | R--- |	|  type | flags  |
	|  0000000005  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |
	----------------	------------------
	*/

	if (handle != NULL) {
		handle_num = atoll(handle);
		nftnl_rule_set_u64(r, NFTNL_RULE_POSITION, handle_num);
	}

	add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
		    saddr ? offsetof(struct ip6_hdr, ip6_src) : offsetof(struct ip6_hdr, ip6_dst), sizeof(struct in6_addr));

	/* The following is interpreted as a range by nftables */
	ip6.s6_addr32[0] = htonl(0xfe800000);
	ip6.s6_addr32[1] = ip6.s6_addr32[2] = ip6.s6_addr32[3] = 0;
	add_cmp(r, NFT_REG_1, NFT_CMP_GTE, &ip6, sizeof(ip6));

	ip6.s6_addr32[0] = htonl(0xfebfffff);
	ip6.s6_addr32[1] = ip6.s6_addr32[2] = ip6.s6_addr32[3] = 0xffffffff;
	add_cmp(r, NFT_REG_1, NFT_CMP_LTE, &ip6, sizeof(ip6));

	add_counter(r);

	add_immediate(r, NFT_GOTO, chain_dest);

	return r;
}

static struct nftnl_rule
*setup_rule_icmpv6(uint8_t family, const char *table,
				   const char *chain, const char *handle,
				   const char *set, uint32_t set_id, uint32_t verdict, bool neg)
{
	struct nftnl_rule *r = NULL;
	uint64_t handle_num;
	struct ip6_hdr ip6;
	struct icmp6_hdr icmp6;

	r = nftnl_rule_alloc();
	if (r == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_rule_set(r, NFTNL_RULE_TABLE, table);
	nftnl_rule_set(r, NFTNL_RULE_CHAIN, chain);
	nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, family);

	if (handle != NULL) {
		handle_num = atoll(handle);
		nftnl_rule_set_u64(r, NFTNL_RULE_POSITION, handle_num);
	}

	ip6.ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_ICMPV6;
	add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
		    offsetof(struct ip6_hdr, ip6_ctlun.ip6_un1.ip6_un1_nxt), sizeof(ip6.ip6_ctlun.ip6_un1.ip6_un1_nxt));
	add_cmp(r, NFT_REG_1, NFT_CMP_EQ, &ip6.ip6_ctlun.ip6_un1.ip6_un1_nxt, sizeof(ip6.ip6_ctlun.ip6_un1.ip6_un1_nxt));

	add_payload(r, NFT_PAYLOAD_TRANSPORT_HEADER, NFT_REG_1,
		    offsetof(struct icmp6_hdr, icmp6_type), sizeof(icmp6.icmp6_type));
	add_lookup(r, NFT_REG_1, set, set_id, neg);

	add_counter(r);

	add_immediate(r, verdict, NULL);

	return r;
}

#ifdef UNUSED_CODE
static struct nftnl_rule *setup_rule_simple(uint8_t family, const char *table,
				   const char *chain, const char *handle,
				   uint32_t verdict)
{
	struct nftnl_rule *r = NULL;
	uint64_t handle_num;

	r = nftnl_rule_alloc();
	if (r == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return NULL;
	}

	nftnl_rule_set(r, NFTNL_RULE_TABLE, table);
	nftnl_rule_set(r, NFTNL_RULE_CHAIN, chain);
	nftnl_rule_set_u32(r, NFTNL_RULE_FAMILY, family);

	if (handle != NULL) {
		handle_num = atoll(handle);
		nftnl_rule_set_u64(r, NFTNL_RULE_POSITION, handle_num);
	}

	add_counter(r);

	add_immediate(r, verdict, NULL);

	return r;
}
#endif

static void
setup_link_local_checks(struct mnl_nlmsg_batch *batch, bool concat_ifname)
{
	char *set_name = concat_ifname ? "vips_link_local_name" : "vips_link_local";
	struct nlmsghdr *nlh;
	struct nftnl_set *s;
	struct nftnl_rule *r;

	s = setup_set(NFPROTO_IPV6, global_data->vrrp_nf_table_name, set_name, (TYPE_IP6ADDR << TYPE_BITS) | (concat_ifname ? TYPE_IFNAME : TYPE_IFINDEX));

	nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
				      NFT_MSG_NEWSET, NFPROTO_IPV6,
				      NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_set_nlmsg_build_payload(nlh, s);
	nftnl_set_free(s);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived in_link_local ip6 daddr . iifname @set_name drop */
	r = setup_rule_if(NFPROTO_IPV6, global_data->vrrp_nf_table_name, "in_link_local", NULL, set_name,
			false, concat_ifname, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived out_link_local ip6 saddr . oifname @set_name drop */
	r = setup_rule_if(NFPROTO_IPV6, global_data->vrrp_nf_table_name, "out_link_local", NULL, set_name,
			true, concat_ifname, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);
}

static struct mnl_nlmsg_batch *
nft_start_batch(void)
{
	struct mnl_nlmsg_batch *batch;
	char *buf = MALLOC(2 * MNL_SOCKET_BUFFER_SIZE);

	if (!seq)
		seq = time(NULL);

	batch = mnl_nlmsg_batch_start(buf, 2 * MNL_SOCKET_BUFFER_SIZE);

	nftnl_batch_begin(mnl_nlmsg_batch_current(batch), seq++);
	my_mnl_nlmsg_batch_next(batch);

	return batch;
}

void
nft_end_batch(struct mnl_nlmsg_batch *batch, bool more)
{
	void *buf;

	nftnl_batch_end(mnl_nlmsg_batch_current(batch), seq++);
	my_mnl_nlmsg_batch_next(batch);

	exchange_nl_msg(batch);

	if (more) {
		mnl_nlmsg_batch_reset(batch);

		nftnl_batch_begin(mnl_nlmsg_batch_current(batch), seq++);
		my_mnl_nlmsg_batch_next(batch);
	}
	else {
		buf = mnl_nlmsg_batch_head(batch);
		FREE(buf);
		mnl_nlmsg_batch_stop(batch);
	}
}

/* To get the netlink message returned (with the handle), set NLM_F_ECHO in nftnl_..._nlmsg_build_hdr
 * For some reason, it isn't working for the second batch sent.
 */
static void
nft_setup_ipv4(struct mnl_nlmsg_batch *batch)
{
	struct nlmsghdr *nlh;
	struct nftnl_table *ta;
	struct nftnl_chain *t;
	struct nftnl_set *s;
	struct nftnl_rule *r;
	char *table = global_data->vrrp_nf_table_name;

	/* nft add table ip keepalived */
	ta = table_add_parse(NFPROTO_IPV4, table);
	nlh = nftnl_table_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWTABLE, nftnl_table_get_u32(ta, NFTNL_TABLE_FAMILY),
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_table_nlmsg_build_payload(nlh, ta);
	nftnl_table_free(ta);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip keepalived in { type filter hook input priority -1; policy accept; } */
	t = chain_add_parse(table, "in");
	if (t == NULL)
		exit(EXIT_FAILURE);

	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV4,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_IN);	// input
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip keepalived out { type filter hook output priority -1; policy accept } */
	t = chain_add_parse(global_data->vrrp_nf_table_name, "out");
	if (t == NULL)
		exit(EXIT_FAILURE);

	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV4,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_OUT);
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add set ip keepalived vips { type ipv4_addr; } */
	s = setup_set(NFPROTO_IPV4, table, "vips", TYPE_IPADDR);

	nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
				      NFT_MSG_NEWSET, NFPROTO_IPV4,
				      NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_set_nlmsg_build_payload(nlh, s);
	nftnl_set_free(s);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip keepalived in ip daddr @vips drop */
	r = setup_rule(NFPROTO_IPV4, table, "in", NULL, "vips", false, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip keepalived out ip saddr @vips drop */
	r = setup_rule(NFPROTO_IPV4, table, "out", NULL, "vips", true, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	ipv4_table_setup = true;
}

static void
nft_setup_ipv6(struct mnl_nlmsg_batch *batch)
{
	struct nlmsghdr *nlh;
	struct nftnl_table *ta;
	struct nftnl_chain *t;
	struct nftnl_set *s;
	struct nftnl_rule *r;
	struct nftnl_set_elem *e;
	struct icmp6_hdr icmp6;
	char *table = global_data->vrrp_nf_table_name;

	/* nft add table ip6 keepalived */
	ta = table_add_parse(NFPROTO_IPV6, table);
	nlh = nftnl_table_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWTABLE, nftnl_table_get_u32(ta, NFTNL_TABLE_FAMILY),
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_table_nlmsg_build_payload(nlh, ta);
	nftnl_table_free(ta);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived in { type filter hook input priority PRIORITY; policy accept; } */
	t = chain_add_parse(table, "in");
	if (t == NULL)
		exit(EXIT_FAILURE);

	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_IN);	// input
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived out { type filter hook putput priority PRIORITY; policy accept; } */
	t = chain_add_parse(table, "out");
	if (t == NULL)
		exit(EXIT_FAILURE);

	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_OUT);
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived in { type filter hook input priority PRIORITY; policy accept; } */
	t = chain_add_parse(table, "in");
	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_IN);
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived in_link_local */
	t = chain_add_parse(table, "in_link_local");
	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived out */
	t = chain_add_parse(table, "out");
	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_HOOKNUM, NF_INET_LOCAL_OUT);
	nftnl_chain_set_str(t, NFTNL_CHAIN_TYPE, "filter");
	nftnl_chain_set_s32(t, NFTNL_CHAIN_PRIO, global_data->vrrp_nf_chain_priority);
	nftnl_chain_set_u32(t, NFTNL_CHAIN_POLICY, NF_ACCEPT);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add chain ip6 keepalived out_link_local */
	t = chain_add_parse(table, "out_link_local");
	nlh = nftnl_chain_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					NFT_MSG_NEWCHAIN, NFPROTO_IPV6,
					NLM_F_CREATE|NLM_F_ACK, seq++);
	nftnl_chain_nlmsg_build_payload(nlh, t);
	nftnl_chain_free(t);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add set ip6 keepalived vips {type ipv6_addr; } */
	s = setup_set(NFPROTO_IPV6, table, "vips", TYPE_IP6ADDR);

	nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
				      NFT_MSG_NEWSET, NFPROTO_IPV6,
				      NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_set_nlmsg_build_payload(nlh, s);
	nftnl_set_free(s);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add set ip6 keepalived neighbor-discovery { type icmpv6_type; } */
	s = setup_set(NFPROTO_IPV6, table, "neighbor-discovery", TYPE_ICMPV6_TYPE);

	nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
				      NFT_MSG_NEWSET, NFPROTO_IPV6,
				      NLM_F_CREATE|NLM_F_ACK, seq++);
	/* set_id = nftnl_set_get_u32(s, NFTNL_SET_ID); */

	nftnl_set_set_u32(s, NFTNL_SET_FLAGS, NFT_SET_CONSTANT);
	nftnl_set_set_u32(s, NFTNL_SET_KEY_LEN, sizeof(icmp6.icmp6_type));

	nftnl_set_nlmsg_build_payload(nlh, s);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add element ip6 keepalived neighbor-discovery { nd-neighbor-solicit, nd-neighbor-advert } */
	nlh = nftnl_set_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
				      NFT_MSG_NEWSETELEM, NFPROTO_IPV6,
				      NLM_F_CREATE|NLM_F_ACK, seq++);
	e = nftnl_set_elem_alloc();
	icmp6.icmp6_type = ND_NEIGHBOR_SOLICIT;
	nftnl_set_elem_set(e, NFTNL_SET_ELEM_KEY, &icmp6.icmp6_type, sizeof(icmp6.icmp6_type));
	nftnl_set_elem_add(s, e);

	e = nftnl_set_elem_alloc();
	icmp6.icmp6_type = ND_NEIGHBOR_ADVERT;
	nftnl_set_elem_set(e, NFTNL_SET_ELEM_KEY, &icmp6.icmp6_type, sizeof(icmp6.icmp6_type));
	nftnl_set_elem_add(s, e);

	nftnl_set_elems_nlmsg_build_payload(nlh, s);
	nftnl_set_free(s);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived in icmpv6 @neighbor-discovery accept */
	r = setup_rule_icmpv6(NFPROTO_IPV6, table, "in", NULL,
			"neighbor-discovery", 0, NF_ACCEPT, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived in ip6 daddr fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff goto in_link_local */
	r = setup_rule_range_goto(NFPROTO_IPV6, table, "in", NULL, "in_link_local", false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived in icmpv6 @neighbor-discovery accept */
	r = setup_rule_icmpv6(NFPROTO_IPV6, table, "out", NULL,
			"neighbor-discovery", 0, NF_ACCEPT, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived out ip6 saddr fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff goto out_link_local */
	r = setup_rule_range_goto(NFPROTO_IPV6, table, "out", NULL, "out_link_local", true);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived in ip6 daddr @vips drop */
	r = setup_rule(NFPROTO_IPV6, table, "in", NULL, "vips", false, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	/* nft add rule ip6 keepalived out ip6 saddr @vips drop */
	r = setup_rule(NFPROTO_IPV6, table, "out", NULL, "vips", true, NF_DROP, false);
	nlh = nftnl_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nftnl_rule_get_u32(r, NFTNL_RULE_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nftnl_rule_nlmsg_build_payload(nlh, r);
	nftnl_rule_free(r);
	my_mnl_nlmsg_batch_next(batch);

	ipv6_table_setup = true;
}

static void
nft_update_ipv4_address(struct mnl_nlmsg_batch *batch, ip_address_t *addr, struct nftnl_set **s)
{
	struct nftnl_set_elem *e;

	if (!ipv4_table_setup)
		nft_setup_ipv4(batch);

	if (!*s) {
		*s = nftnl_set_alloc();
		if (*s == NULL) {
			log_message(LOG_INFO, "OOM error - %d", errno);
			return;
		}

		nftnl_set_set(*s, NFTNL_SET_TABLE, global_data->vrrp_nf_table_name);
		nftnl_set_set(*s, NFTNL_SET_NAME, "vips");
	}

	/* nft add element ip keepalived vips { ADDR } */
	e = nftnl_set_elem_alloc();
	if (e == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return;
	}

	nftnl_set_elem_set(e, NFTNL_SET_ELEM_KEY, &addr->u.sin.sin_addr.s_addr, sizeof(struct in_addr));
	nftnl_set_elem_add(*s, e);
}

static void
nft_update_ipv6_address(struct mnl_nlmsg_batch *batch, ip_address_t *addr, bool dont_track_primary, interface_t *ifp,
			struct nftnl_set **set_global, struct nftnl_set **set_ll, struct nftnl_set **set_ll_ifname)
{
	struct nftnl_set_elem *e;
	uint32_t data_buf[sizeof(struct in6_addr) + IFNAMSIZ];
	struct nftnl_set **s;
	char *set_name;
	bool use_link_name = false;
	bool is_link_local;
	uint32_t len;

	if (!ipv6_table_setup)
		nft_setup_ipv6(batch);

	is_link_local = IN6_IS_ADDR_LINKLOCAL(&addr->u.sin6_addr);
	if (!is_link_local) {
		s = set_global;
		set_name = "vips";
	} else if (!global_data->vrrp_nf_ifindex &&
		   dont_track_primary &&
		   (addr->ifp == ifp || addr->dont_track)) {
		s = set_ll_ifname;
		set_name = "vips_link_local_name";
		use_link_name = true;
	} else {
		s = set_ll;
		set_name = "vips_link_local";
	}

	/* Create the specific set if not already done so */
	if (is_link_local) {
		if (use_link_name) {
			if (!setup_ll_ifname) {
				setup_link_local_checks(batch, true);
				setup_ll_ifname = true;
			}
		} else {
			if (!setup_ll_ifindex) {
				setup_link_local_checks(batch, false);
				setup_ll_ifindex = true;
			}
		}
	}

	/* Create set structure if it doesn't already exist */
	if (!*s) {
		*s = nftnl_set_alloc();
		if (*s == NULL) {
			log_message(LOG_INFO, "OOM error - %d", errno);
			return;
		}

		nftnl_set_set(*s, NFTNL_SET_TABLE, global_data->vrrp_nf_table_name);
		nftnl_set_set(*s, NFTNL_SET_NAME, set_name);
	}

	/* Add element to set
	 * nft add element ip6 keepalived vips ADDR or
	 * nft add element ip6 keepalived vips_link_local ADDR . IF or
	 * nft add element ip6 keepalived vips_link_local_ifname ADDR . IF */
	e = nftnl_set_elem_alloc();
	if (e == NULL) {
		log_message(LOG_INFO, "OOM error - %d", errno);
		return;
	}

	data_buf[0] = addr->u.sin6_addr.s6_addr32[0];
	data_buf[1] = addr->u.sin6_addr.s6_addr32[1];
	data_buf[2] = addr->u.sin6_addr.s6_addr32[2];
	data_buf[3] = addr->u.sin6_addr.s6_addr32[3];
	len = sizeof(struct in6_addr);

	if (is_link_local) {
		if (use_link_name) {
			memset(&data_buf[4], 0, IFNAMSIZ);
			memcpy(&data_buf[4], addr->ifp->ifname, strlen(addr->ifp->ifname));
			len += IFNAMSIZ;
		} else {
			data_buf[4] = addr->ifp->ifindex;
			len += sizeof(data_buf[4]);
		}
	}

	nftnl_set_elem_set(e, NFTNL_SET_ELEM_KEY, &data_buf, len);
	nftnl_set_elem_add(*s, e);
}

static void
nft_update_addresses(vrrp_t *vrrp, int cmd)
{
	struct mnl_nlmsg_batch *batch;
	struct nlmsghdr *nlh;
	ip_address_t *ip_addr;
	element e;
	struct nftnl_set *ipv4_set = NULL;
	struct nftnl_set *ipv6_set = NULL;
	struct nftnl_set *ipv6_ll_index_set = NULL;
	struct nftnl_set *ipv6_ll_name_set = NULL;
	bool set_rule = (cmd == NFT_MSG_NEWSETELEM);

	batch = nft_start_batch();

	LIST_FOREACH(vrrp->vip, ip_addr, e) {
		if (set_rule == ip_addr->nftable_rule_set)
			continue;

		if (ip_addr->ifa.ifa_family == AF_INET)
			nft_update_ipv4_address(batch, ip_addr, &ipv4_set);
		else
			nft_update_ipv6_address(batch, ip_addr, vrrp->dont_track_primary, vrrp->ifp,
					&ipv6_set, &ipv6_ll_index_set, &ipv6_ll_name_set);

		ip_addr->nftable_rule_set = set_rule;
	}

	LIST_FOREACH(vrrp->evip, ip_addr, e) {
		if (set_rule == ip_addr->nftable_rule_set)
			continue;

		if (ip_addr->ifa.ifa_family == AF_INET)
			nft_update_ipv4_address(batch, ip_addr, &ipv4_set);
		else
			nft_update_ipv6_address(batch, ip_addr, vrrp->dont_track_primary, vrrp->ifp,
					&ipv6_set, &ipv6_ll_index_set, &ipv6_ll_name_set);

		ip_addr->nftable_rule_set = set_rule;
	}

	if (ipv4_set) {
		nlh = nftnl_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					    cmd, NFPROTO_IPV4,
					    NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK,
					    seq++);
		nftnl_set_elems_nlmsg_build_payload(nlh, ipv4_set);
		nftnl_set_free(ipv4_set);
		my_mnl_nlmsg_batch_next(batch);
	}

	if (ipv6_set) {
		nlh = nftnl_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					    cmd, NFPROTO_IPV6,
					    NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK,
					    seq++);
		nftnl_set_elems_nlmsg_build_payload(nlh, ipv6_set);
		nftnl_set_free(ipv6_set);
		my_mnl_nlmsg_batch_next(batch);
	}

	if (ipv6_ll_index_set) {
		nlh = nftnl_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					    cmd, NFPROTO_IPV6,
					    NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK,
					    seq++);
		nftnl_set_elems_nlmsg_build_payload(nlh, ipv6_ll_index_set);
		nftnl_set_free(ipv6_ll_index_set);
		my_mnl_nlmsg_batch_next(batch);
	}

	if (ipv6_ll_name_set) {
		nlh = nftnl_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
					    cmd, NFPROTO_IPV6,
					    NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK,
					    seq++);
		nftnl_set_elems_nlmsg_build_payload(nlh, ipv6_ll_name_set);
		nftnl_set_free(ipv6_ll_name_set);
		my_mnl_nlmsg_batch_next(batch);
	}

	nft_end_batch(batch, false);
}

void
nft_add_addresses(vrrp_t *vrrp)
{
	nft_update_addresses(vrrp, NFT_MSG_NEWSETELEM);
}

void
nft_remove_addresses(vrrp_t *vrrp)
{
if (!nl) return;	// Should delete tables
	nft_update_addresses(vrrp, NFT_MSG_DELSETELEM);
}

void
nft_remove_addresses_iplist(list l)
{
	vrrp_t vrrp = { .vip = l };

	nft_update_addresses(&vrrp, NFT_MSG_DELSETELEM);
}

void
nft_cleanup(void)
{
	/*
	----------------	------------------
	|  0000000020  |	| message length |
	| 00016 | R--- |	|  type | flags  |
	|  0000000003  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |
	----------------	------------------
	----------------	------------------
	|  0000000036  |	| message length |
	| 02562 | R-A- |	|  type | flags  |
	|  0000000004  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 02 00 00 00  |	|  extra header  |
	|00015|--|00001|	|len |flags| type|
	| 6b 65 65 70  |	|      data      |	 k e e p
	| 61 6c 69 76  |	|      data      |	 a l i v
	| 65 64 00 00  |	|      data      |	 e d
	----------------	------------------
	----------------	------------------
	|  0000000020  |	| message length |
	| 00017 | R--- |	|  type | flags  |
	|  0000000005  |	| sequence number|
	|  0000000000  |	|     port ID    |
	----------------	------------------
	| 00 00 0a 00  |	|  extra header  |
	----------------	------------------
	*/
	struct nftnl_table *t;
	struct nlmsghdr *nlh;
	struct mnl_nlmsg_batch *batch;

	if (!ipv4_table_setup && !ipv6_table_setup)
		return;

	batch = nft_start_batch();

	t = nftnl_table_alloc();
	nftnl_table_set_str(t, NFTNL_TABLE_NAME, global_data->vrrp_nf_table_name);

	if (ipv4_table_setup) {
		nlh = nftnl_table_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
						NFT_MSG_DELTABLE, NFPROTO_IPV4,
						NLM_F_ACK, seq++);
		nftnl_table_nlmsg_build_payload(nlh, t);

		my_mnl_nlmsg_batch_next(batch);
	}

	if (ipv6_table_setup) {
		nlh = nftnl_table_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
						NFT_MSG_DELTABLE, NFPROTO_IPV6,
						NLM_F_ACK, seq++);
		nftnl_table_nlmsg_build_payload(nlh, t);

		my_mnl_nlmsg_batch_next(batch);
	}

	nftnl_table_free(t);

	nft_end_batch(batch, false);

	ipv4_table_setup = false;
	ipv6_table_setup = false;
	setup_ll_ifname = false;
	setup_ll_ifindex = false;
}

void
nft_end(void)
{
	nft_cleanup();

	mnl_socket_close(nl);
	nl = NULL;
}
