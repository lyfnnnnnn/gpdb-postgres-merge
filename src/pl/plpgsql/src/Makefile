#-------------------------------------------------------------------------
#
# Makefile for the pl/pgsql procedural language
#
# $PostgreSQL: pgsql/src/pl/plpgsql/src/Makefile,v 1.35 2009/11/12 00:13:00 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME= plpgsql

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)
SHLIB_LINK = $(filter -lintl, $(LIBS))
rpath =

OBJS = pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o pl_scanner.o

DATA = plpgsql.control plpgsql--1.0.sql plpgsql--unpackaged--1.0.sql

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


install: all install-lib install-data

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'

uninstall: uninstall-lib uninstall-data

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))

.PHONY: install-data uninstall-data


# Force these dependencies to be known even without dependency info built:
pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o pl_scanner.o: plpgsql.h pl_gram.h

# See notes in src/backend/parser/Makefile about the following two rules

pl_gram.h: pl_gram.c ;

pl_gram.c: gram.y
ifdef BISON
	$(BISON) -d $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

distprep: pl_gram.h pl_gram.c

# pl_gram.c and pl_gram.h are in the distribution tarball,
# so they are not cleaned here.
clean distclean: clean-lib
	rm -f $(OBJS)

maintainer-clean: clean
	rm -f pl_gram.c pl_gram.h
