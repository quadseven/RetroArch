/*  RetroArch - OTLP log exporter
 *  See otlp_log_exporter.h for what this is and how it is configured.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <net/net_http.h>
#include <string/stdstring.h>

#include "otlp_log_exporter.h"
#include "file_path_special.h"

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
   char     body[OTLP_MAX_LINE];
} otlp_record_t;

typedef struct
{
   bool            running;
   volatile bool   stopping;

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
      otlp_append(out, cap, "\"}}");
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
      strlcpy(otlp_st.last_error, "could not create connection",
            sizeof(otlp_st.last_error));
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
      strlcpy(otlp_st.last_error, "connection failed (dns, tls or refused)",
            sizeof(otlp_st.last_error));
      net_http_connection_free(conn);
      return false;
   }

   while (!net_http_update(http, NULL, NULL))
   {
      if (otlp_st.stopping)
         break;
      retro_sleep(10);
   }

   status = net_http_status(http);
   ok     = (status >= 200 && status < 300);

   if (!ok)
      snprintf(otlp_st.last_error, sizeof(otlp_st.last_error),
            "endpoint returned HTTP %d", status);

   net_http_delete(http);
   net_http_connection_free(conn);
   return ok;
}

/* ------------------------------------------------------------------ */

static void otlp_worker(void *unused)
{
   otlp_record_t batch[OTLP_MAX_BATCH];

   (void)unused;

   for (;;)
   {
      unsigned n = 0;

      slock_lock(otlp_st.lock);

      if (!otlp_st.stopping && otlp_st.count == 0)
         scond_wait_timeout(otlp_st.cond, otlp_st.lock, OTLP_FLUSH_US);

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
         return;
      }

      slock_unlock(otlp_st.lock);

      if (n > 0)
      {
         char *payload = otlp_build_payload(batch, n);
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
   char endpoint[512];
   size_t len;

   if (otlp_st.running || !config_dir)
      return otlp_st.running;

   memset(&otlp_st, 0, sizeof(otlp_st));

   otlp_read_first_line(config_dir, "otel-endpoint", endpoint, sizeof(endpoint));
   if (string_is_empty(endpoint))
      return false;

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
      strlcpy(otlp_st.headers, "Content-Type: application/json",
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
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), "\r\n");
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), p);
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), ": ");
            otlp_append(otlp_st.headers, sizeof(otlp_st.headers), eq + 1);
         }
         if (!comma)
            break;
         p = comma + 1;
      }
   }

   if (!(otlp_st.records = (otlp_record_t*)calloc(OTLP_MAX_RECORDS,
               sizeof(otlp_record_t))))
      return false;

   if (!(otlp_st.lock = slock_new()))
      goto error;
   if (!(otlp_st.cond = scond_new()))
      goto error;

   otlp_st.running = true;

   if (!(otlp_st.thread = sthread_create(otlp_worker, NULL)))
   {
      otlp_st.running = false;
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

bool otlp_log_exporter_enabled(void)
{
   return otlp_st.running;
}

void otlp_log_exporter_log(const char *tag, const char *line)
{
   otlp_record_t *rec;
   int severity;

   if (!otlp_st.running || !line || !*line)
      return;

   severity = otlp_severity_number(tag);

   slock_lock(otlp_st.lock);

   rec = &otlp_st.records[otlp_st.head];
   otlp_st.head = (otlp_st.head + 1) % OTLP_MAX_RECORDS;

   if (otlp_st.count == OTLP_MAX_RECORDS)
      otlp_st.stat_dropped++;   /* overwrote the oldest */
   else
      otlp_st.count++;

   {
      /* time() is second resolution, which is all that is portable here.
       * OTLP wants nanoseconds. */
      rec->time_unix_nano  = (int64_t)time(NULL) * 1000000000LL;
      rec->severity_number = severity;
      strlcpy(rec->severity_text, otlp_severity_text(severity),
            sizeof(rec->severity_text));
      strlcpy(rec->body, line, sizeof(rec->body));
   }

   otlp_st.stat_accepted++;

   slock_unlock(otlp_st.lock);
   scond_signal(otlp_st.cond);
}

void otlp_log_exporter_deinit(void)
{
   if (!otlp_st.running)
      return;

   slock_lock(otlp_st.lock);
   otlp_st.stopping = true;
   slock_unlock(otlp_st.lock);
   scond_signal(otlp_st.cond);

   if (otlp_st.thread)
      sthread_join(otlp_st.thread);

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
