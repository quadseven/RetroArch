/* OTLP metrics for the Switch build. See otlp_metrics.h for why.
 *
 * Sampled from the log exporter's worker thread, never from the frame loop.
 * Measuring frames must not cost frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <rthreads/rthreads.h>
#include <features/features_cpu.h>

#include "otlp_metrics.h"

#include "gfx/video_driver.h"
#ifdef HAVE_THREADS
#include "gfx/video_thread_wrapper.h"
#endif

#ifdef __SWITCH__
#include <malloc.h>
#endif

/* Enough for everything below with room to add, and small enough that a linear
 * scan per record is cheaper than any index would be. */
#define OTLP_MAX_SERIES 32
#define OTLP_MAX_NAME   64

typedef struct
{
   char     name[OTLP_MAX_NAME];
   int64_t  value;      /* gauge: current value. counter: delta this interval */
   bool     used;
   bool     is_counter;
} otlp_series_t;

static otlp_series_t otlp_series[OTLP_MAX_SERIES];
static slock_t      *otlp_metrics_lock;
static bool          otlp_metrics_any;

/* Start of the current delta interval, in Unix nanoseconds. Every export
 * closes one interval and opens the next; a delta point whose start and end are
 * the same instant is meaningless, so this is only ever advanced on export. */
static int64_t       otlp_interval_start_ns;

/* ------------------------------------------------------------------ */

static slock_t *otlp_metrics_get_lock(void)
{
   /* Created on first use rather than in an init function. The producers here
    * are called from RetroArch code that runs long before any exporter starts,
    * and a metric recorded before init should be dropped quietly rather than
    * crash. */
   if (!otlp_metrics_lock)
      otlp_metrics_lock = slock_new();
   return otlp_metrics_lock;
}

static otlp_series_t *otlp_find_or_create(const char *name, bool is_counter)
{
   unsigned i;
   int free_slot = -1;

   for (i = 0; i < OTLP_MAX_SERIES; i++)
   {
      if (otlp_series[i].used)
      {
         if (strcmp(otlp_series[i].name, name) == 0)
            return &otlp_series[i];
      }
      else if (free_slot < 0)
         free_slot = (int)i;
   }

   /* Full. Dropping a new series is correct: the alternative is evicting one
    * that is already being graphed, which turns a missing metric into a
    * flapping one. */
   if (free_slot < 0)
      return NULL;

   strlcpy(otlp_series[free_slot].name, name, OTLP_MAX_NAME);
   otlp_series[free_slot].used       = true;
   otlp_series[free_slot].is_counter = is_counter;
   otlp_series[free_slot].value      = 0;
   otlp_metrics_any                  = true;
   return &otlp_series[free_slot];
}

void otlp_metric_gauge(const char *name, int64_t value)
{
   slock_t *lock;
   otlp_series_t *s;

   if (!name)
      return;

   lock = otlp_metrics_get_lock();
   if (!lock)
      return;

   slock_lock(lock);
   if ((s = otlp_find_or_create(name, false)))
      s->value = value;
   slock_unlock(lock);
}

void otlp_metric_add(const char *name, int64_t delta)
{
   slock_t *lock;
   otlp_series_t *s;

   if (!name)
      return;

   lock = otlp_metrics_get_lock();
   if (!lock)
      return;

   slock_lock(lock);
   if ((s = otlp_find_or_create(name, true)))
      s->value += delta;
   slock_unlock(lock);
}

bool otlp_metrics_have_data(void)
{
   return otlp_metrics_any;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */

void otlp_metrics_sample_video(void)
{
   double refresh_rate = 0.0;
   double deviation    = 0.0;
   unsigned samples    = 0;

#ifdef HAVE_THREADS
   /* The threaded path first, because it is the one this console actually
    * runs. video_monitor_fps_statistics() documents itself as returning false
    * when threaded video is enabled, so on a threaded build it reports nothing
    * at all and an exporter built only on it would ship an empty payload
    * forever while looking correct.
    *
    * hit_count and miss_count are frames the wrapper accepted and frames it
    * dropped, which is exactly the "Frames pushed / Frames dropped" pair
    * RetroArch already prints at exit. */
   if (video_driver_is_threaded())
   {
      thread_video_t *thr = (thread_video_t*)video_driver_get_ptr();

      if (thr)
      {
         static unsigned last_hits;
         static unsigned last_misses;
         unsigned hits   = thr->hit_count;
         unsigned misses = thr->miss_count;

         /* Deltas, and guarded against the counters going backwards, which
          * they do when the driver is torn down and rebuilt on a core change.
          * Without the guard a core switch reports a single enormous negative
          * frame count. */
         if (hits >= last_hits)
            otlp_metric_add("retroarch.frames_pushed", (int64_t)(hits - last_hits));
         if (misses >= last_misses)
            otlp_metric_add("retroarch.frames_dropped", (int64_t)(misses - last_misses));

         last_hits   = hits;
         last_misses = misses;

         otlp_metric_gauge("retroarch.video_threaded", 1);
      }
      return;
   }
   otlp_metric_gauge("retroarch.video_threaded", 0);
#endif

   /* Non-threaded: RetroArch computes the statistics itself. Reported in
    * millihertz because the transport carries int64 and a frame rate rounded
    * to a whole number cannot show the difference between 59.94 and 60. */
   if (video_monitor_fps_statistics(&refresh_rate, &deviation, &samples))
   {
      otlp_metric_gauge("retroarch.fps_millihz",
            (int64_t)(refresh_rate * 1000.0));
      otlp_metric_gauge("retroarch.fps_deviation_millihz",
            (int64_t)(deviation * 1000.0));
      otlp_metric_gauge("retroarch.fps_samples", (int64_t)samples);
   }
}

void otlp_metrics_sample_process(void)
{
#ifdef __SWITCH__
   /* mallinfo walks the free list, so this is not free. At one sample per
    * export interval that is irrelevant, and on the frame loop it would not
    * be. */
   struct mallinfo mi = mallinfo();

   otlp_metric_gauge("retroarch.heap_used_bytes",  (int64_t)mi.uordblks);
   otlp_metric_gauge("retroarch.heap_free_bytes",  (int64_t)mi.fordblks);
   otlp_metric_gauge("retroarch.heap_arena_bytes", (int64_t)mi.arena);
#endif
}

/* ------------------------------------------------------------------ */
/* Payload                                                             */
/* ------------------------------------------------------------------ */

static void otlp_m_append(char *out, size_t out_len, const char *text)
{
   size_t o = strlen(out);
   size_t n = strlen(text);

   if (o + n + 1 > out_len)
      n = (out_len > o + 1) ? out_len - o - 1 : 0;
   if (n == 0)
      return;
   memcpy(out + o, text, n);
   out[o + n] = '\0';
}

static void otlp_m_append_attrs(char *out, size_t cap, const char *attrs)
{
   const char *p = attrs;
   bool first    = true;

   while (p && *p)
   {
      const char *comma = strchr(p, ',');
      const char *eq;
      size_t item_len   = comma ? (size_t)(comma - p) : strlen(p);
      char item[256];
      char key[128];

      if (item_len >= sizeof(item))
         item_len = sizeof(item) - 1;
      memcpy(item, p, item_len);
      item[item_len] = '\0';

      if ((eq = strchr(item, '=')))
      {
         size_t klen = (size_t)(eq - item);

         if (klen < sizeof(key))
         {
            memcpy(key, item, klen);
            key[klen] = '\0';

            if (!first)
               otlp_m_append(out, cap, ",");
            first = false;

            otlp_m_append(out, cap, "{\"key\":\"");
            otlp_m_append(out, cap, key);
            otlp_m_append(out, cap, "\",\"value\":{\"stringValue\":\"");
            otlp_m_append(out, cap, eq + 1);
            otlp_m_append(out, cap, "\"}}");
         }
      }

      if (!comma)
         break;
      p = comma + 1;
   }
}

char *otlp_metrics_build_payload(const char *resource_attrs)
{
   /* Bounded: OTLP_MAX_SERIES points, each well under 512 bytes, plus the
    * resource block. */
   const size_t cap = 2048 + OTLP_MAX_SERIES * 512;
   char    *out     = (char*)malloc(cap);
   slock_t *lock    = otlp_metrics_get_lock();
   int64_t  now_ns;
   int64_t  start_ns;
   unsigned i;
   bool     any     = false;
   char     num[64];

   if (!out)
      return NULL;
   out[0] = '\0';

   /* Microseconds from RetroArch's own clock, promoted to nanoseconds. This is
    * wall clock, which is what OTLP wants; the monotonic tick would place every
    * point in 1970. */
   now_ns = (int64_t)cpu_features_get_time_usec() * 1000;

   if (lock)
      slock_lock(lock);

   start_ns = otlp_interval_start_ns ? otlp_interval_start_ns : now_ns;

   for (i = 0; i < OTLP_MAX_SERIES; i++)
   {
      if (otlp_series[i].used)
      {
         any = true;
         break;
      }
   }

   if (!any)
   {
      if (lock)
         slock_unlock(lock);
      free(out);
      return NULL;
   }

   otlp_m_append(out, cap, "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[");
   otlp_m_append_attrs(out, cap, resource_attrs ? resource_attrs : "");
   otlp_m_append(out, cap, "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"retroarch\"},\"metrics\":[");

   {
      bool first = true;

      for (i = 0; i < OTLP_MAX_SERIES; i++)
      {
         if (!otlp_series[i].used)
            continue;

         if (!first)
            otlp_m_append(out, cap, ",");
         first = false;

         otlp_m_append(out, cap, "{\"name\":\"");
         otlp_m_append(out, cap, otlp_series[i].name);
         otlp_m_append(out, cap, "\",\"unit\":\"1\",");

         if (otlp_series[i].is_counter)
         {
            /* aggregationTemporality 1 is DELTA. Datadog's OTLP intake accepts
             * only delta sums and drops cumulative ones silently: no error, no
             * rejection, the series simply never appears. */
            otlp_m_append(out, cap,
                  "\"sum\":{\"aggregationTemporality\":1,\"isMonotonic\":true,"
                  "\"dataPoints\":[{\"asInt\":\"");
         }
         else
            otlp_m_append(out, cap, "\"gauge\":{\"dataPoints\":[{\"asInt\":\"");

         snprintf(num, sizeof(num), "%lld", (long long)otlp_series[i].value);
         otlp_m_append(out, cap, num);

         otlp_m_append(out, cap, "\",\"startTimeUnixNano\":\"");
         snprintf(num, sizeof(num), "%lld", (long long)start_ns);
         otlp_m_append(out, cap, num);

         otlp_m_append(out, cap, "\",\"timeUnixNano\":\"");
         snprintf(num, sizeof(num), "%lld", (long long)now_ns);
         otlp_m_append(out, cap, num);
         otlp_m_append(out, cap, "\"}]}}");

         /* Counters reset, gauges persist. A gauge that stops being updated
          * should keep reporting its last known value, because "the decoder is
          * still alive" is information; a counter that keeps its total would
          * double-count every interval. */
         if (otlp_series[i].is_counter)
            otlp_series[i].value = 0;
      }
   }

   otlp_m_append(out, cap, "]}]}]}");

   otlp_interval_start_ns = now_ns;

   if (lock)
      slock_unlock(lock);

   return out;
}
