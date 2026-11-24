dist_check_SCRIPTS = test.pl
dist_check_DATA = testcase-44100.wav testcase-22050.wav testcase-float32.wav

check_PROGRAMS = twc_entrypoints
twc_entrypoints_SOURCES = twc_entrypoints.c
twc_entrypoints_CFLAGS = -I$(top_builddir)/libtwolame -I$(top_srcdir)/libtwolame
twc_entrypoints_LDADD = $(top_builddir)/libtwolame/libtwolame.la -lm

# test.pl is a Perl script; twc_entrypoints is a compiled C unit test that is
# executed directly. Per-target test variables keep the Perl interpreter and
# its flags away from the native binary.
TESTS = $(dist_check_SCRIPTS) twc_entrypoints

LOG_COMPILER = $(PERL)
test_pl_LOG_FLAGS = -Mstrict -w
# An empty per-target compiler makes the serial harness run the executable
# directly instead of passing it through $(LOG_COMPILER).
twc_entrypoints_LOG_COMPILER =

TESTS_ENVIRONMENT = \
	TWOLAME_CMD="$(top_builddir)/frontend/twolame" \
	STWOLAME_CMD="$(top_builddir)/simplefrontend/stwolame"

CLEANFILES = *.mp2 *.raw
