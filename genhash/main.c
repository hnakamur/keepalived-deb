/*
 * Soft:        Perform a GET query to a remote HTTP/HTTPS server.
 *              Set a timer to compute global remote server response
 *              time.
 *
 * Part:        Main entry point.
 *
 * Authors:     Alexandre Cassen, <acassen@linux-vs.org>
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

#include "config.h"

/* system includes */
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* keepalived includes */
#include "utils.h"
#include "signals.h"

/* genhash includes */
#include "include/main.h"


/* global var */
REQ *req = NULL;
int exit_code;

/* Terminate handler */
static void
sigend(__attribute__((unused)) void *v, __attribute__((unused)) int sig)
{
	/* register the terminate thread */
	thread_add_terminate_event(master);
}

/* Initialize signal handler */
static void
signal_init(void)
{
	signal_set(SIGHUP, sigend, NULL);
	signal_set(SIGINT, sigend, NULL);
	signal_set(SIGTERM, sigend, NULL);
	signal_ignore(SIGPIPE);
}

/* Usage function */
static void
usage(const char *prog)
{
	enum feat_hashes i;

	fprintf(stderr, VERSION_STRING);
	fprintf(stderr,
		"Usage:\n"
		"  %1$s -s server-address -p port -u url\n"
		"  %1$s -S -s server-address -p port -u url\n"
		"  %1$s -h\n" "  %1$s -r\n\n", prog);
	fprintf(stderr,
		"Commands:\n"
		"Either long or short options are allowed.\n"
		"  %1$s --use-ssl         -S       Use SSL connection to remote server.\n"
#ifdef _HAVE_SSL_SET_TLSEXT_HOST_NAME_
		"  %1$s --use-sni         -I       Use SNI during SSL handshake (uses virtualhost setting; see -V).\n"
#endif
		"  %1$s --server          -s       Use the specified remote server address.\n"
		"  %1$s --port            -p       Use the specified remote server port.\n"
		"  %1$s --url             -u       Use the specified remote server url.\n"
		"  %1$s --use-virtualhost -V       Use the specified virtualhost in GET query.\n"
		"  %1$s --hash            -H       Use the specified hash algorithm.\n"
		"  %1$s --verbose         -v       Use verbose mode output.\n"
		"  %1$s --help            -h       Display this short inlined help screen.\n"
		"  %1$s --release         -r       Display the release number.\n"
		"  %1$s --fwmark          -m       Use the specified FW mark.\n",
		prog);
	fprintf(stderr, "\nSupported hash algorithms:\n");
	for (i = hash_first; i < hash_guard; i++)
		fprintf(stderr, "  %s%s\n",
			hashes[i].id, i == hash_default ? " (default)": "");
}

/* Command line parser */
static int
parse_cmdline(int argc, char **argv, REQ * req_obj)
{
	int c;
	enum feat_hashes i;
	struct addrinfo hint, *res = NULL;
	int ret;
	void *ptr;
	char *endptr;
	long port_num;

	memset(&hint, '\0', sizeof hint);

	hint.ai_family = PF_UNSPEC;
	hint.ai_flags = AI_NUMERICHOST;

	struct option long_options[] = {
		{"release",		no_argument,       0, 'r'},
		{"help",		no_argument,       0, 'h'},
		{"verbose",		no_argument,       0, 'v'},
		{"use-ssl",		no_argument,       0, 'S'},
#ifdef _HAVE_SSL_SET_TLSEXT_HOST_NAME_
		{"use-sni",		no_argument,       0, 'I'},
#endif
		{"server",		required_argument, 0, 's'},
		{"hash",		required_argument, 0, 'H'},
		{"use-virtualhost",	required_argument, 0, 'V'},
		{"port",		required_argument, 0, 'p'},
		{"url",			required_argument, 0, 'u'},
		{"fwmark",		required_argument, 0, 'm'},
		{0, 0, 0, 0}
	};

	/* Parse the command line arguments */
	while ((c = getopt_long (argc, argv, "rhvSs:H:V:p:u:m:"
#ifdef _HAVE_SSL_SET_TLSEXT_HOST_NAME_
							       "I"
#endif
				  , long_options, NULL)) != EOF) {
		switch (c) {
		case 'r':
			fprintf(stderr, VERSION_STRING);
			break;
		case 'h':
			usage(argv[0]);
			break;
		case 'v':
			req_obj->verbose = 1;
			break;
		case 'S':
			req_obj->ssl = 1;
			break;
#ifdef _HAVE_SSL_SET_TLSEXT_HOST_NAME_
		case 'I':
			req_obj->sni = 1;
			break;
#endif
		case 's':
			if ((ret = getaddrinfo(optarg, NULL, &hint, &res)) != 0){
				fprintf(stderr, "server should be an IP, not %s\n", optarg);
				return CMD_LINE_ERROR;
			} else {
				if(res->ai_family == AF_INET) {
					req_obj->dst = res;
					ptr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
					inet_ntop (res->ai_family, ptr, req_obj->ipaddress, INET_ADDRSTRLEN);
				} else if (res->ai_family == AF_INET6) {
					req_obj->dst = res;
					ptr = &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
					inet_ntop (res->ai_family, ptr, req_obj->ipaddress, INET6_ADDRSTRLEN);
				} else {
					fprintf(stderr, "server should be an IP, not %s\n", optarg);
					return CMD_LINE_ERROR;
				}
			}
			break;
		case 'H':
			for (i = hash_first; i < hash_guard; i++)
				if (!strcasecmp(optarg, hashes[i].id)) {
					req_obj->hash = i;
					break;
				}
			if (i == hash_guard) {
				fprintf(stderr, "unknown hash algorithm: %s\n", optarg);
				return CMD_LINE_ERROR;
			}
			break;
		case 'V':
			req_obj->vhost = optarg;
			break;
		case 'p':
			port_num = strtol(optarg, &endptr, 10);
			if (*endptr || port_num <= 0 || port_num > 65535) {
				fprintf(stderr, "invalid port number '%s'\n", optarg);
				return CMD_LINE_ERROR;
			}
			req_obj->addr_port = htons(port_num);
			break;
		case 'u':
			req_obj->url = optarg;
			break;
		case 'm':
#ifdef _WITH_SO_MARK_
			req_obj->mark = (unsigned)strtoul(optarg + strspn(optarg, " \t"), &endptr, 10);
			if (*endptr || optarg[strspn(optarg, " \t")] == '-') {
				fprintf(stderr, "invalid fwmark '%s'\n", optarg);
				return CMD_LINE_ERROR;
			}
#else
			fprintf(stderr, "genhash built without fwmark support\n");
			return CMD_LINE_ERROR;
#endif
			break;
		default:
			usage(argv[0]);
			return CMD_LINE_ERROR;
		}
	}

	/* check unexpected arguments */
	if (optind < argc) {
		fprintf(stderr, "Unexpected argument(s): ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
		return CMD_LINE_ERROR;
	}

	return CMD_LINE_SUCCESS;
}

int
main(int argc, char **argv)
{
	char *url_default = "/";

#ifdef _MEM_CHECK_
	mem_log_init("Genhash", "Genhash process");
	enable_mem_log_termination();
#endif

	/* Allocate the room */
	req = (REQ *) MALLOC(sizeof (REQ));

	/* Preset (potentially) non-zero defaults */
	req->hash = hash_default;

	/* Command line parser */
	if (!parse_cmdline(argc, argv, req)) {
		FREE(req);
		exit(1);
	}

	/* Check minimum configuration need */
	if (!req->dst && !req->addr_port && !req->url) {
		freeaddrinfo(req->dst);
		FREE(req);
		exit(1);
	}

	if(!req->url)
		req->url = url_default;

	/* Init the reference timer */
	req->ref_time = timer_long(timer_now());
	DBG("Reference timer = %lu\n", req->ref_time);

	/* Init SSL context */
	init_ssl();

	/* Create the master thread */
	master = thread_make_master();

	/* Signal handling initialization  */
	signal_init();

	/* Register the GET request */
	init_sock();

	/*
	 * Processing the master thread queues,
	 * return and execute one ready thread.
	 * Run until error, used for debuging only.
	 * Note that not calling launch_thread_scheduler()
	 * does not activate SIGCHLD handling, however,
	 * this is no issue here.
	 */
	process_threads(master);

	/* Finalize output informations */
	if (req->verbose)
		printf("Global response time for [%s] =%lu\n",
			    req->url, req->response_time - req->ref_time);

	/* exit cleanly */
	thread_destroy_master(master);
	SSL_CTX_free(req->ctx);
	free_sock(sock);
	freeaddrinfo(req->dst);
	FREE(req);
	exit(exit_code);
}
