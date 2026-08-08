/* OTLP metrics for the Switch build.
 *
 * Why this exists
 * ---------------
 * The logs this fork already ships answer "what happened". They cannot answer
 * "how fast", and the questions actually worth asking of RetroArch on a Switch
 * are all rates: why the menu runs at 11fps, whether a core is dropping frames,
 * whether memory is climbing before a fatal. None of those are visible in a log
 * line, and every one of them is a number RetroArch already computes.
 *
 * Deliberately small. There is no aggregation, no histogram and no exemplar
 * support: a handful of named int64 series, sampled on the log worker's own
 * schedule. A metrics pipeline that is complicated enough to have its own bugs
 * is worse than none, because it produces confident numbers that are wrong.
 *
 * DELTA, not cumulative
 * ---------------------
 * Datadog's OTLP intake accepts only delta temporality
 * (aggregationTemporality 1). Cumulative sums are dropped silently: no error,
 * no rejection, the data simply never appears. Counters here are therefore
 * exported as the change since the previous export and reset, and gauges are
 * exported as instantaneous values, which carry no temporality at all.
 */

#ifndef OTLP_METRICS_H
#define OTLP_METRICS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Records the current value of a gauge, e.g. frames per second.
 *
 * Last value wins within an interval. Safe to call before init, in which case
 * it does nothing, so callers never need to check whether export is enabled.
 */
void otlp_metric_gauge(const char *name, int64_t value);

/**
 * Adds to a counter, exported as the delta since the last export and then
 * reset. Safe to call before init.
 */
void otlp_metric_add(const char *name, int64_t delta);

/**
 * Builds the OTLP JSON body for everything recorded since the last call, and
 * resets counter deltas. Returns NULL when there is nothing to send.
 *
 * The caller owns the returned buffer and must free() it.
 *
 * Counters are reset here rather than after a successful POST. That loses one
 * interval of counts if the POST fails, which is the correct trade for a
 * gauge-heavy set: holding deltas across a failure makes the next interval
 * report a spike that never happened, and a spike that never happened is worse
 * than a gap that did.
 */
char *otlp_metrics_build_payload(const char *resource_attrs);

/** True once at least one series has been recorded. */
bool otlp_metrics_have_data(void);

/**
 * Samples what RetroArch already knows: frame rate, frame time and frame
 * count. Called from the exporter's worker, not from the frame loop.
 *
 * On the frame loop this would be the single hottest path in the program, and
 * an exporter that costs frames while measuring frames is self-defeating.
 * Everything read here is a value RetroArch maintains anyway.
 */
void otlp_metrics_sample_video(void);

/** Samples process-level numbers: free heap and thread count. */
void otlp_metrics_sample_process(void);

#endif
