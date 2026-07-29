/* SPDX-License-Identifier: MIT */
/*
 * hegel_compat.h -- a thin compatibility layer that lets the sparsemap
 * property tests, originally written against the (now-deprecated)
 * gburd/hegel-c socket client, run unchanged on the official in-process
 * FFI library, hegeldev/hegel-rust/hegel-c (<hegel.h>, libhegel).
 *
 * The official API is an explicit event loop: a run produces test cases
 * one at a time; the body draws values with hegel_generate_* and then
 * calls hegel_mark_complete with a status (VALID = passed, INTERESTING
 * = failed, OVERRUN = ran out of choice budget).  The old API was a
 * callback that drew with hegel_draw_* and signalled failure by a plain
 * assert().
 *
 * This shim reconstructs the old surface on top of the new one:
 *
 *   - hegel_session               wraps hegel_context_t *
 *   - hegel_run_test(fn)          drives the run loop, invoking fn per
 *                                 test case
 *   - hegel_draw_int/bool/bytes   call hegel_generate_* against the
 *                                 current (ctx, test case)
 *   - assert(x) inside a body     on failure, records the failing
 *                                 expression as the origin and longjmps
 *                                 back to the loop, which marks the case
 *                                 INTERESTING so the engine shrinks it
 *   - a draw hitting HEGEL_E_STOP_TEST (choice-budget exhaustion)
 *                                 longjmps back and marks OVERRUN
 *
 * A test TU includes <assert.h> for any real invariant asserts it wants
 * to keep as hard aborts, then includes this header, which redefines
 * assert() to the shrink-friendly form for the property bodies.  There
 * is no server, no libcbor, no zlib: just -lhegel.
 */
#ifndef HEGEL_COMPAT_H
#define HEGEL_COMPAT_H

#include <hegel.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- per-run/per-case state the shim threads to the draw shims ---- */

typedef struct {
	hegel_context_t *ctx;
} hegel_session;

/* Old callers pass a test-case pointer around; under the new API the
 * live (ctx, tc) plus the bail-out jmp_buf are what a draw needs, so we
 * stash them in a thread-local the shims read.  The public token type
 * stays an opaque struct so the bodies' `hegel_test_case *tc` still
 * compiles. */
typedef struct hegel_test_case hegel_test_case; /* opaque token */

typedef struct {
	hegel_context_t *ctx;
	hegel_test_case_t *tc;
	jmp_buf bail;
	int status;		/* HEGEL_STATUS_* to mark on bail */
	const char *origin;	/* failing expression, for INTERESTING */
} hegel_compat_state;

/* Defined once in the TU via HEGEL_COMPAT_IMPL (see bottom). */
extern hegel_compat_state *hegel_cur;

/* ---- settings shim ---- */

typedef struct {
	uint32_t max_examples;
	int verbosity;
	uint64_t seed;
	int have_seed;
} hegel_settings;

#define HEGEL_DEFAULT_SETTINGS ((hegel_settings) { 200, 0, 0, 0 })

/* ---- results shim ---- */

typedef struct {
	int passed;
} hegel_results;

/* ---- generator "descriptors" (old API took a value; new takes bounds) ---- */

typedef struct {
	int64_t lo, hi;
} hegel_int_gen;
typedef struct {
	uint64_t lo, hi;
} hegel_bytes_gen;
typedef struct {
	double p;
} hegel_bool_gen;

static inline hegel_int_gen
hegel_integers(int64_t lo, int64_t hi)
{
	return ((hegel_int_gen) { lo, hi });
}
static inline hegel_bytes_gen
hegel_binary(uint64_t lo, uint64_t hi)
{
	return ((hegel_bytes_gen) { lo, hi });
}
static inline hegel_bool_gen
hegel_booleans(void)
{
	return ((hegel_bool_gen) { 0.5 });
}

/* ---- draw shims: call the FFI, bail on STOP_TEST ---- */

static inline int64_t
hegel_draw_int(hegel_test_case *tc, hegel_int_gen g)
{
	(void)tc;
	int64_t v = 0;
	hegel_result_t rc =
	    hegel_generate_integer(hegel_cur->ctx, hegel_cur->tc, g.lo, g.hi, &v);
	if (rc == HEGEL_E_STOP_TEST) {
		hegel_cur->status = HEGEL_STATUS_OVERRUN;
		longjmp(hegel_cur->bail, 1);
	}
	if (rc != HEGEL_OK) {
		/* A non-STOP error is a harness bug; treat the case as
		 * inconclusive rather than a property failure. */
		hegel_cur->status = HEGEL_STATUS_INVALID;
		longjmp(hegel_cur->bail, 1);
	}
	return (v);
}

static inline bool
hegel_draw_bool(hegel_test_case *tc, hegel_bool_gen g)
{
	(void)tc;
	bool v = false;
	hegel_result_t rc = hegel_generate_boolean(hegel_cur->ctx, hegel_cur->tc,
	    g.p, false, false, &v);
	if (rc == HEGEL_E_STOP_TEST) {
		hegel_cur->status = HEGEL_STATUS_OVERRUN;
		longjmp(hegel_cur->bail, 1);
	}
	if (rc != HEGEL_OK) {
		hegel_cur->status = HEGEL_STATUS_INVALID;
		longjmp(hegel_cur->bail, 1);
	}
	return (v);
}

/* Returns a malloc'd copy the caller frees with free(), matching how the
 * old hegel_draw_bytes buffer was consumed. */
static inline uint8_t *
hegel_draw_bytes(hegel_test_case *tc, hegel_bytes_gen g, size_t *out_len)
{
	(void)tc;
	hegel_generate_bytes_result_t r = { 0 };
	hegel_result_t rc = hegel_generate_bytes(hegel_cur->ctx, hegel_cur->tc,
	    g.lo, g.hi, &r);
	if (rc == HEGEL_E_STOP_TEST) {
		hegel_cur->status = HEGEL_STATUS_OVERRUN;
		longjmp(hegel_cur->bail, 1);
	}
	if (rc != HEGEL_OK) {
		hegel_cur->status = HEGEL_STATUS_INVALID;
		longjmp(hegel_cur->bail, 1);
	}
	uint8_t *copy = (uint8_t *)malloc(r.len ? r.len : 1);
	if (copy != NULL && r.len)
		memcpy(copy, r.data, r.len);
	*out_len = r.len;
	hegel_generate_bytes_result_free(hegel_cur->ctx, &r);
	return (copy);
}

/* ---- the property-body assert(): shrink-friendly failure signal ---- */

#ifdef assert
#undef assert
#endif
#define assert(cond)                                                          \
	do {                                                                  \
		if (!(cond)) {                                                \
			hegel_cur->status = HEGEL_STATUS_INTERESTING;         \
			hegel_cur->origin = #cond;                            \
			longjmp(hegel_cur->bail, 1);                          \
		}                                                             \
	} while (0)

/* ---- session + run harness ---- */

static inline hegel_session *
hegel_session_new(void)
{
	hegel_session *s = (hegel_session *)calloc(1, sizeof(*s));
	if (s != NULL)
		s->ctx = hegel_context_new();
	return (s);
}

static inline void
hegel_session_free(hegel_session *s)
{
	if (s == NULL)
		return;
	if (s->ctx != NULL)
		hegel_context_free(s->ctx);
	free(s);
}

/*
 * Drive one property to completion.  Returns hegel_results with
 * passed = 1 iff the engine's final run status is PASSED.  On failure it
 * prints the shrunk counterexample's origin (the failing expression),
 * mirroring the old harness's diagnostic.
 */
static inline hegel_results
hegel_run_test(hegel_session *s, void (*fn)(hegel_test_case *, void *),
    void *user_data, const hegel_settings *settings)
{
	hegel_results out = { 0 };
	hegel_context_t *ctx = s->ctx;

	hegel_settings_t *set = NULL;
	if (hegel_settings_new(ctx, &set) != HEGEL_OK)
		return (out);
	hegel_settings_set_test_cases(ctx, set,
	    settings ? settings->max_examples : 200);
	/* Deterministic, no on-disk example database (tests are hermetic). */
	hegel_settings_set_database(ctx, set, "");
	hegel_settings_set_derandomize(ctx, set, true);
	if (settings && settings->have_seed)
		hegel_settings_set_seed(ctx, set, settings->seed, true);

	hegel_run_t *run = NULL;
	if (hegel_run_start(ctx, set, NULL, NULL, &run) != HEGEL_OK) {
		hegel_settings_free(ctx, set);
		return (out);
	}

	for (;;) {
		hegel_test_case_t *tc = NULL;
		if (hegel_next_test_case(ctx, run, &tc) != HEGEL_OK)
			break;
		if (tc == NULL)
			break; /* run finished */

		hegel_compat_state st = { 0 };
		st.ctx = ctx;
		st.tc = tc;
		st.status = HEGEL_STATUS_VALID;
		st.origin = NULL;
		hegel_cur = &st;

		if (setjmp(st.bail) == 0) {
			fn((hegel_test_case *)tc, user_data);
			/* Body returned normally: property held. */
			hegel_mark_complete(ctx, tc, HEGEL_STATUS_VALID, NULL);
		} else {
			hegel_mark_complete(ctx, tc, st.status, st.origin);
		}
		hegel_cur = NULL;
		hegel_test_case_free(ctx, tc);
	}

	hegel_run_result_t *result = NULL;
	if (hegel_run_result(ctx, run, &result) == HEGEL_OK) {
		hegel_run_status_t status = HEGEL_RUN_STATUS_ERROR;
		hegel_run_result_status(ctx, result, &status);
		out.passed = (status == HEGEL_RUN_STATUS_PASSED);
		if (!out.passed) {
			size_t nf = 0;
			hegel_run_result_failure_count(ctx, result, &nf);
			for (size_t i = 0; i < nf; i++) {
				hegel_failure_t *f = NULL;
				if (hegel_run_result_failure(ctx, result, i, &f) !=
				    HEGEL_OK)
					continue;
				const char *origin = NULL;
				hegel_failure_origin(ctx, f, &origin);
				fprintf(stderr,
				    "  counterexample: %s\n",
				    origin ? origin : "(no origin)");
				hegel_failure_free(ctx, f);
			}
		}
		hegel_run_result_free(ctx, result);
	}

	hegel_run_free(ctx, run);
	hegel_settings_free(ctx, set);
	return (out);
}

static inline void
hegel_results_free(hegel_results *r)
{
	(void)r; /* no owned resources in the shim's results */
}

/* Define the thread-local run state in exactly one TU. */
#ifdef HEGEL_COMPAT_IMPL
hegel_compat_state *hegel_cur = NULL;
#endif

#endif /* HEGEL_COMPAT_H */
