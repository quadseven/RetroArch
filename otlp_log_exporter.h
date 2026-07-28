/*  RetroArch - OTLP log exporter
 *
 *  Exports RetroArch log lines over OTLP/HTTP so a device can be diagnosed
 *  without pulling its storage. Vendor neutral: OTLP is JSON over HTTP, so
 *  this talks to any OTLP log endpoint, whether that is an OpenTelemetry
 *  Collector on your own network or a hosted backend.
 *
 *  Configuration follows the OpenTelemetry environment variable conventions,
 *  read from files because a console has no environment to set. Paths are
 *  relative to the port directory, e.g. on Switch:
 *
 *    sdmc:/retroarch/otel-endpoint             OTEL_EXPORTER_OTLP_ENDPOINT
 *    sdmc:/retroarch/otel-headers              OTEL_EXPORTER_OTLP_HEADERS
 *    sdmc:/retroarch/otel-resource-attributes  OTEL_RESOURCE_ATTRIBUTES
 *
 *  With no endpoint file this does nothing: no thread, no allocation.
 *
 *  Uses only what RetroArch already links: net_http for transport and
 *  rthreads for the worker, so it adds no dependency on any platform.
 */

#ifndef __OTLP_LOG_EXPORTER_H
#define __OTLP_LOG_EXPORTER_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/**
 * otlp_log_exporter_init:
 * @config_dir : directory holding the otel-* files.
 *
 * Reads configuration and, if an endpoint is set, starts the worker.
 *
 * @return true if exporting is on.
 */
bool otlp_log_exporter_init(const char *config_dir);

/**
 * otlp_log_exporter_enabled:
 *
 * Cheap probe so callers can skip formatting a line that would be discarded.
 *
 * @return true if the exporter is running.
 */
bool otlp_log_exporter_enabled(void);

/**
 * otlp_log_exporter_log:
 * @tag  : RetroArch log tag, used to derive severity.
 * @line : the formatted log line.
 *
 * Queues one record. Safe to call from any thread, and safe to call when
 * the exporter is not running, in which case it does nothing.
 *
 * Must never be called from the exporter's own worker thread: nothing on
 * that path may log, or a failed export would generate the record that
 * causes the next failure.
 */
void otlp_log_exporter_log(const char *tag, const char *line);

/**
 * otlp_log_exporter_deinit:
 *
 * Stops the worker after a final flush attempt.
 */
void otlp_log_exporter_deinit(void);

/**
 * otlp_log_exporter_stats:
 *
 * Counters, for reporting from the main thread. The exporter cannot report
 * them itself without recursing into the logger. Any pointer may be NULL.
 */
void otlp_log_exporter_stats(unsigned *accepted, unsigned *sent,
      unsigned *dropped, unsigned *failures, const char **last_error);

RETRO_END_DECLS

#endif
