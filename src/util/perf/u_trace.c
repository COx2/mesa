/*
 * Copyright © 2020 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <inttypes.h>

#include "util/list.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_fifo.h"
#include "util/u_vector.h"

#include "u_trace.h"

#define __NEEDS_TRACE_PRIV
#include "u_trace_priv.h"

#define PAYLOAD_BUFFER_SIZE 0x100
#define TIMESTAMP_BUF_SIZE 0x1000
#define TRACES_PER_CHUNK   (TIMESTAMP_BUF_SIZE / sizeof(uint64_t))

int _u_trace_instrument;

#ifdef HAVE_PERFETTO
int ut_perfetto_enabled;

/**
 * Global list of contexts, so we can defer starting the queue until
 * perfetto tracing is started.
 */
static struct list_head ctx_list = { &ctx_list, &ctx_list };

static simple_mtx_t ctx_list_mutex = _SIMPLE_MTX_INITIALIZER_NP;
#endif

struct u_trace_payload_buf {
   uint32_t refcount;

   uint8_t *buf;
   uint8_t *next;
   uint8_t *end;
};

struct u_trace_event {
   const struct u_tracepoint *tp;
   const void *payload;
};

/**
 * A "chunk" of trace-events and corresponding timestamp buffer.  As
 * trace events are emitted, additional trace chucks will be allocated
 * as needed.  When u_trace_flush() is called, they are transferred
 * from the u_trace to the u_trace_context queue.
 */
struct u_trace_chunk {
   struct list_head node;

   struct u_trace_context *utctx;

   /* The number of traces this chunk contains so far: */
   unsigned num_traces;

   /* table of trace events: */
   struct u_trace_event traces[TRACES_PER_CHUNK];

   /* table of driver recorded 64b timestamps, index matches index
    * into traces table
    */
   void *timestamps;

   /* Array of u_trace_payload_buf referenced by traces[] elements.
    */
   struct u_vector payloads;

   /* Current payload buffer being written. */
   struct u_trace_payload_buf *payload;

   struct util_queue_fence fence;

   bool last;          /* this chunk is last in batch */
   bool eof;           /* this chunk is last in frame */

   void *flush_data; /* assigned by u_trace_flush */

   /**
    * Several chunks reference a single flush_data instance thus only
    * one chunk should be designated to free the data.
    */
   bool free_flush_data;
};

struct u_trace_printer {
   void (*start)(struct u_trace_context *utctx);
   void (*end)(struct u_trace_context *utctx);
   void (*start_of_frame)(struct u_trace_context *utctx);
   void (*end_of_frame)(struct u_trace_context *utctx);
   void (*start_of_batch)(struct u_trace_context *utctx);
   void (*end_of_batch)(struct u_trace_context *utctx);
   void (*event)(struct u_trace_context *utctx,
                 struct u_trace_chunk *chunk,
                 const struct u_trace_event *evt,
                 uint64_t ns, int32_t delta);
};

static void
print_txt_start(struct u_trace_context *utctx)
{

}

static void
print_txt_end_of_frame(struct u_trace_context *utctx)
{
   fprintf(utctx->out, "END OF FRAME %u\n", utctx->frame_nr);
}

static void
print_txt_start_of_batch(struct u_trace_context *utctx)
{
   fprintf(utctx->out, "+----- NS -----+ +-- Δ --+  +----- MSG -----\n");
}

static void
print_txt_end_of_batch(struct u_trace_context *utctx)
{
   uint64_t elapsed = utctx->last_time_ns - utctx->first_time_ns;
   fprintf(utctx->out, "ELAPSED: %"PRIu64" ns\n", elapsed);
}

static void
print_txt_event(struct u_trace_context *utctx,
                struct u_trace_chunk *chunk,
                const struct u_trace_event *evt,
                uint64_t ns, int32_t delta)
{
   if (evt->tp->print) {
      fprintf(utctx->out, "%016"PRIu64" %+9d: %s: ", ns, delta, evt->tp->name);
      evt->tp->print(utctx->out, evt->payload);
   } else {
      fprintf(utctx->out, "%016"PRIu64" %+9d: %s\n", ns, delta, evt->tp->name);
   }
}

static struct u_trace_printer txt_printer = {
   .start = &print_txt_start,
   .end = &print_txt_start,
   .start_of_frame = &print_txt_start,
   .end_of_frame = &print_txt_end_of_frame,
   .start_of_batch = &print_txt_start_of_batch,
   .end_of_batch = &print_txt_end_of_batch,
   .event = &print_txt_event,
};

static void
print_json_start(struct u_trace_context *utctx)
{
   fprintf(utctx->out, "[\n");
}

static void
print_json_end(struct u_trace_context *utctx)
{
   fprintf(utctx->out, "\n]");
}

static void
print_json_start_of_frame(struct u_trace_context *utctx)
{
   if (utctx->frame_nr != 0)
      fprintf(utctx->out, ",\n");
   fprintf(utctx->out, "{\n\"frame\": %u,\n", utctx->frame_nr);
   fprintf(utctx->out, "\"batches\": [\n");
}

static void
print_json_end_of_frame(struct u_trace_context *utctx)
{
   fprintf(utctx->out, "]\n}\n");
   fflush(utctx->out);
}

static void
print_json_start_of_batch(struct u_trace_context *utctx)
{
   if (utctx->batch_nr != 0)
      fprintf(utctx->out, ",\n");
   fprintf(utctx->out, "{\n\"events\": [\n");
}

static void
print_json_end_of_batch(struct u_trace_context *utctx)
{
   uint64_t elapsed = utctx->last_time_ns - utctx->first_time_ns;
   fprintf(utctx->out, "],\n");
   fprintf(utctx->out, "\"duration_ns\": %"PRIu64"\n", elapsed);
   fprintf(utctx->out, "}\n");
}

static void
print_json_event(struct u_trace_context *utctx,
                 struct u_trace_chunk *chunk,
                 const struct u_trace_event *evt,
                 uint64_t ns, int32_t delta)
{
   if (utctx->event_nr != 0)
      fprintf(utctx->out, ",\n");
   fprintf(utctx->out, "{\n\"event\": \"%s\",\n", evt->tp->name);
   fprintf(utctx->out, "\"time_ns\": \"%016"PRIu64"\",\n", ns);
   fprintf(utctx->out, "\"params\": {");
   if (evt->tp->print)
      evt->tp->print_json(utctx->out, evt->payload);
   fprintf(utctx->out, "}\n}\n");
}

static struct u_trace_printer json_printer = {
   .start = print_json_start,
   .end = print_json_end,
   .start_of_frame = &print_json_start_of_frame,
   .end_of_frame = &print_json_end_of_frame,
   .start_of_batch = &print_json_start_of_batch,
   .end_of_batch = &print_json_end_of_batch,
   .event = &print_json_event,
};

static struct u_trace_payload_buf *
u_trace_payload_buf_create(void)
{
   struct u_trace_payload_buf *payload =
      malloc(sizeof(*payload) + PAYLOAD_BUFFER_SIZE);

   p_atomic_set(&payload->refcount, 1);

   payload->buf = (uint8_t *) (payload + 1);
   payload->end = payload->buf + PAYLOAD_BUFFER_SIZE;
   payload->next = payload->buf;

   return payload;
}

static struct u_trace_payload_buf *
u_trace_payload_buf_ref(struct u_trace_payload_buf *payload)
{
   p_atomic_inc(&payload->refcount);
   return payload;
}

static void
u_trace_payload_buf_unref(struct u_trace_payload_buf *payload)
{
   if (p_atomic_dec_zero(&payload->refcount))
      free(payload);
}

static void
free_chunk(void *ptr)
{
   struct u_trace_chunk *chunk = ptr;

   chunk->utctx->delete_timestamp_buffer(chunk->utctx, chunk->timestamps);

   /* Unref payloads attached to this chunk. */
   struct u_trace_payload_buf **payload;
   u_vector_foreach(payload, &chunk->payloads)
      u_trace_payload_buf_unref(*payload);
   u_vector_finish(&chunk->payloads);

   list_del(&chunk->node);
   free(chunk);
}

static void
free_chunks(struct list_head *chunks)
{
   while (!list_is_empty(chunks)) {
      struct u_trace_chunk *chunk = list_first_entry(chunks,
            struct u_trace_chunk, node);
      free_chunk(chunk);
   }
}

static struct u_trace_chunk *
get_chunk(struct u_trace *ut, size_t payload_size)
{
   struct u_trace_chunk *chunk;

   assert(payload_size <= PAYLOAD_BUFFER_SIZE);

   /* do we currently have a non-full chunk to append msgs to? */
   if (!list_is_empty(&ut->trace_chunks)) {
           chunk = list_last_entry(&ut->trace_chunks,
                           struct u_trace_chunk, node);
           /* Can we store a new trace in the chunk? */
           if (chunk->num_traces < TRACES_PER_CHUNK) {
              /* If no payload required, nothing else to check. */
              if (payload_size <= 0)
                 return chunk;

              /* If the payload buffer has space for the payload, we're good.
               */
              if (chunk->payload &&
                  (chunk->payload->end - chunk->payload->next) >= payload_size)
                 return chunk;

              /* If we don't have enough space in the payload buffer, can we
               * allocate a new one?
               */
              struct u_trace_payload_buf **buf = u_vector_add(&chunk->payloads);
              *buf = u_trace_payload_buf_create();
              chunk->payload = *buf;
              return chunk;
           }
           /* we need to expand to add another chunk to the batch, so
            * the current one is no longer the last one of the batch:
            */
           chunk->last = false;
   }

   /* .. if not, then create a new one: */
   chunk = calloc(1, sizeof(*chunk));

   chunk->utctx = ut->utctx;
   chunk->timestamps = ut->utctx->create_timestamp_buffer(ut->utctx, TIMESTAMP_BUF_SIZE);
   chunk->last = true;
   u_vector_init(&chunk->payloads, 4, sizeof(struct u_trace_payload_buf *));
   if (payload_size > 0) {
      struct u_trace_payload_buf **buf = u_vector_add(&chunk->payloads);
      *buf = u_trace_payload_buf_create();
      chunk->payload = *buf;
   }

   list_addtail(&chunk->node, &ut->trace_chunks);

   return chunk;
}

DEBUG_GET_ONCE_BOOL_OPTION(trace_instrument, "GPU_TRACE_INSTRUMENT", false)
DEBUG_GET_ONCE_BOOL_OPTION(trace, "GPU_TRACE", false)
DEBUG_GET_ONCE_FILE_OPTION(trace_file, "GPU_TRACEFILE", NULL, "w")
DEBUG_GET_ONCE_OPTION(trace_format, "GPU_TRACE_FORMAT", "txt")

static FILE *
get_tracefile(void)
{
   static FILE *tracefile = NULL;
   static bool firsttime = true;

   if (firsttime) {
      tracefile = debug_get_option_trace_file();
      if (!tracefile && debug_get_option_trace()) {
         tracefile = stdout;
      }

      if (tracefile || debug_get_option_trace_instrument())
         p_atomic_inc(&_u_trace_instrument);

      firsttime = false;
   }

   return tracefile;
}

static void
queue_init(struct u_trace_context *utctx)
{
   if (utctx->queue.jobs)
      return;

   bool ret = util_queue_init(&utctx->queue, "traceq", 256, 1,
                              UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY |
                              UTIL_QUEUE_INIT_RESIZE_IF_FULL, NULL);
   assert(ret);

   if (!ret)
      utctx->out = NULL;
}

void
u_trace_context_init(struct u_trace_context *utctx,
      void *pctx,
      u_trace_create_ts_buffer  create_timestamp_buffer,
      u_trace_delete_ts_buffer  delete_timestamp_buffer,
      u_trace_record_ts         record_timestamp,
      u_trace_read_ts           read_timestamp,
      u_trace_delete_flush_data delete_flush_data)
{
   utctx->pctx = pctx;
   utctx->create_timestamp_buffer = create_timestamp_buffer;
   utctx->delete_timestamp_buffer = delete_timestamp_buffer;
   utctx->record_timestamp = record_timestamp;
   utctx->read_timestamp = read_timestamp;
   utctx->delete_flush_data = delete_flush_data;

   utctx->last_time_ns = 0;
   utctx->first_time_ns = 0;
   utctx->frame_nr = 0;
   utctx->batch_nr = 0;
   utctx->event_nr = 0;
   utctx->start_of_frame = true;

   list_inithead(&utctx->flushed_trace_chunks);

   utctx->out = get_tracefile();

   const char *trace_format = debug_get_option_trace_format();
   if (strcmp(trace_format, "json") == 0) {
      utctx->out_printer = &json_printer;
   } else {
      utctx->out_printer = &txt_printer;
   }

#ifdef HAVE_PERFETTO
   simple_mtx_lock(&ctx_list_mutex);
   list_add(&utctx->node, &ctx_list);
   simple_mtx_unlock(&ctx_list_mutex);
#endif

   if (!u_trace_context_actively_tracing(utctx))
      return;

#ifdef HAVE_PERFETTO
   simple_mtx_lock(&ctx_list_mutex);
#endif
   queue_init(utctx);
#ifdef HAVE_PERFETTO
   simple_mtx_unlock(&ctx_list_mutex);
#endif

   if (utctx->out) {
      utctx->out_printer->start(utctx);
   }
}

void
u_trace_context_fini(struct u_trace_context *utctx)
{
#ifdef HAVE_PERFETTO
   simple_mtx_lock(&ctx_list_mutex);
   list_del(&utctx->node);
   simple_mtx_unlock(&ctx_list_mutex);
#endif

   if (utctx->out) {
      utctx->out_printer->end(utctx);
      fflush(utctx->out);
   }

   if (!utctx->queue.jobs)
      return;
   util_queue_finish(&utctx->queue);
   util_queue_destroy(&utctx->queue);
   free_chunks(&utctx->flushed_trace_chunks);
}

#ifdef HAVE_PERFETTO
void
u_trace_perfetto_start(void)
{
   simple_mtx_lock(&ctx_list_mutex);
   list_for_each_entry (struct u_trace_context, utctx, &ctx_list, node)
      queue_init(utctx);
   simple_mtx_unlock(&ctx_list_mutex);

   if (p_atomic_inc_return(&ut_perfetto_enabled) == 1)
      p_atomic_inc(&_u_trace_instrument);
}

void
u_trace_perfetto_stop(void)
{
   assert(ut_perfetto_enabled > 0);
   if (p_atomic_dec_return(&ut_perfetto_enabled) == 0)
      p_atomic_dec(&_u_trace_instrument);
}
#endif

static void
process_chunk(void *job, void *gdata, int thread_index)
{
   struct u_trace_chunk *chunk = job;
   struct u_trace_context *utctx = chunk->utctx;

   if (utctx->start_of_frame) {
      utctx->start_of_frame = false;
      utctx->batch_nr = 0;
      if (utctx->out) {
         utctx->out_printer->start_of_frame(utctx);
      }
   }

   /* For first chunk of batch, accumulated times will be zerod: */
   if (!utctx->last_time_ns) {
      utctx->event_nr = 0;
      if (utctx->out) {
         utctx->out_printer->start_of_batch(utctx);
      }
   }

   for (unsigned idx = 0; idx < chunk->num_traces; idx++) {
      const struct u_trace_event *evt = &chunk->traces[idx];

      if (!evt->tp)
         continue;

      uint64_t ns = utctx->read_timestamp(utctx, chunk->timestamps, idx, chunk->flush_data);
      int32_t delta;

      if (!utctx->first_time_ns)
         utctx->first_time_ns = ns;

      if (ns != U_TRACE_NO_TIMESTAMP) {
         delta = utctx->last_time_ns ? ns - utctx->last_time_ns : 0;
         utctx->last_time_ns = ns;
      } else {
         /* we skipped recording the timestamp, so it should be
          * the same as last msg:
          */
         ns = utctx->last_time_ns;
         delta = 0;
      }

      if (utctx->out) {
         utctx->out_printer->event(utctx, chunk, evt, ns, delta);
      }
#ifdef HAVE_PERFETTO
      if (evt->tp->perfetto) {
         evt->tp->perfetto(utctx->pctx, ns, chunk->flush_data, evt->payload);
      }
#endif

      utctx->event_nr++;
   }

   if (chunk->last) {
      if (utctx->out) {
         utctx->out_printer->end_of_batch(utctx);
      }

      utctx->batch_nr++;
      utctx->last_time_ns = 0;
      utctx->first_time_ns = 0;
   }

   if (chunk->eof) {
      if (utctx->out) {
         utctx->out_printer->end_of_frame(utctx);
      }
      utctx->frame_nr++;
      utctx->start_of_frame = true;
   }

   if (chunk->free_flush_data && utctx->delete_flush_data) {
      utctx->delete_flush_data(utctx, chunk->flush_data);
   }
}

static void
cleanup_chunk(void *job, void *gdata, int thread_index)
{
   free_chunk(job);
}

void
u_trace_context_process(struct u_trace_context *utctx, bool eof)
{
   struct list_head *chunks = &utctx->flushed_trace_chunks;

   if (list_is_empty(chunks))
      return;

   struct u_trace_chunk *last_chunk = list_last_entry(chunks,
            struct u_trace_chunk, node);
   last_chunk->eof = eof;

   while (!list_is_empty(chunks)) {
      struct u_trace_chunk *chunk = list_first_entry(chunks,
            struct u_trace_chunk, node);

      /* remove from list before enqueuing, because chunk is freed
       * once it is processed by the queue:
       */
      list_delinit(&chunk->node);

      util_queue_add_job(&utctx->queue, chunk, &chunk->fence,
            process_chunk, cleanup_chunk,
            TIMESTAMP_BUF_SIZE);
   }
}


void
u_trace_init(struct u_trace *ut, struct u_trace_context *utctx)
{
   ut->utctx = utctx;
   list_inithead(&ut->trace_chunks);
}

void
u_trace_fini(struct u_trace *ut)
{
   /* Normally the list of trace-chunks would be empty, if they
    * have been flushed to the trace-context.
    */
   free_chunks(&ut->trace_chunks);
}

bool
u_trace_has_points(struct u_trace *ut)
{
   return !list_is_empty(&ut->trace_chunks);
}

struct u_trace_iterator
u_trace_begin_iterator(struct u_trace *ut)
{
   if (list_is_empty(&ut->trace_chunks))
      return (struct u_trace_iterator) { ut, NULL, 0 };

   struct u_trace_chunk *first_chunk =
      list_first_entry(&ut->trace_chunks, struct u_trace_chunk, node);

   return (struct u_trace_iterator) { ut, first_chunk, 0};
}

struct u_trace_iterator
u_trace_end_iterator(struct u_trace *ut)
{
   if (list_is_empty(&ut->trace_chunks))
      return (struct u_trace_iterator) { ut, NULL, 0 };

   struct u_trace_chunk *last_chunk =
      list_last_entry(&ut->trace_chunks, struct u_trace_chunk, node);

   return (struct u_trace_iterator) { ut, last_chunk, last_chunk->num_traces};
}

/* If an iterator was created when there were no chunks and there are now
 * chunks, "sanitize" it to include the first chunk.
 */
static struct u_trace_iterator
sanitize_iterator(struct u_trace_iterator iter)
{
   if (iter.ut && !iter.chunk && !list_is_empty(&iter.ut->trace_chunks)) {
      iter.chunk = list_first_entry(&iter.ut->trace_chunks, struct
                                    u_trace_chunk, node);
   }

   return iter;
}

bool
u_trace_iterator_equal(struct u_trace_iterator a,
                       struct u_trace_iterator b)
{
   a = sanitize_iterator(a);
   b = sanitize_iterator(b);
   return a.ut == b.ut &&
          a.chunk == b.chunk &&
          a.event_idx == b.event_idx;
}

void
u_trace_clone_append(struct u_trace_iterator begin_it,
                     struct u_trace_iterator end_it,
                     struct u_trace *into,
                     void *cmdstream,
                     u_trace_copy_ts_buffer copy_ts_buffer)
{
   begin_it = sanitize_iterator(begin_it);
   end_it = sanitize_iterator(end_it);

   struct u_trace_chunk *from_chunk = begin_it.chunk;
   uint32_t from_idx = begin_it.event_idx;

   while (from_chunk != end_it.chunk || from_idx != end_it.event_idx) {
      struct u_trace_chunk *to_chunk = get_chunk(into, 0 /* payload_size */);

      unsigned to_copy = MIN2(TRACES_PER_CHUNK - to_chunk->num_traces,
                              from_chunk->num_traces - from_idx);
      if (from_chunk == end_it.chunk)
         to_copy = MIN2(to_copy, end_it.event_idx - from_idx);

      copy_ts_buffer(begin_it.ut->utctx, cmdstream,
                     from_chunk->timestamps, from_idx,
                     to_chunk->timestamps, to_chunk->num_traces,
                     to_copy);

      memcpy(&to_chunk->traces[to_chunk->num_traces],
             &from_chunk->traces[from_idx],
             to_copy * sizeof(struct u_trace_event));

      /* Take a refcount on payloads from from_chunk if needed. */
      if (begin_it.ut != into) {
         struct u_trace_payload_buf **in_payload;
         u_vector_foreach(in_payload, &from_chunk->payloads) {
            struct u_trace_payload_buf **out_payload =
               u_vector_add(&to_chunk->payloads);

            *out_payload = u_trace_payload_buf_ref(*in_payload);
         }
      }

      to_chunk->num_traces += to_copy;
      from_idx += to_copy;

      assert(from_idx <= from_chunk->num_traces);
      if (from_idx == from_chunk->num_traces) {
         if (from_chunk == end_it.chunk)
            break;

         from_idx = 0;
         from_chunk = list_entry(from_chunk->node.next, struct u_trace_chunk, node);
      }
   }
}

void
u_trace_disable_event_range(struct u_trace_iterator begin_it,
                            struct u_trace_iterator end_it)
{
   begin_it = sanitize_iterator(begin_it);
   end_it = sanitize_iterator(end_it);

   struct u_trace_chunk *current_chunk = begin_it.chunk;
   uint32_t start_idx = begin_it.event_idx;

   while(current_chunk != end_it.chunk) {
      memset(&current_chunk->traces[start_idx], 0,
             (current_chunk->num_traces - start_idx) * sizeof(struct u_trace_event));
      start_idx = 0;
      current_chunk = list_entry(current_chunk->node.next, struct u_trace_chunk, node);
   }

   memset(&current_chunk->traces[start_idx], 0,
          (end_it.event_idx - start_idx) * sizeof(struct u_trace_event));
}

/**
 * Append a trace event, returning pointer to buffer of tp->payload_sz
 * to be filled in with trace payload.  Called by generated tracepoint
 * functions.
 */
void *
u_trace_append(struct u_trace *ut, void *cs, const struct u_tracepoint *tp)
{
   struct u_trace_chunk *chunk = get_chunk(ut, tp->payload_sz);
   unsigned tp_idx = chunk->num_traces++;

   assert(tp->payload_sz == ALIGN_NPOT(tp->payload_sz, 8));

   /* sub-allocate storage for trace payload: */
   void *payload = NULL;
   if (tp->payload_sz > 0) {
      payload = chunk->payload->next;
      chunk->payload->next += tp->payload_sz;
   }

   /* record a timestamp for the trace: */
   ut->utctx->record_timestamp(ut, cs, chunk->timestamps, tp_idx, tp->end_of_pipe);

   chunk->traces[tp_idx] = (struct u_trace_event) {
         .tp = tp,
         .payload = payload,
   };

   return payload;
}

void
u_trace_flush(struct u_trace *ut, void *flush_data, bool free_data)
{
   list_for_each_entry(struct u_trace_chunk, chunk, &ut->trace_chunks, node) {
      chunk->flush_data = flush_data;
      chunk->free_flush_data = false;
   }

   if (free_data && !list_is_empty(&ut->trace_chunks)) {
      struct u_trace_chunk *last_chunk =
         list_last_entry(&ut->trace_chunks, struct u_trace_chunk, node);
      last_chunk->free_flush_data = true;
   }

   /* transfer batch's log chunks to context: */
   list_splicetail(&ut->trace_chunks, &ut->utctx->flushed_trace_chunks);
   list_inithead(&ut->trace_chunks);
}
