dist_check_SCRIPTS = test.pl
dist_check_DATA = testcase-44100.wav testcase-22050.wav testcase-float32.wav

check_PROGRAMS = test-chunked
test_chunked_SOURCES = test-chunked.c
test_chunked_CPPFLAGS = -I$(top_srcdir)/libtwolame
test_chunked_LDADD = $(top_builddir)/libtwolame/libtwolame.la

TESTS = $(check_PROGRAMS) $(dist_check_SCRIPTS)
TEST_EXTENSIONS = .pl
PL_LOG_COMPILER = $(PERL)
AM_PL_LOG_FLAGS = -Mstrict -w

TESTS_ENVIRONMENT = \
	TWOLAME_CMD="$(top_builddir)/frontend/twolame" \
	STWOLAME_CMD="$(top_builddir)/simplefrontend/stwolame"

CLEANFILES = *.mp2 *.raw
