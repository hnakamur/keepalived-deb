# Makefile.am
#
# Keepalived OpenSource project.
#
# Copyright (C) 2020-2020 Alexandre Cassen, <acassen@gmail.com>

DATE=`date -u --date=@\"\`git log -n 1 --format=%ct -- $@.in 2>/dev/null || date --reference=$(srcdir)/$@.in +%s\`\" +\"%Y-%m-%d\"`

edit = echo "  EDIT     $@"; \
	@SED@ \
	 -e "s|@DATE[@]|$(DATE)|g"

dist_man1_MANS  = genhash.1

genhash.1: Makefile.am $(top_builddir)/lib/config.h
	@$(edit) '$(srcdir)/$@.in' >$@

genhash.1: $(srcdir)/genhash.1.in

EXTRA_DIST		= genhash.1.in

MOSTLYCLEANFILES	= genhash.1
