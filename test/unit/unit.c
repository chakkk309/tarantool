#include "unit.h"

#include <stdio.h>
#include <stdarg.h>

/*
 * The definition of function set_sigint_cb is needed to avoid the error
 * while linking. This occurs because in some unit tests there are calls
 * of the function lbox_console_readline() from console.c that is included
 * in box library. This function must call set_sigint_cb, defined in main.cc,
 * that is not linked with tests.
 */
#include <tarantool_ev.h>
typedef void (*sigint_cb_t)(ev_loop *loop, struct ev_signal *w, int revents);
sigint_cb_t
set_sigint_cb(sigint_cb_t new_sigint_cb){
	return NULL;
}

enum { MAX_LEVELS = 10 };
static int tests_done[MAX_LEVELS];
static int tests_failed[MAX_LEVELS];
static int plan_test[MAX_LEVELS];
static int level = -1;

void
_space(FILE *stream)
{
	for (int i = 0 ; i < level; i++) {
		fprintf(stream, "    ");
	}
}

void
plan(int count)
{
	++level;
	plan_test[level] = count;
	tests_done[level] = 0;
	tests_failed[level] = 0;

	_space(stdout);
	printf("%d..%d\n", 1, plan_test[level]);
}

int
check_plan(void)
{
	int r = 0;
	if (tests_done[level] != plan_test[level]) {
		_space(stderr);
		fprintf(stderr,
			"# Looks like you planned %d tests but ran %d.\n",
			plan_test[level], tests_done[level]);
		r = -1;
	}

	if (tests_failed[level]) {
		_space(stderr);
		fprintf(stderr,
			"# Looks like you failed %d test of %d run.\n",
			tests_failed[level], tests_done[level]);
		r = tests_failed[level];
	}
	--level;
	if (level >= 0) {
		is(r, 0, "subtests");
	}
	return r;
}

int
_ok(int condition, const char *fmt, ...)
{
	va_list ap;

	_space(stdout);
	printf("%s %d - ", condition ? "ok" : "not ok", ++tests_done[level]);
	if (!condition)
		tests_failed[level]++;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	return condition;
}

