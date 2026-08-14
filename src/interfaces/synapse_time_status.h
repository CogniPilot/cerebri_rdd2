/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared resolver for the synapse.types.TimeStatus wire field. It reports
 * whether a producer's published timestamps are on the shared gPTP grandmaster
 * timescale, and returns the boot-to-PHC offset that carries a boot-clock stamp
 * onto it. Every cerebri TX producer (the communications image today, the
 * flight publishing path next) resolves the domain the same way, so a consumer
 * reads one honest label across the whole vehicle.
 */
#ifndef RDD2_INTERFACES_SYNAPSE_TIME_STATUS_H_
#define RDD2_INTERFACES_SYNAPSE_TIME_STATUS_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#if defined(CONFIG_NET_GPTP)
#include <zephyr/net/gptp.h>
#endif

#include <synapse/types_reader.h>

/*
 * Resolve this node's clock discipline state and, when it is on the shared
 * timescale, the offset that carries a boot-clock timestamp onto it.
 *
 * gptp_event_capture reads the gPTP-disciplined PHC: the slave clock on a
 * follower node, or the node's own served clock on the grandmaster once that
 * served clock is disciplined. Sensor sample stamps come from the free-running
 * kernel boot clock, a different clock from the PHC, so a boot-clock stamp is
 * placed on the shared timescale by adding offset = phc_now - boot_now read at
 * the same instant. Every stamp in one published message is shifted by that one
 * offset, so the spacing between stamps is preserved while their absolute
 * reference moves onto the GNSS-traceable domain.
 *
 * The returned status names the domain honestly: GptpSynced while a grandmaster
 * is present, GptpHoldover once one has been lost after a prior lock (the PHC
 * then coasts on the last learned rate), and LocalFreerun before any lock or
 * when the gPTP stack is absent, where the offset stays zero and the stamps
 * remain node-local monotonic boot time.
 *
 * The caller owns the ever-synced latch behind time_ever_synced. It is set the
 * first time a grandmaster is seen and never cleared, so a later loss is
 * published as holdover rather than as never-synchronized.
 */
static inline synapse_types_TimeStatus_enum_t
synapse_time_status_resolve(bool *time_ever_synced, int64_t *offset_ns)
{
	*offset_ns = 0;

#if defined(CONFIG_NET_GPTP)
	struct net_ptp_time phc;
	bool gm_present = false;

	/* nonzero: no disciplined PHC to read. Either gPTP is not up yet, or
	 * this node is an as-yet-undisciplined grandmaster. Node-local time is
	 * all there is to publish.
	 */
	if (gptp_event_capture(&phc, &gm_present) != 0) {
		return synapse_types_TimeStatus_LocalFreerun;
	}

	/* never disciplined: the PHC carries no traceable meaning to claim */
	if (!gm_present && !*time_ever_synced) {
		return synapse_types_TimeStatus_LocalFreerun;
	}

	int64_t phc_now_ns =
		(int64_t)phc.second * 1000000000LL + (int64_t)phc.nanosecond;
	int64_t boot_now_ns = k_ticks_to_ns_floor64(k_uptime_ticks());

	*offset_ns = phc_now_ns - boot_now_ns;

	if (gm_present) {
		*time_ever_synced = true;
		return synapse_types_TimeStatus_GptpSynced;
	}

	/* was synced, grandmaster lost: the disciplined PHC now coasts */
	return synapse_types_TimeStatus_GptpHoldover;
#else
	ARG_UNUSED(time_ever_synced);
	return synapse_types_TimeStatus_LocalFreerun;
#endif
}

/*
 * Full-precision monotonic boot-clock nanoseconds. Reads the tick counter
 * directly rather than scaling k_uptime_get, which is quantized to
 * milliseconds and discards the low nanoseconds a PHC cross-check needs.
 */
static inline uint64_t synapse_time_boot_ns(void)
{
	return (uint64_t)k_ticks_to_ns_floor64(k_uptime_ticks());
}

/*
 * Place a boot-clock nanosecond stamp on the resolved timescale by adding the
 * offset the resolver returned. In LocalFreerun the offset is zero and the
 * stamp stays node-local monotonic boot time.
 */
static inline uint64_t synapse_time_apply_offset(uint64_t boot_ns, int64_t offset_ns)
{
	return (uint64_t)((int64_t)boot_ns + offset_ns);
}

#endif /* RDD2_INTERFACES_SYNAPSE_TIME_STATUS_H_ */
