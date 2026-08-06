/*  RetroArch - OTLP log exporter
 *  See otlp_log_exporter.h for what this is and how it is configured.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <net/net_http.h>
#include <string/stdstring.h>

#include "otlp_log_exporter.h"

/* Defined below; used by init before its definition. */
static void otlp_write_status(const char *fmt, ...);
#include "file_path_special.h"

#ifdef __SWITCH__
#include <switch.h>

/*
 * Wall clock on this console is whole seconds. time() is all that newlib
 * offers, so every record written inside the same second used to carry an
 * identical timestamp and could not be put in order at all. For a crash where
 * the interesting events are 3ms apart that is the difference between a
 * usable log and a pile of lines.
 *
 * armGetSystemTick is a monotonic counter with a known frequency. Anchor it
 * to time() once at startup and interpolate from there: the wall clock stays
 * correct to the second and everything within that second is ordered properly.
 * The anchor is only ever read, never re-taken, so records cannot go backwards
 * if the RTC is adjusted underneath us.
 */
static int64_t otlp_epoch_ns_at_anchor;
static uint64_t otlp_tick_at_anchor;

static void otlp_clock_anchor(void)
{
   otlp_epoch_ns_at_anchor = (int64_t)time(NULL) * 1000000000LL;
   otlp_tick_at_anchor     = armGetSystemTick();
}

static int64_t otlp_now_ns(void)
{
   if (!otlp_tick_at_anchor)
      return (int64_t)time(NULL) * 1000000000LL;
   return otlp_epoch_ns_at_anchor +
          (int64_t)armTicksToNs(armGetSystemTick() - otlp_tick_at_anchor);
}

/* Which thread emitted a line. Without this, two events milliseconds apart
 * are indistinguishable from one thread doing two things and two threads
 * racing, and that distinction is usually the entire question. */
static uint64_t otlp_thread_id(void)
{
   u64 tid = 0;
   if (R_FAILED(svcGetThreadId(&tid, threadGetCurHandle())))
      return 0;
   return tid;
}
#else
static void otlp_clock_anchor(void) { }
static int64_t otlp_now_ns(void)
{
   return (int64_t)time(NULL) * 1000000000LL;
}
static uint64_t otlp_thread_id(void) { return 0; }
#endif

/* Bounded so a runaway log cannot grow memory without limit. Dropping the
 * oldest is the right trade: the newest records describe what is happening
 * now, which is what anyone reading them wants. */
#define OTLP_MAX_RECORDS     2048
#define OTLP_MAX_BATCH       128
#define OTLP_FLUSH_US        (10 * 1000 * 1000)
#define OTLP_MAX_LINE        1024

typedef struct
{
   int64_t  time_unix_nano;
   int      severity_number;
   char     severity_text[8];
   /* Empty for a line from RARCH_LOG and friends, which is the common case.
    * "stdout" or "stderr" for a raw write picked up by the capture in
    * platform_switch.c, emitted as a log.source attribute. Worth telling
    * apart: a RetroArch log line carries a real level, a raw write does
    * not, and a core that printf()s is a different thing from one using
    * the libretro log callback. */
   char     source[8];
   /* 0 when unavailable. Emitted as thread.id so a query can separate one
    * thread doing two things from two threads racing. */
   uint64_t thread_id;
   char     body[OTLP_MAX_LINE];
} otlp_record_t;

typedef struct
{
   bool            running;
   bool            stopping;     /* guarded by lock */
   bool            suspended;    /* guarded by lock */

   char            url[512];
   /* May carry a credential. Never logged, never returned. */
   char            headers[1024];
   char            resource_attrs[512];

   otlp_record_t  *records;
   unsigned        head;
   unsigned        count;

   sthread_t      *thread;
   slock_t        *lock;
   scond_t        *cond;

   unsigned        stat_accepted;
   unsigned        stat_sent;
   unsigned        stat_dropped;
   unsigned        stat_failures;
   char            last_error[160];
   char            config_dir[256];
} otlp_state_t;

static otlp_state_t otlp_st;

/* ------------------------------------------------------------------ */

static void otlp_read_first_line(const char *dir, const char *name,
      char *out, size_t out_len)
{
   char path[PATH_MAX_LENGTH];
   FILE *fp;
   size_t len;

   out[0] = '\0';
   snprintf(path, sizeof(path), "%s/%s", dir, name);

   if (!(fp = fopen(path, "r")))
      return;

   if (fgets(out, (int)out_len, fp))
   {
      len = strlen(out);
      while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'
               || out[len - 1] == ' ' || out[len - 1] == '\t'))
         out[--len] = '\0';
   }

   fclose(fp);
}

/**
 * Appends value to out as the contents of a JSON string.
 *
 * Hand rolled because RetroArch has no JSON writer that is guaranteed
 * present on every port this builds for. The escaping is the only part that
 * genuinely has to be right, and it is unit tested on a host over quotes,
 * backslashes, newlines, tabs, control characters and UTF-8.
 */
static void otlp_append_escaped(char *out, size_t out_len, const char *value)
{
   size_t o = strlen(out);
   const unsigned char *p;

   for (p = (const unsigned char*)value; *p; p++)
   {
      /* Worst case one input byte becomes six output bytes (\u00xx), plus
       * the terminator. Stop rather than truncate mid escape. */
      if (o + 7 >= out_len)
         break;

      switch (*p)
      {
         case '"':  out[o++] = '\\'; out[o++] = '"';  break;
         case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
         case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
         case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
         case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
         case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
         case '\t': out[o++] = '\\'; out[o++] = 't';  break;
         default:
            if (*p < 0x20)
            {
               /* Control characters must be escaped. Log lines can carry
                * ANSI colour sequences, which start with 0x1b. */
               snprintf(out + o, out_len - o, "\\u%04x", *p);
               o += 6;
            }
            else
               out[o++] = (char)*p;
            break;
      }
   }

   out[o] = '\0';
}

static void otlp_append(char *out, size_t out_len, const char *text)
{
   size_t o = strlen(out);
   size_t n = strlen(text);
   if (o + n + 1 > out_len)
      n = out_len - o - 1;
   if ((int)n <= 0)
      return;
   memcpy(out + o, text, n);
   out[o + n] = '\0';
}

/* RetroArch tags are "[INFO] ", "[WARN] ", "[ERROR] ". Map onto the
 * OpenTelemetry severity number scale. */
static int otlp_severity_number(const char *tag)
{
   if (tag)
   {
      if (strstr(tag, "ERROR"))
         return 17;
      if (strstr(tag, "WARN"))
         return 13;
      if (strstr(tag, "DEBUG"))
         return 5;
   }
   return 9;
}

static const char *otlp_severity_text(int number)
{
   switch (number)
   {
      case 17: return "ERROR";
      case 13: return "WARN";
      case 5:  return "DEBUG";
      default: break;
   }
   return "INFO";
}

/* ------------------------------------------------------------------ */

static char *otlp_build_payload(const otlp_record_t *batch, unsigned n)
{
   /* Generous: each record is bounded by OTLP_MAX_LINE, and escaping can at
    * worst multiply a line by six. */
   size_t cap = 1024 + (size_t)n * (OTLP_MAX_LINE * 6 + 256);
   char *out  = (char*)malloc(cap);
   unsigned i;
   char num[32];

   if (!out)
      return NULL;
   out[0] = '\0';

   otlp_append(out, cap, "{\"resourceLogs\":[{\"resource\":{\"attributes\":[");

   /* resource_attrs is the OTEL_RESOURCE_ATTRIBUTES form, key=value pairs
    * separated by commas. */
   {
      const char *p = otlp_st.resource_attrs;
      bool first    = true;

      while (p && *p)
      {
         const char *comma = strchr(p, ',');
         const char *eq;
         size_t item_len   = comma ? (size_t)(comma - p) : strlen(p);
         char item[256];

         if (item_len >= sizeof(item))
            item_len = sizeof(item) - 1;
         memcpy(item, p, item_len);
         item[item_len] = '\0';

         if ((eq = strchr(item, '=')))
         {
            char key[128];
            size_t key_len = (size_t)(eq - item);
            if (key_len >= sizeof(key))
               key_len = sizeof(key) - 1;
            memcpy(key, item, key_len);
            key[key_len] = '\0';

            if (!first)
               otlp_append(out, cap, ",");
            first = false;

            otlp_append(out, cap, "{\"key\":\"");
            otlp_append_escaped(out, cap, key);
            otlp_append(out, cap, "\",\"value\":{\"stringValue\":\"");
            otlp_append_escaped(out, cap, eq + 1);
            otlp_append(out, cap, "\"}}");
         }

         if (!comma)
            break;
         p = comma + 1;
      }
   }

   otlp_append(out, cap, "]},\"scopeLogs\":[{\"logRecords\":[");

   for (i = 0; i < n; i++)
   {
      if (i)
         otlp_append(out, cap, ",");

      /* timeUnixNano is a string in the OTLP JSON mapping: it is a uint64
       * and JSON numbers cannot carry that range safely. */
      otlp_append(out, cap, "{\"timeUnixNano\":\"");
      snprintf(num, sizeof(num), "%lld", (long long)batch[i].time_unix_nano);
      otlp_append(out, cap, num);
      otlp_append(out, cap, "\",\"severityNumber\":");
      snprintf(num, sizeof(num), "%d", batch[i].severity_number);
      otlp_append(out, cap, num);
      otlp_append(out, cap, ",\"severityText\":\"");
      otlp_append(out, cap, batch[i].severity_text);
      otlp_append(out, cap, "\",\"body\":{\"stringValue\":\"");
      otlp_append_escaped(out, cap, batch[i].body);
      otlp_append(out, cap, "\"}");
      /* Attributes, when there is anything worth saying. log.source only
       * appears on raw writes; thread.id whenever the kernel gave us one.
       * Emitting empties on every line would cost payload size on the
       * hottest path for nothing. */
      if (batch[i].source[0] || batch[i].thread_id)
      {
         int wrote = 0;
         otlp_append(out, cap, ",\"attributes\":[");
         if (batch[i].source[0])
         {
            otlp_append(out, cap,
                  "{\"key\":\"log.source\",\"value\":{\"stringValue\":\"");
            otlp_append_escaped(out, cap, batch[i].source);
            otlp_append(out, cap, "\"}}");
            wrote = 1;
         }
         if (batch[i].thread_id)
         {
            if (wrote)
               otlp_append(out, cap, ",");
            /* String, not a JSON number: this is a u64 and JSON cannot
             * carry that range without losing the low bits. */
            otlp_append(out, cap,
                  "{\"key\":\"thread.id\",\"value\":{\"stringValue\":\"");
            snprintf(num, sizeof(num), "%llu",
                  (unsigned long long)batch[i].thread_id);
            otlp_append(out, cap, num);
            otlp_append(out, cap, "\"}}");
         }
         otlp_append(out, cap, "]");
      }
      otlp_append(out, cap, "}");
   }

   otlp_append(out, cap, "]}]}]}");
   return out;
}

static bool otlp_post(const char *body)
{
   struct http_connection_t *conn = NULL;
   struct http_t *http            = NULL;
   bool ok                        = false;
   int status                     = 0;

   if (!(conn = net_http_connection_new(otlp_st.url, "POST", body)))
   {
      slock_lock(otlp_st.lock);
      strlcpy(otlp_st.last_error, "could not create connection",
            sizeof(otlp_st.last_error));
      slock_unlock(otlp_st.lock);
      return false;
   }

   net_http_connection_set_headers(conn, otlp_st.headers);

   while (!net_http_connection_done(conn))
   {
      if (!net_http_connection_iterate(conn))
         break;
   }

   if (!(http = net_http_new(conn)))
   {
      slock_lock(otlp_st.lock);
      strlcpy(otlp_st.last_error, "connection failed (dns, tls or refused)",
            sizeof(otlp_st.last_error));
      slock_unlock(otlp_st.lock);
      net_http_connection_free(conn);
      return false;
   }

   while (!net_http_update(http, NULL, NULL))
   {
      bool stop;
      slock_lock(otlp_st.lock);
      stop = otlp_st.stopping;
      slock_unlock(otlp_st.lock);
      if (stop)
         break;
      retro_sleep(10);
   }

   status = net_http_status(http);
   ok     = (status >= 200 && status < 300);

   if (!ok)
   {
      slock_lock(otlp_st.lock);
      snprintf(otlp_st.last_error, sizeof(otlp_st.last_error),
            "endpoint returned HTTP %d", status);
      slock_unlock(otlp_st.lock);
   }

   net_http_delete(http);
   net_http_connection_free(conn);
   return ok;
}

/* ------------------------------------------------------------------ */

static void otlp_worker(void *unused)
{
   /* Heap, not stack. OTLP_MAX_BATCH records is around 134KB, which is far
    * more than a thread stack on this platform: an earlier version declared
    * this as a local array and faulted in the function prologue the instant
    * the thread was created. */
   otlp_record_t *batch = (otlp_record_t*)malloc(
         sizeof(otlp_record_t) * OTLP_MAX_BATCH);

   (void)unused;

   if (!batch)
   {
      /* Without this the exporter stays "running" with no consumer: producers
       * keep accepting records into a queue nobody drains, and the exit
       * report shows sent=0 with zero failures, which reads as a transport
       * mystery instead of an allocation failure. Stop loudly. */
      slock_lock(otlp_st.lock);
      otlp_st.running = false;
      slock_unlock(otlp_st.lock);
      otlp_write_status("stopped: worker could not allocate the batch buffer");
      return;
   }

   for (;;)
   {
      unsigned n = 0;

      slock_lock(otlp_st.lock);

      if ((!otlp_st.stopping && otlp_st.count == 0) || otlp_st.suspended)
         scond_wait_timeout(otlp_st.cond, otlp_st.lock, OTLP_FLUSH_US);

      /* Park while the console is suspended rather than taking a batch we
       * would then try to post over a network the OS is dismantling. */
      if (otlp_st.suspended && !otlp_st.stopping)
      {
         slock_unlock(otlp_st.lock);
         continue;
      }

      while (n < OTLP_MAX_BATCH && otlp_st.count > 0)
      {
         unsigned tail = (otlp_st.head + OTLP_MAX_RECORDS - otlp_st.count)
            % OTLP_MAX_RECORDS;
         batch[n++]    = otlp_st.records[tail];
         otlp_st.count--;
      }

      if (n == 0 && otlp_st.stopping)
      {
         slock_unlock(otlp_st.lock);
         free(batch);
         return;
      }

      slock_unlock(otlp_st.lock);

      if (n > 0)
      {
         char *payload;
         bool  parked;

         slock_lock(otlp_st.lock);
         parked = otlp_st.suspended && !otlp_st.stopping;
         if (parked)
         {
            /* Focus was lost after the batch was taken. Put it back rather
             * than posting into a network that is going away; the records are
             * still wanted, just not now. */
            unsigned i;
            for (i = 0; i < n && otlp_st.count < OTLP_MAX_RECORDS; i++)
            {
               unsigned tail = (otlp_st.head + OTLP_MAX_RECORDS
                     - otlp_st.count - 1) % OTLP_MAX_RECORDS;
               otlp_st.records[tail] = batch[n - 1 - i];
               otlp_st.count++;
            }
         }
         slock_unlock(otlp_st.lock);

         if (parked)
            continue;

         payload = otlp_build_payload(batch, n);
         bool ok       = payload && otlp_post(payload);

         free(payload);

         slock_lock(otlp_st.lock);
         if (ok)
            otlp_st.stat_sent += n;
         else
         {
            /* Dropped rather than requeued. Requeuing a batch that failed
             * because the network is down grows the buffer without bound and
             * starves the newer records describing what happened. */
            otlp_st.stat_dropped += n;
            otlp_st.stat_failures++;
         }
         slock_unlock(otlp_st.lock);
      }
   }
}

/* ------------------------------------------------------------------ */

bool otlp_log_exporter_init(const char *config_dir)
{
   /* Before anything can be timestamped. */
   otlp_clock_anchor();

   char endpoint[512];
   size_t len;

   if (otlp_st.running || !config_dir)
      return otlp_st.running;

   memset(&otlp_st, 0, sizeof(otlp_st));

   /* Recorded before anything can fail, because otlp_write_status builds its
    * path from it. Set later and every early return below is silent, which
    * makes the most likely failure the one that reports nothing at all. */
   strlcpy(otlp_st.config_dir, config_dir, sizeof(otlp_st.config_dir));

   otlp_read_first_line(config_dir, "otel-endpoint", endpoint, sizeof(endpoint));
   if (string_is_empty(endpoint))
   {
      otlp_write_status("stopped: no endpoint. Expected a url on the first"
            " line of %s/otel-endpoint", config_dir);
      return false;
   }

   /* OTEL_EXPORTER_OTLP_ENDPOINT is a base that the signal path is appended
    * to. Accept a full logs url too, so either form works. */
   len = strlen(endpoint);
   while (len > 0 && endpoint[len - 1] == '/')
      endpoint[--len] = '\0';

   if (len >= 8 && !strcmp(endpoint + len - 8, "/v1/logs"))
      strlcpy(otlp_st.url, endpoint, sizeof(otlp_st.url));
   else
      snprintf(otlp_st.url, sizeof(otlp_st.url), "%s/v1/logs", endpoint);

   otlp_read_first_line(config_dir, "otel-resource-attributes",
         otlp_st.resource_attrs, sizeof(otlp_st.resource_attrs));
   if (string_is_empty(otlp_st.resource_attrs))
      strlcpy(otlp_st.resource_attrs, "service.name=retroarch",
            sizeof(otlp_st.resource_attrs));

   /* otel-headers is the OTEL_EXPORTER_OTLP_HEADERS form, key=value pairs
    * separated by commas. net_http wants them CRLF separated. */
   {
      char raw[1024];
      char *p;

      otlp_read_first_line(config_dir, "otel-headers", raw, sizeof(raw));

      /* Every line here, including the last, has to end in CRLF. net_http
       * sends this block verbatim and then appends Content-Length itself, so
       * a missing terminator does not merely drop a header: the last one runs
       * into "Content-Length: N" and eats it. The server then has a bad
       * credential and no way to size the body, waits for it, and closes.
       * That reads back as a failed receive rather than an HTTP error, which
       * is why it looked like a transport fault. task_http.c sets the same
       * precedent with "Expect: 100-continue\r\n". */
      strlcpy(otlp_st.headers, "Content-Type: application/json\r\n",
            sizeof(otlp_st.headers));

      p = raw;
      while (p && *p)
      {
         char *comma = strchr(p, ',');
         char *eq;
         if (comma)
            *comma = '\0';
         if ((eq = strchr(p, '=')))
         {
            *eq = '\0';
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), p);
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), ": ");
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), eq + 1);
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), "\r\n");
         }
         if (!comma)
            break;
         p = comma + 1;
      }
   }

   if (!(otlp_st.records = (otlp_record_t*)calloc(OTLP_MAX_RECORDS,
               sizeof(otlp_record_t))))
   {
      otlp_write_status("stopped: out of memory for %u records",
            (unsigned)OTLP_MAX_RECORDS);
      return false;
   }

   if (!(otlp_st.lock = slock_new()))
   {
      otlp_write_status("stopped: slock_new failed");
      goto error;
   }
   if (!(otlp_st.cond = scond_new()))
   {
      otlp_write_status("stopped: scond_new failed");
      goto error;
   }

   otlp_st.running = true;
   otlp_write_status("started, endpoint=%s", otlp_st.url);

   if (!(otlp_st.thread = sthread_create(otlp_worker, NULL)))
   {
      otlp_st.running = false;
      otlp_write_status("stopped: could not start the sender thread");
      goto error;
   }

   return true;

error:
   if (otlp_st.cond)
      scond_free(otlp_st.cond);
   if (otlp_st.lock)
      slock_free(otlp_st.lock);
   free(otlp_st.records);
   memset(&otlp_st, 0, sizeof(otlp_st));
   return false;
}


/**
 * Writes a one line status file next to the config.
 *
 * Deliberately plain fopen rather than RARCH_LOG. On some ports the log file
 * is never opened, so anything reported through the logger is invisible, and
 * a diagnostic that depends on the subsystem it is diagnosing is worth
 * nothing. This is the only place the exporter reports on itself.
 */
static void otlp_write_status(const char *fmt, ...)
{
   char path[512];
   FILE *fp;
   va_list ap;

   snprintf(path, sizeof(path), "%s/otel-status.txt", otlp_st.config_dir);
   if (!(fp = fopen(path, "w")))
      return;

   va_start(ap, fmt);
   vfprintf(fp, fmt, ap);
   va_end(ap);
   fputc('\n', fp);
   fclose(fp);
}

bool otlp_log_exporter_enabled(void)
{
   return otlp_st.running;
}

static void otlp_enqueue(int severity, const char *source, const char *line)
{
   otlp_record_t *rec;

   slock_lock(otlp_st.lock);

   /* Checked under the lock: the worker clears running from its own thread
    * when it stops itself, and enqueueing past that point writes into a
    * queue nobody will ever drain. */
   if (!otlp_st.running)
   {
      slock_unlock(otlp_st.lock);
      return;
   }

   rec = &otlp_st.records[otlp_st.head];
   otlp_st.head = (otlp_st.head + 1) % OTLP_MAX_RECORDS;

   if (otlp_st.count == OTLP_MAX_RECORDS)
      otlp_st.stat_dropped++;   /* overwrote the oldest */
   else
      otlp_st.count++;

   {
      rec->time_unix_nano  = otlp_now_ns();
      rec->thread_id       = otlp_thread_id();
      rec->severity_number = severity;
      strlcpy(rec->severity_text, otlp_severity_text(severity),
            sizeof(rec->severity_text));
      strlcpy(rec->source, source ? source : "", sizeof(rec->source));
      strlcpy(rec->body, line, sizeof(rec->body));
   }

   otlp_st.stat_accepted++;

   slock_unlock(otlp_st.lock);
   scond_signal(otlp_st.cond);
}

void otlp_log_exporter_log(const char *tag, const char *line)
{
   if (!line || !*line)
      return;
   otlp_enqueue(otlp_severity_number(tag), NULL, line);
}

void otlp_log_exporter_log_raw(const char *line, bool is_stderr)
{
   if (!line || !*line)
      return;
   /* A raw write carries no level. stderr is reported one step above stdout
    * because callers overwhelmingly use it for failures, but neither is a
    * real severity and log.source is what actually says where it came from.
    * 9 is INFO and 13 is WARN in the OTLP severity numbering. */
   otlp_enqueue(is_stderr ? 13 : 9, is_stderr ? "stderr" : "stdout", line);
}

void otlp_log_exporter_set_suspended(bool suspended)
{
   bool changed;

   if (!otlp_st.running)
      return;

   slock_lock(otlp_st.lock);
   changed = (otlp_st.suspended != suspended);
   otlp_st.suspended = suspended;
   slock_unlock(otlp_st.lock);

   /* Waking on resume matters: the worker may be part way through its flush
    * wait, and records buffered during the sleep should not sit there for the
    * remainder of it. */
   if (changed && !suspended)
      scond_signal(otlp_st.cond);
}

void otlp_log_exporter_deinit(void)
{
   /* Keyed on the thread, not on running: the worker sets running to false
    * itself if its batch allocation fails, and bailing out on that basis
    * would skip the join and leak the lock, the cond and the queue. */
   if (!otlp_st.thread)
      return;

   slock_lock(otlp_st.lock);
   otlp_st.stopping = true;
   slock_unlock(otlp_st.lock);
   scond_signal(otlp_st.cond);

   if (otlp_st.thread)
      sthread_join(otlp_st.thread);

   otlp_write_status("accepted=%u sent=%u dropped=%u failures=%u last_error=%s",
         otlp_st.stat_accepted, otlp_st.stat_sent, otlp_st.stat_dropped,
         otlp_st.stat_failures,
         otlp_st.last_error[0] ? otlp_st.last_error : "none");

   scond_free(otlp_st.cond);
   slock_free(otlp_st.lock);
   free(otlp_st.records);

   /* Clear the headers, which may hold a credential. */
   memset(&otlp_st, 0, sizeof(otlp_st));
}

void otlp_log_exporter_stats(unsigned *accepted, unsigned *sent,
      unsigned *dropped, unsigned *failures, const char **last_error)
{
   if (!otlp_st.running)
      return;

   slock_lock(otlp_st.lock);
   if (accepted)   *accepted   = otlp_st.stat_accepted;
   if (sent)       *sent       = otlp_st.stat_sent;
   if (dropped)    *dropped    = otlp_st.stat_dropped;
   if (failures)   *failures   = otlp_st.stat_failures;
   if (last_error) *last_error = otlp_st.last_error;
   slock_unlock(otlp_st.lock);
}
