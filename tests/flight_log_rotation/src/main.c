/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Flight-log rotation regression suite.
 *
 * Runs the production flight_log.c capture and writer threads, ring, session
 * rotation, and standing-reservation pool on native_sim against a fake card that
 * injects a multi-second extent-reservation stall. The offered load mirrors the
 * recorded flight per-channel rates. Two size-triggered rotations are driven
 * with the stall active:
 *
 *   - the deferred build (default) waits for a standing reservation and a quiet
 *     ring, rotates by rename, and must drop no frames;
 *   - the eager build (CONFIG_RDD2_FLIGHT_LOG_EAGER_ROTATE) rotates at the byte
 *     threshold, hits the inline reservation stall while the ring is under
 *     load, and must overflow the ring and drop frames.
 *
 * A final phase exercises the manual-rotate path against an idle ring, which
 * must never drop frames regardless of build, and then the pool contract: the
 * standing reservations survive a stop, the next start opens its session by
 * renaming one rather than reserving inline, and the builder refills the
 * consumed slot in the background with no inline reservation anywhere.
 */

#include "harness.h"

#include "flight_log.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define RING_BYTES ((uint32_t)CONFIG_RDD2_FLIGHT_LOG_RING_BYTES)

static struct rdd2_flight_log_status snapshot(void)
{
	struct rdd2_flight_log_status s;

	rdd2_flight_log_status_get(&s);
	return s;
}

static bool wait_for_active(int max_ms)
{
	for (int waited = 0; waited < max_ms; waited += 20) {
		if (snapshot().active) {
			return true;
		}
		k_sleep(K_MSEC(20));
	}
	return snapshot().active;
}

static bool wait_for_index(uint32_t target, int max_ms)
{
	for (int waited = 0; waited < max_ms; waited += 20) {
		struct rdd2_flight_log_status s = snapshot();

		if (s.session_index >= target) {
			return true;
		}
		if ((waited % 1000) == 0) {
			TC_PRINT("t=%dms index=%u bytes=%llu drops=%u ring_hw=%u "
				 "reservations=%u/%u\n",
				 waited, s.session_index, (unsigned long long)s.bytes_written,
				 s.dropped_frames, s.ring_high_water,
				 rdd2_test_ready_reservations(), s.reservation_slots);
		}
		k_sleep(K_MSEC(20));
	}
	return snapshot().session_index >= target;
}

static bool wait_for(bool (*predicate)(void), int max_ms)
{
	for (int waited = 0; waited < max_ms; waited += 20) {
		if (predicate()) {
			return true;
		}
		k_sleep(K_MSEC(20));
	}
	return predicate();
}

static bool session_idle(void)
{
	return !snapshot().active;
}

static bool two_reservations_ready(void)
{
	return rdd2_test_ready_reservations() >= 2U;
}

static bool pool_full(void)
{
	return rdd2_test_ready_reservations() >= snapshot().reservation_slots;
}

ZTEST(flight_log_rotation, test_rotation_ring_integrity)
{
	struct rdd2_flight_log_status s;
	struct rdd2_flight_log_status before;
	struct rdd2_flight_log_status after;
	uint32_t expands;
	uint32_t renames;

	/* Inject a three-second reservation stall, matching a mid-flight f_expand
	 * burst on a real card, and run the offered load. */
	rdd2_test_load_paused = false;
	rdd2_test_expand_stall_ms = 3800U;

	zassert_true(wait_for_active(20000), "logging session never opened");

	/* Drive past at least two size-triggered rotations. */
	zassert_true(wait_for_index(2U, 120000), "did not reach two rotations");

	s = snapshot();
	TC_PRINT("after >=2 rotations: session_index=%u dropped_frames=%u "
		 "ring_high_water=%u/%u bytes_written=%llu reservations=%u/%u\n",
		 s.session_index, s.dropped_frames, s.ring_high_water, RING_BYTES,
		 (unsigned long long)s.bytes_written, rdd2_test_ready_reservations(),
		 s.reservation_slots);

#if defined(CONFIG_RDD2_FLIGHT_LOG_EAGER_ROTATE)
	/* Eager rotation stalls the writer inline while capture keeps producing:
	 * the ring pins full and frames are lost at every rotation. */
	zassert_true(s.dropped_frames > 1000U,
		     "eager rotation should drop frames, saw %u", s.dropped_frames);
	zassert_true(s.ring_high_water > (RING_BYTES * 3U) / 4U,
		     "eager rotation should pin the ring, high water %u/%u",
		     s.ring_high_water, RING_BYTES);
#else
	/* Deferred rotation swaps a pre-built extent by rename on a quiet ring:
	 * no reservation stall reaches the streaming path, so nothing is lost. */
	zassert_equal(s.dropped_frames, 0U,
		      "deferred rotation dropped %u frames", s.dropped_frames);
	zassert_true(s.ring_high_water < RING_BYTES / 2U,
		     "deferred rotation ring high water %u should stay well below %u",
		     s.ring_high_water, RING_BYTES);
#endif

	/* Manual rotate against an idle ring: quiesce the bus, let the writer
	 * drain, then rotate through the bypass path. No frames should drop. */
	rdd2_test_load_paused = true;
	k_sleep(K_MSEC(500));

	before = snapshot();
	rdd2_flight_log_request_rotate();
	zassert_true(wait_for_index(before.session_index + 1U, 30000),
		     "manual rotate did not advance the session");

	after = snapshot();
	TC_PRINT("manual idle rotate: index %u->%u dropped %u->%u ring_high_water=%u\n",
		 before.session_index, after.session_index, before.dropped_frames,
		 after.dropped_frames, after.ring_high_water);

	zassert_equal(after.dropped_frames, before.dropped_frames,
		      "idle manual rotate dropped %u frames",
		      after.dropped_frames - before.dropped_frames);

	/* Standing pool: let the builder put two reservations on the card, then stop.
	 * Both must survive, and the next start must open its session by renaming one
	 * instead of paying the inline reservation stall. */
	zassert_true(wait_for(two_reservations_ready, 60000),
		     "the pool never reached two standing reservations");

	rdd2_flight_log_request_stop();
	zassert_true(wait_for(session_idle, 30000), "session never stopped");
	zassert_true(rdd2_test_ready_reservations() >= 2U,
		     "stop removed standing reservations, %u left",
		     rdd2_test_ready_reservations());

	before = snapshot();
	expands = rdd2_test_prealloc_count;
	renames = rdd2_test_rename_count;

	rdd2_flight_log_request_start();
	zassert_true(wait_for_active(30000), "session never restarted");

	TC_PRINT("restart from the pool: index %u->%u expands %u->%u renames %u->%u "
		 "reservations=%u/%u\n",
		 before.session_index, snapshot().session_index, expands,
		 rdd2_test_prealloc_count, renames, rdd2_test_rename_count,
		 rdd2_test_ready_reservations(), snapshot().reservation_slots);

	zassert_equal(rdd2_test_prealloc_count, expands,
		      "restart reserved inline instead of renaming a standing one");
	zassert_equal(rdd2_test_rename_count, renames + 1U,
		      "restart did not rename a standing reservation into the session");

	/* The consumed slot is refilled in the background, one reservation at a
	 * time, and the refill never falls back to an inline reservation. */
	zassert_true(wait_for(pool_full, 120000),
		     "the pool did not refill, %u of %u standing",
		     rdd2_test_ready_reservations(), snapshot().reservation_slots);
	zassert_equal(rdd2_test_prealloc_count, expands,
		      "refilling the pool reserved inline");

	TC_PRINT("pool refilled to %u/%u with %u inline reservations total\n",
		 rdd2_test_ready_reservations(), snapshot().reservation_slots,
		 rdd2_test_prealloc_count);
}

ZTEST_SUITE(flight_log_rotation, NULL, NULL, NULL, NULL, NULL);
