/*
 * Copyright (c) 2011 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef NXWEB_INTERNAL_H_INCLUDED
#define NXWEB_INTERNAL_H_INCLUDED

#include <netinet/in.h>

#include "config.h"
#include "nxweb.h"
#include "nx_queue.h"

enum {
  NXE_CLASS_LISTEN=0,
  NXE_CLASS_SOCKET,
  NXE_CLASS_WORKER_JOB_DONE,
  NXE_CLASS_NET_THREAD_ACCEPT,
  NXE_CLASS_NET_THREAD_SHUTDOWN,
  NXE_CLASS_USER1,
  NXE_CLASS_USER2,
  NXE_CLASS_USER3,
  NXE_CLASS_USER4
};

enum nxweb_timers {
  NXE_TIMER_KEEP_ALIVE,
  NXE_TIMER_READ,
  NXE_TIMER_WRITE
};

typedef struct nxweb_job {
  nxweb_connection* conn;
} nxweb_job;

typedef struct nxweb_job_queue {
  nx_queue q;
  nxweb_job jobs[NXWEB_JOBS_QUEUE_SIZE];
} nxweb_job_queue;

typedef struct nxweb_accept {
  int client_fd;
  struct in_addr sin_addr;
} nxweb_accept;

typedef struct nx_queue nxweb_accept_queue;

typedef struct nxweb_net_thread {
  pthread_t thread_id;
  nxe_loop* loop;
  nxe_event_async shutdown_evt;
  nxe_event_async accept_evt;
  nxweb_accept_queue* accept_queue;
  nxweb_job_queue job_queue;
  pthread_mutex_t job_queue_mux;
  pthread_cond_t job_queue_cond;
  pthread_t worker_threads[N_WORKER_THREADS];
} nxweb_net_thread;


static inline void nxweb_job_queue_init(nxweb_job_queue* jq) {
  nx_queue_init(&jq->q, sizeof(nxweb_job), NXWEB_JOBS_QUEUE_SIZE);
}

static inline int nxweb_job_queue_push(nxweb_job_queue* jq, const nxweb_job* job) {
  return nx_queue_push(&jq->q, job);
}

static inline int nxweb_job_queue_pop(nxweb_job_queue* jq, nxweb_job* job) {
  return nx_queue_pop(&jq->q, job);
}

static inline int nxweb_job_queue_is_empty(nxweb_job_queue* jq) {
  return nx_queue_is_empty(&jq->q);
}

static inline int nxweb_job_queue_is_full(nxweb_job_queue* jq) {
  return nx_queue_is_full(&jq->q);
}


static inline nxweb_accept_queue* nxweb_accept_queue_new(int size) {
  return (nxweb_accept_queue*)nx_queue_new(sizeof(nxweb_accept), size);
}

static inline int nxweb_accept_queue_push(nxweb_accept_queue* jq, const nxweb_accept* accpt) {
  return nx_queue_push(jq, accpt);
}

static inline int nxweb_accept_queue_pop(nxweb_accept_queue* jq, nxweb_accept* accpt) {
  return nx_queue_pop(jq, accpt);
}

static inline int nxweb_accept_queue_is_empty(nxweb_accept_queue* jq) {
  return nx_queue_is_empty(jq);
}

static inline int nxweb_accept_queue_is_full(nxweb_accept_queue* jq) {
  return nx_queue_is_full(jq);
}

// Internal use
void _nxweb_main();
nxweb_net_thread* _nxweb_get_net_thread_data();
char* _nxweb_find_end_of_http_headers(char* buf, int len);
int _nxweb_parse_http_request(nxweb_request* req, char* headers, char* end_of_headers, int bytes_received);
void _nxweb_write_response_headers_raw(nxweb_request* req, const char* fmt, ...) __attribute__((format (printf, 2, 3)));
void _nxweb_prepare_response_headers(nxweb_request* req);
void _nxweb_finalize_response(nxweb_request *req);
void _nxweb_register_printf_extensions();
void _nxweb_decode_chunked_request(nxweb_request* req);
int _nxweb_decode_chunked(char* buf, int buf_len);
int _nxweb_verify_chunked(const char* buf, int buf_len);
int _nxweb_decode_chunked_stream(nxweb_chunked_decoder_state* decoder_state, char* buf, long* buf_len);
void nxweb_parse_request_parameters(nxweb_request *req, int preserve_uri); // Modifies conn->uri and conn->request_body content (does url_decode inplace)
void nxweb_parse_request_cookies(nxweb_request *req); // Modifies conn->cookie content (does url_decode inplace)

const char* nx_simple_map_get(nx_simple_map_entry map[], const char* name);
const char* nx_simple_map_get_nocase(nx_simple_map_entry map[], const char* name);
void nx_simple_map_add(nx_simple_map_entry map[], const char* name, const char* value, int max_entries);

#define max(a,b) \
({ __typeof__ (a) _a = (a); \
__typeof__ (b) _b = (b); \
_a > _b ? _a : _b; })

#define min(a,b) \
({ __typeof__ (a) _a = (a); \
__typeof__ (b) _b = (b); \
_a < _b ? _a : _b; })

#endif // NXWEB_INTERNAL_H_INCLUDED

