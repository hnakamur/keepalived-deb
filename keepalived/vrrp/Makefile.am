# Makefile.am
#
# Keepalived OpenSource project.
#
# Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>

AM_CPPFLAGS		= -I $(top_srcdir)/keepalived/include -I $(top_srcdir)/lib
AM_CPPFLAGS		+= $(KA_CPPFLAGS) $(DEBUG_CPPFLAGS)
AM_CFLAGS		= $(KA_CFLAGS) $(DEBUG_CFLAGS)
AM_LDFLAGS		= $(KA_LDFLAGS) $(DEBUG_LDFLAGS)
# AM_LIBS		= $(KA_LIBS)
# AM_LIBTOOLFLAGS	= $(KA_LIBTOOLFLAGS)

noinst_LIBRARIES	= libvrrp.a

libvrrp_a_SOURCES	= \
	vrrp_daemon.c vrrp_print.c vrrp_data.c vrrp_parser.c \
	vrrp.c vrrp_notify.c vrrp_scheduler.c vrrp_sync.c \
	vrrp_arp.c vrrp_if.c vrrp_track.c vrrp_ipaddress.c \
	vrrp_ndisc.c vrrp_if_config.c vrrp_static_track.c \
	vrrp_iproute.c vrrp_iprule.c vrrp_ip_rule_route_parser.c

libvrrp_a_SOURCES	+= ../include/vrrp_daemon.h

libvrrp_a_LIBADD	=
EXTRA_libvrrp_a_SOURCES	=

if VMAC
  libvrrp_a_LIBADD	+= vrrp_vmac.o
  EXTRA_libvrrp_a_SOURCES += vrrp_vmac.c
endif

if VRRP_AUTH
  libvrrp_a_LIBADD	+= vrrp_ipsecah.o
  EXTRA_libvrrp_a_SOURCES += vrrp_ipsecah.c
endif

if WITH_DBUS
  libvrrp_a_LIBADD	+= vrrp_dbus.o
  EXTRA_libvrrp_a_SOURCES += vrrp_dbus.c
endif

if FIREWALL
  libvrrp_a_LIBADD	+= vrrp_firewall.o vrrp_firewall.o
  EXTRA_libvrrp_a_SOURCES += vrrp_firewall.c vrrp_firewall.c
endif

if IPTABLES
  libvrrp_a_LIBADD	+= vrrp_iptables.o vrrp_iptables_calls.o
  EXTRA_libvrrp_a_SOURCES += vrrp_iptables.c vrrp_iptables_calls.c
endif

if LIBIPSET
  libvrrp_a_LIBADD	+= vrrp_ipset.o
  EXTRA_libvrrp_a_SOURCES += vrrp_ipset.c
endif

if NFTABLES
  libvrrp_a_LIBADD	+= vrrp_nftables.o
  EXTRA_libvrrp_a_SOURCES += vrrp_nftables.c
endif

if SNMP_VRRP
  libvrrp_a_LIBADD	+= vrrp_snmp.o
  EXTRA_libvrrp_a_SOURCES += vrrp_snmp.c
endif

if WITH_JSON
  libvrrp_a_LIBADD	+= vrrp_json.o
  EXTRA_libvrrp_a_SOURCES += vrrp_json.c
endif

if NETWORK_MANAGER
  libvrrp_a_LIBADD	+= vrrp_vmac_nm.o
  EXTRA_libvrrp_a_SOURCES += vrrp_vmac_nm.c
endif

MAINTAINERCLEANFILES	= @MAINTAINERCLEANFILES@
