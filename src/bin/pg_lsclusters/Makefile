#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_lsclusters (Kumar Server)
#
# src/bin/pg_lsclusters/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_lsclusters - list the PostgreSQL clusters running on this machine"
PGAPPICON = win32

subdir = src/bin/pg_lsclusters
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pg_lsclusters.o

all: pg_lsclusters

pg_lsclusters: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_lsclusters$(X) '$(DESTDIR)$(bindir)/pg_lsclusters$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_lsclusters$(X)'

clean distclean:
	rm -f pg_lsclusters$(X) $(OBJS)
