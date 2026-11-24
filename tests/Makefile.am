dist_check_SCRIPTS = test.pl
dist_check_DATA = testcase-44100.wav testcase-22050.wav testcase-float32.wav

check_PROGRAMS = twe_reservoir
twe_reservoir_SOURCES = twe_reservoir.c
twe_reservoir_CFLAGS = -I$(top_srcdir)/libtwolame $(WARNING_CFLAGS)
twe_reservoir_LDADD = $(top_builddir)/libtwolame/libtwolame.la

TESTS = $(dist_check_SCRIPTS) twe_reservoir$(EXEEXT)
PL_LOG_COMPILER = $(PERL)
AM_PL_LOG_FLAGS = -Mstrict -w

TESTS_ENVIRONMENT = \
	TWOLAME_CMD="$(top_builddir)/frontend/twolame" \
	STWOLAME_CMD="$(top_builddir)/simplefrontend/stwolame"

CLEANFILES = *.mp2 *.raw
