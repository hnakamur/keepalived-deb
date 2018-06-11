/*
 * Soft:        Vrrpd is an implementation of VRRPv2 as specified in rfc2338.
 *              VRRP is a protocol which elect a master server on a LAN. If the
 *              master fails, a backup server takes over.
 *              The original implementation has been made by jerome etienne.
 *
 * Part:        Print running VRRP state information
 *
 * Author:      John Southworth, <john.southworth@vyatta.com>
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
 * Copyright (C) 2012 John Southworth, <john.southworth@vyatta.com>
 * Copyright (C) 2015-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>

#include "logger.h"

#include "vrrp.h"
#include "vrrp_data.h"
#include "vrrp_print.h"

static const char *dump_file = "/tmp/keepalived.data";
static const char *stats_file = "/tmp/keepalived.stats";

void
vrrp_print_data(void)
{
	FILE *file = fopen (dump_file, "w");

	if (!file) {
		log_message(LOG_INFO, "Can't open %s (%d: %s)",
			dump_file, errno, strerror(errno));
		return;
	}

	dump_data_vrrp(file);

	fclose(file);
}

void
vrrp_print_stats(void)
{
	FILE *file;
	file = fopen (stats_file, "w");

	if (!file) {
		log_message(LOG_INFO, "Can't open %s (%d: %s)",
			stats_file, errno, strerror(errno));
		return;
	}

	list l = vrrp_data->vrrp;
	element e;
	vrrp_t *vrrp;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vrrp = ELEMENT_DATA(e);
		fprintf(file, "VRRP Instance: %s", vrrp->iname);
		fprintf(file, "  Advertisements:");
		fprintf(file, "    Received: %" PRIu64 "", vrrp->stats->advert_rcvd);
		fprintf(file, "    Sent: %d", vrrp->stats->advert_sent);
		fprintf(file, "  Became master: %d", vrrp->stats->become_master);
		fprintf(file, "  Released master: %d",
			vrrp->stats->release_master);
		fprintf(file, "  Packet Errors:");
		fprintf(file, "    Length: %" PRIu64 "", vrrp->stats->packet_len_err);
		fprintf(file, "    TTL: %" PRIu64 "", vrrp->stats->ip_ttl_err);
		fprintf(file, "    Invalid Type: %" PRIu64 "",
			vrrp->stats->invalid_type_rcvd);
		fprintf(file, "    Advertisement Interval: %" PRIu64 "",
			vrrp->stats->advert_interval_err);
		fprintf(file, "    Address List: %" PRIu64 "",
			vrrp->stats->addr_list_err);
		fprintf(file, "  Authentication Errors:");
		fprintf(file, "    Invalid Type: %d",
			vrrp->stats->invalid_authtype);
#ifdef _WITH_VRRP_AUTH_
		fprintf(file, "    Type Mismatch: %d",
			vrrp->stats->authtype_mismatch);
		fprintf(file, "    Failure: %d",
			vrrp->stats->auth_failure);
#endif
		fprintf(file, "  Priority Zero:");
		fprintf(file, "    Received: %" PRIu64 "", vrrp->stats->pri_zero_rcvd);
		fprintf(file, "    Sent: %" PRIu64 "", vrrp->stats->pri_zero_sent);
	}
	fclose(file);
}