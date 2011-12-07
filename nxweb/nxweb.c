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

#define _FILE_OFFSET_BITS 64

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>

#include "nxweb_internal.h"

static pthread_t main_thread_id=0;
static struct ev_loop *main_loop=0;

static volatile sig_atomic_t shutdown_in_progress=0;
static volatile int num_connections=0;

static nxweb_net_thread net_threads[N_NET_THREADS];

// local prototypes:
static void socket_read_cb(struct ev_loop *loop, ev_io *w, int revents);
static void socket_write_cb(struct ev_loop *loop, ev_io *w, int revents);
static void read_request_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);
static void keep_alive_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);
static void write_response_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents);
static void data_ready_cb(struct ev_loop *loop, struct ev_async *w, int revents);
static void dispatch_request(struct ev_loop *loop, nxweb_connection *conn);


//static void* nxweb_malloc(size_t size) {
//  nxweb_log_error("nxweb_malloc(%d)", (int)size);
//  return malloc(size);
//}
//
//static void nxweb_free(void* ptr) {
//  nxweb_log_error("nxweb_free()");
//  free(ptr);
//}

static nxweb_request* new_request(nxweb_connection* conn) {
  nxweb_request* req;
  //if (!conn->data.chunk) obstack_specify_allocation(&conn->data, DEFAULT_CHUNK_SIZE, 0, nxweb_malloc, nxweb_free);
  if (!conn->data.chunk) obstack_specify_allocation(&conn->data, DEFAULT_CHUNK_SIZE, 0, malloc, free);
  req=obstack_alloc(&conn->data, sizeof(nxweb_request));
  if (!req) return 0;
  memset(req, 0, sizeof(nxweb_request));
  req->conn=conn;
  return req;
}

static nxweb_connection* new_connection(struct ev_loop *loop, nxweb_net_thread* tdata, int client_fd, struct in_addr* sin_addr) {
  nxweb_connection* conn=(nxweb_connection*)calloc(1, sizeof(nxweb_connection));
  if (!conn) return 0;

  conn->request=new_request(conn);
  if (!conn->request) {
    free(conn);
    nxweb_log_error("can't create initial request (maybe out of memory)");
    return 0;
  }

  conn->fd=client_fd;
  inet_ntop(AF_INET, sin_addr, conn->remote_addr, sizeof(conn->remote_addr));
  conn->loop=loop;
  conn->tdata=tdata;

  conn->cstate=NXWEB_CS_WAITING_FOR_REQUEST;

  ev_io_init(&conn->watch_socket_read, socket_read_cb, client_fd, EV_READ);
  ev_io_start(loop, &conn->watch_socket_read);
  // attempt reading immediately
  ev_feed_event(loop, &conn->watch_socket_read, EV_READ);

  ev_timer_init(&conn->watch_keep_alive_time, keep_alive_timeout_cb, KEEP_ALIVE_TIMEOUT, KEEP_ALIVE_TIMEOUT);
  ev_timer_start(loop, &conn->watch_keep_alive_time);

  // init but not start:
  ev_io_init(&conn->watch_socket_write, socket_write_cb, conn->fd, EV_WRITE);
  ev_async_init(&conn->watch_async_data_ready, data_ready_cb);

  __sync_add_and_fetch(&num_connections, 1);

  return conn;
}

static void close_connection(struct ev_loop *loop, nxweb_connection* conn) {
  if (ev_is_active(&conn->watch_keep_alive_time)) ev_timer_stop(loop, &conn->watch_keep_alive_time);
  if (ev_is_active(&conn->watch_read_request_time)) ev_timer_stop(loop, &conn->watch_read_request_time);
  if (ev_is_active(&conn->watch_write_response_time)) ev_timer_stop(loop, &conn->watch_write_response_time);
  if (ev_is_active(&conn->watch_socket_read)) ev_io_stop(loop, &conn->watch_socket_read);
  if (ev_is_active(&conn->watch_socket_write)) ev_io_stop(loop, &conn->watch_socket_write);
  if (ev_is_active(&conn->watch_async_data_ready)) ev_async_stop(loop, &conn->watch_async_data_ready);

  if (conn->cstate==NXWEB_CS_TIMEOUT || conn->cstate==NXWEB_CS_ERROR) _nxweb_close_bad_socket(conn->fd);
  else _nxweb_close_good_socket(conn->fd);

  if (conn->request && conn->request->sendfile_fd) close(conn->request->sendfile_fd);

  obstack_free(&conn->data, 0);
  // check if obstack has been initialized; free it if it was
  if (conn->user_data.chunk) obstack_free(&conn->user_data, 0);

  free(conn);

  int connections_left=__sync_sub_and_fetch(&num_connections, 1);
  assert(connections_left>=0);
//  if (connections_left==0) { // for debug only
//    nxweb_log_error("all connections closed");
//  }
}

static void conn_request_receive_complete(struct ev_loop *loop, nxweb_connection* conn, int unfinished) {
  nxweb_request* req=conn->request;

  // finish receiving request headers or content in case of error or timeout
  if (unfinished) obstack_finish(&conn->data);

  if (!req->content_length || req->content_received < req->content_length) req->request_body=0;

  ev_timer_stop(loop, &conn->watch_read_request_time);
  ev_io_stop(loop, &conn->watch_socket_read);
}

static void rearm_connection(struct ev_loop *loop, nxweb_connection* conn) {
  if (conn->request->sendfile_fd) close(conn->request->sendfile_fd);
  obstack_free(&conn->data, conn->request);
  conn->request=new_request(conn);
  if (!conn->request) {
    nxweb_log_error("can't create new request (maybe out of memory)");
    conn->cstate=NXWEB_CS_ERROR;
    close_connection(loop, conn);
    return;
  }

  conn->request_count++;
  conn->cstate=NXWEB_CS_WAITING_FOR_REQUEST;

  ev_io_start(loop, &conn->watch_socket_read);
  // attempt reading immediately
  ev_feed_event(loop, &conn->watch_socket_read, EV_READ);

  ev_timer_init(&conn->watch_keep_alive_time, keep_alive_timeout_cb, KEEP_ALIVE_TIMEOUT, KEEP_ALIVE_TIMEOUT);
  ev_timer_start(loop, &conn->watch_keep_alive_time);
}

static void start_sending_response(struct ev_loop *loop, nxweb_connection *conn) {

  _nxweb_finalize_response_writing_state(conn->request);

  if (!conn->request->out_headers) {
    _nxweb_prepare_response_headers(conn->request);
  }

  //conn->request->write_pos=0;
  //conn->request->header_sent=0;
  conn->cstate=NXWEB_CS_SENDING_HEADERS;

  if (!conn->sending_100_continue) {
    // if we are in process of sending 100-continue these watchers have already been activated
    ev_timer_init(&conn->watch_write_response_time, write_response_timeout_cb, WRITE_RESPONSE_TIMEOUT, WRITE_RESPONSE_TIMEOUT);
    ev_timer_start(loop, &conn->watch_write_response_time);
    ev_io_start(loop, &conn->watch_socket_write);
    // attempt writing immediately
    ev_feed_event(loop, &conn->watch_socket_write, EV_WRITE);
  }
}

static void write_response_timeout_cb(struct ev_loop *loop, struct ev_timer *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_write_response_time)));
  conn->cstate=NXWEB_CS_TIMEOUT;
  nxweb_log_error("write timeout - connection [%s] closed", conn->remote_addr);
  close_connection(loop, conn);
}

static void read_request_timeout_cb(struct ev_loop *loop, struct ev_timer *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_read_request_time)));

  nxweb_log_error("connection [%s] request timeout", conn->remote_addr);
  conn_request_receive_complete(loop, conn, 1);
  conn->keep_alive=0;
  nxweb_send_http_error(conn->request, 408, "Request Timeout");
  start_sending_response(loop, conn);
}

static void keep_alive_timeout_cb(struct ev_loop *loop, struct ev_timer *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_keep_alive_time)));
  conn->cstate=NXWEB_CS_TIMEOUT;
  nxweb_log_error("keep-alive connection [%s] closed; request_count=%d", conn->remote_addr, conn->request_count);
  close_connection(loop, conn);
}

static void data_ready_cb(struct ev_loop *loop, struct ev_async *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_async_data_ready)));
  nxweb_request* req=conn->request;

  ev_async_stop(loop, w);

  if (req->handler_result==NXWEB_NEXT) {
    // dispatch again
    dispatch_request(loop, conn);
    return;
  }

  start_sending_response(loop, conn);
}

static const char* response_100_continue = "HTTP/1.1 100 Continue\r\n\r\n";

static const void* next_write_bytes(nxweb_request* req, int* size) {
  nxweb_connection* conn=req->conn;
  const void* bytes=0;
  int bytes_avail=0;
  if (conn->sending_100_continue) { // sending 100-continue
    assert(req->write_pos<=strlen(response_100_continue));
    bytes=response_100_continue + req->write_pos;
    bytes_avail=strlen(bytes);
  }
  else {
    if (conn->cstate==NXWEB_CS_SENDING_HEADERS) {
      bytes=req->out_headers? req->out_headers + req->write_pos : 0;
      bytes_avail=bytes? strlen(bytes) : 0;
      if (!bytes_avail) {
        // header complete => start sending body
        conn->cstate=NXWEB_CS_SENDING_BODY;
        req->write_chunk=req->out_body_chunk;
        req->write_pos=0;
      }
    }
    if (conn->cstate==NXWEB_CS_SENDING_BODY) {
      if (!req->head_method) {
        while (req->write_chunk) {
          bytes=req->write_chunk->data + req->write_pos;
          bytes_avail=req->write_chunk->size - req->write_pos;
          if (bytes_avail) break;
          req->write_chunk=req->write_chunk->next;
          req->write_pos=0;
        }
      }
    }
  }
  *size=bytes_avail;
  return bytes;
}

static void socket_write_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_socket_write)));
  nxweb_request* req=conn->request;

  if (revents & EV_WRITE) {
    int bytes_avail, bytes_sent=0;
    const void* bytes;
    _nxweb_batch_write_begin(conn->fd);
    do {
      bytes=next_write_bytes(req, &bytes_avail);
      if (bytes_avail) {
        bytes_sent=write(conn->fd, bytes, bytes_avail);
        if (bytes_sent<0) {
          _nxweb_batch_write_end(conn->fd);
          if (errno!=EAGAIN) {
            char buf[1024];
            strerror_r(errno, buf, sizeof(buf));
            nxweb_log_error("write() returned %d: %d %s", bytes_sent, errno, buf);
            conn->cstate=NXWEB_CS_ERROR;
            close_connection(loop, conn);
          }
          return;
        }
        if (bytes_sent==0) {
          nxweb_log_error("write() returned 0");
        }
        req->write_pos+=bytes_sent;
        if (bytes_sent>0) {
          ev_timer_again(loop, &conn->watch_write_response_time);
        }
      }
      else if (req->sendfile_fd && req->sendfile_offset<req->sendfile_length && !req->head_method) {
        bytes_avail=req->sendfile_length - req->sendfile_offset;
        bytes_sent=sendfile(conn->fd, req->sendfile_fd, &req->sendfile_offset, bytes_avail);
        //nxweb_log_error("sendfile: %d bytes sent", bytes_sent); // debug only
        if (bytes_sent<0) {
          _nxweb_batch_write_end(conn->fd);
          if (errno!=EAGAIN) {
            char buf[1024];
            strerror_r(errno, buf, sizeof(buf));
            nxweb_log_error("write() returned %d: %d %s", bytes_sent, errno, buf);
          }
          return;
        }
      }
      else { // all sent
        if (conn->sending_100_continue) {
          if (conn->cstate==NXWEB_CS_SENDING_HEADERS) {
            // we've already reached sending headers phase
            // (this could have happened if we were writing slower than we were reading)
            // do not stop watchers, continue writing headers
            conn->sending_100_continue=0;
            req->write_pos=0;
            continue;
          }
        }
        _nxweb_batch_write_end(conn->fd);
        ev_io_stop(loop, &conn->watch_socket_write);
        ev_timer_stop(loop, &conn->watch_write_response_time);
        if (conn->sending_100_continue) {
          conn->sending_100_continue=0;
          req->write_pos=0;
        }
        else if (req->conn->keep_alive && !shutdown_in_progress) {
          // rearm connection for keep-alive
          rearm_connection(loop, conn);
        }
        else {
          conn->cstate=NXWEB_CS_CLOSING;
          //nxweb_log_error("connection closed OK");
          close_connection(loop, conn);
        }
        return;
      }
    } while (bytes_sent==bytes_avail);
    _nxweb_batch_write_end(conn->fd);
  }
}

static nxweb_result default_uri_callback(nxweb_uri_handler_phase phase, nxweb_request *req) {
  nxweb_send_http_error(req, 404, "Not Found");
  return NXWEB_OK;
}

static const nxweb_uri_handler default_handler={0, default_uri_callback, NXWEB_INPROCESS|NXWEB_HANDLE_ANY};

static const nxweb_uri_handler* find_handler(nxweb_request* req) {
  if (req->handler==&default_handler) return 0; // no more handlers
  char c;
  const char* uri=req->uri;
  int uri_len=strlen(uri);
  req->path_info=uri;
  const nxweb_module* const* module=req->handler_module? req->handler_module : nxweb_modules;
  const nxweb_uri_handler* handler=req->handler? req->handler+1 : (*module? (*module)->uri_handlers : 0);
  while (*module) {
    while (handler && handler->callback && handler->uri_prefix) {
      int prefix_len=strlen(handler->uri_prefix);
      if (prefix_len==0 || (prefix_len<=uri_len
          && strncmp(uri, handler->uri_prefix, prefix_len)==0
          && ((c=uri[prefix_len])==0 || c=='?' || c=='/'))) {
        req->path_info=uri+prefix_len;
        req->handler=handler;
        req->handler_module=module; // save position
        return handler;
      }
      handler++;
    }
    module++;
    handler=(*module)? (*module)->uri_handlers : 0;
  }
  handler=&default_handler;
  req->handler=handler;
  req->handler_module=module; // save position
  return handler;
}

static int prepare_request_for_handler(struct ev_loop *loop, nxweb_request* req) {
  const nxweb_uri_handler* handler=req->handler;
  unsigned flags=handler->flags;
  if ((flags&_NXWEB_HANDLE_MASK) && !(flags&NXWEB_HANDLE_ANY)) {
    if ((!(flags&NXWEB_HANDLE_GET) || (!!strcasecmp(req->method, "GET") && !!strcasecmp(req->method, "HEAD")))
      && (!(flags&NXWEB_HANDLE_POST) || !!strcasecmp(req->method, "POST"))) {
        nxweb_send_http_error(req, 405, "Method Not Allowed");
        start_sending_response(loop, req->conn);
        return -1;
    }
  }
  if (flags&NXWEB_PARSE_PARAMETERS) nxweb_parse_request_parameters(req, 0);
  if (flags&NXWEB_PARSE_COOKIES) nxweb_parse_request_cookies(req);
  return 0;
}

static void dispatch_request(struct ev_loop *loop, nxweb_connection *conn) {
  nxweb_request* req=conn->request;
  while (1) {
    const nxweb_uri_handler* handler=find_handler(req);
    assert(handler!=0); // default handler never returns NXWEB_NEXT
    if (prepare_request_for_handler(loop, req)) return;
    if (handler->flags&NXWEB_INPROCESS) {
      req->handler_result=handler->callback(NXWEB_PH_CONTENT, req);
      if (req->handler_result==NXWEB_NEXT) continue;
      start_sending_response(loop, conn);
    }
    else {
      // go async
      ev_async_start(loop, &conn->watch_async_data_ready);
      // hand over to worker thread
      nxweb_job job={conn};
      pthread_mutex_lock(&conn->tdata->job_queue_mux);
      if (!nxweb_job_queue_push(&conn->tdata->job_queue, &job)) {
        pthread_cond_signal(&conn->tdata->job_queue_cond);
        pthread_mutex_unlock(&conn->tdata->job_queue_mux);
      }
      else { // queue full
        pthread_mutex_unlock(&conn->tdata->job_queue_mux);
        nxweb_send_http_error(req, 503, "Service Unavailable");
        start_sending_response(loop, conn);
      }
    }
    break;
  }
}

static int next_room_for_read(struct ev_loop *loop, nxweb_connection* conn, void** room, int* size) {
  nxweb_request* req=conn->request;
  obstack* ob=&conn->data;

  int size_left=obstack_room(ob);

  if (size_left==0) {
    if (conn->cstate==NXWEB_CS_RECEIVING_HEADERS) {
      // do not expand; initial buffer should be enough
      // request headers too long
      nxweb_log_error("connection [%s] rejected (request headers too long)", conn->remote_addr);
      conn_request_receive_complete(loop, conn, 1);
      conn->cstate=NXWEB_CS_ERROR;
      conn->keep_alive=0;
      nxweb_send_http_error(req, 400, "Bad Request");
      start_sending_response(loop, conn);
      return -1;
    }
    else {
      assert(req->content_length<0); // unspecified length: chunked (or until close?)
      int received_size=req->content_received;
      int add_size=min(REQUEST_CONTENT_SIZE_LIMIT-received_size, received_size);
      if (add_size<=0) {
        // Too long
        obstack_free(ob, obstack_finish(ob));
        conn_request_receive_complete(loop, conn, 0);
        conn->cstate=NXWEB_CS_ERROR;
        conn->keep_alive=0;
        nxweb_send_http_error(req, 413, "Request Entity Too Large");
        // switch to sending response
        start_sending_response(loop, conn);
        return -1;
      }
      obstack_make_room(ob, add_size);
    }
  }

  *room=obstack_next_free(ob);
  *size=obstack_room(ob);
  assert(*size>0);
  return 0;
}

static int read_bytes_from_socket(struct ev_loop *loop, nxweb_connection* conn, void* room, int size) {

  int bytes_received=read(conn->fd, room, size);

  if (bytes_received<0) { // EAGAIN or ...?
    if (errno!=EAGAIN) {
      char buf[1024];
      strerror_r(errno, buf, sizeof(buf));
      nxweb_log_error("read() returned %d: %d %s", bytes_received, errno, buf);
      conn->cstate=NXWEB_CS_ERROR;
      close_connection(loop, conn);
    }
    return -1;
  }

  if (!bytes_received) { // connection closed by client
    // this is OK for keep-alive connections
    if (!conn->keep_alive)
      nxweb_log_error("connection [%s] closed (nothing to read)", conn->remote_addr);
    conn->cstate=NXWEB_CS_CLOSING;
    close_connection(loop, conn);
    return -2;
  }

  return bytes_received;
}

static int is_request_body_complete(nxweb_request* req, const char* body) {
  if (req->content_length==0) {
    return 1; // no body needed
  }
  else if (req->content_length>0) {
    return req->content_received >= req->content_length;
  }
  else if (req->chunked_request) { // req->content_length<0
    // verify chunked
    return _nxweb_verify_chunked(body, req->content_received)>=0;
  }
  return 1;
}

static void socket_read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  nxweb_connection *conn=((nxweb_connection*)(((char*)w)-offsetof(nxweb_connection, watch_socket_read)));
  nxweb_request* req=conn->request;
  obstack* ob=&conn->data;
  void* room;
  int room_size, bytes_received;

  do {
    if (next_room_for_read(loop, conn, &room, &room_size)) return;
    bytes_received=read_bytes_from_socket(loop, conn, room, room_size);
    if (bytes_received<=0) return;
    obstack_blank_fast(ob, bytes_received);

    if (conn->cstate==NXWEB_CS_WAITING_FOR_REQUEST) {
      // start receiving request
      ev_timer_stop(loop, &conn->watch_keep_alive_time);
      ev_timer_init(&conn->watch_read_request_time, read_request_timeout_cb, READ_REQUEST_TIMEOUT, READ_REQUEST_TIMEOUT);
      ev_timer_start(loop, &conn->watch_read_request_time);
      conn->cstate=NXWEB_CS_RECEIVING_HEADERS;
    }

    if (conn->cstate==NXWEB_CS_RECEIVING_HEADERS) {
      void* in_headers=obstack_base(ob);
      int in_headers_size=obstack_object_size(ob);
      char* end_of_request_headers=_nxweb_find_end_of_http_headers(in_headers, in_headers_size);

      if (!end_of_request_headers) {
        nxweb_log_error("partial headers receive (all ok)"); // rare case; log for debug
        return;
      }

      // null-terminate and finish
      obstack_1grow(ob, 0);
      req->in_headers=obstack_finish(ob);

      // parse request
      if (_nxweb_parse_http_request(req, req->in_headers==in_headers? end_of_request_headers:0, in_headers_size)) {
        conn_request_receive_complete(loop, conn, 1);
        conn->cstate=NXWEB_CS_ERROR;
        conn->keep_alive=0;
        nxweb_send_http_error(req, 400, "Bad Request");
        // switch to sending response
        start_sending_response(loop, conn);
        return;
      }

      if (is_request_body_complete(req, req->request_body)) {
        // receiving request complete
        if (req->chunked_request) _nxweb_decode_chunked_request(req);
        conn_request_receive_complete(loop, conn, 0);
        conn->cstate=NXWEB_CS_HANDLING;
        dispatch_request(loop, conn);
        return;
      }

      // so not all content received with headers

      if (req->content_length > REQUEST_CONTENT_SIZE_LIMIT) {
        conn_request_receive_complete(loop, conn, 1);
        conn->cstate=NXWEB_CS_ERROR;
        conn->keep_alive=0;
        nxweb_send_http_error(req, 413, "Request Entity Too Large");
        // switch to sending response
        start_sending_response(loop, conn);
        return;
      }

      if (req->content_length>0) {
        // body size specified; pre-allocate buffer for the content
        obstack_make_room(ob, req->content_length+1); // plus null-terminator char
      }

      if (req->content_received>0) {
        // copy what we have already received with headers
        void* new_body_ptr=obstack_next_free(ob);
        obstack_grow(ob, req->request_body, req->content_received);
        req->request_body=new_body_ptr;
      }

      // continue receiving request body
      conn->cstate=NXWEB_CS_RECEIVING_BODY;
      ev_timer_again(loop, &conn->watch_read_request_time);

      if (req->expect_100_continue && !req->content_received) {
        // send 100-continue
        conn->sending_100_continue=1;
        req->write_pos=0;
        ev_timer_init(&conn->watch_write_response_time, write_response_timeout_cb, WRITE_RESPONSE_TIMEOUT, WRITE_RESPONSE_TIMEOUT);
        ev_timer_start(loop, &conn->watch_write_response_time);
        ev_io_start(loop, &conn->watch_socket_write);
        // attempt writing immediately
        ev_feed_event(loop, &conn->watch_socket_write, EV_WRITE);
        // do not stop reading
      }
    }
    else if (conn->cstate==NXWEB_CS_RECEIVING_BODY) {
      req->content_received+=bytes_received;
      if (is_request_body_complete(req, obstack_base(ob))) {
        // receiving request complete
        // append null-terminator and close input buffer
        obstack_1grow(ob, 0);
        req->request_body=obstack_finish(ob);
        if (req->chunked_request) _nxweb_decode_chunked_request(req);
        conn_request_receive_complete(loop, conn, 0);
        conn->cstate=NXWEB_CS_HANDLING;
        dispatch_request(loop, conn);
        return;
      }
      nxweb_log_error("partial receive request body (all ok)"); // for debug only
    }
  } while (bytes_received==room_size);
}

static void net_thread_accept_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
  nxweb_net_thread* tdata=((nxweb_net_thread*)(((char*)w)-offsetof(nxweb_net_thread, watch_accept)));
  int client_fd;
  struct sockaddr_in client_addr;
  socklen_t client_len=sizeof(client_addr);
  nxweb_connection *conn;
  while (!shutdown_in_progress && (client_fd=accept(w->fd, (struct sockaddr *)&client_addr, &client_len))!=-1) {
    if (_nxweb_set_non_block(client_fd) || _nxweb_setup_client_socket(client_fd)) {
      _nxweb_close_bad_socket(client_fd);
      nxweb_log_error("failed to setup client socket");
      continue;
    }

    conn=new_connection(loop, tdata, client_fd, &client_addr.sin_addr);
    if (!conn) {
      nxweb_log_error("can't create new connection (maybe out of memory)");
      _nxweb_close_bad_socket(client_fd);
      continue;
    }
  }
  if (errno!=EAGAIN) {
    char buf[1024];
    strerror_r(errno, buf, sizeof(buf));
    nxweb_log_error("accept() returned -1: errno=%d %s", errno, buf);
  }
}

static void net_thread_shutdown_cb(struct ev_loop *loop, struct ev_async *w, int revents) {
  nxweb_net_thread* tdata=((nxweb_net_thread*)(((char*)w)-offsetof(nxweb_net_thread, watch_shutdown)));

  ev_async_stop(loop, &tdata->watch_shutdown);
  ev_io_stop(loop, &tdata->watch_accept);
}

static void* worker_thread_main(void* pdata) {
  nxweb_net_thread* tdata=(nxweb_net_thread*)pdata;
  nxweb_job job;
  nxweb_connection* conn;
  int result;

  while (!shutdown_in_progress) {
    pthread_mutex_lock(&tdata->job_queue_mux);
    while ((result=nxweb_job_queue_pop(&tdata->job_queue, &job)) && !shutdown_in_progress) {
      pthread_cond_wait(&tdata->job_queue_cond, &tdata->job_queue_mux);
    }
    pthread_mutex_unlock(&tdata->job_queue_mux);
    if (!result) {
      conn=job.conn;
      conn->request->handler_result=conn->request->handler->callback(NXWEB_PH_CONTENT, conn->request);
      ev_async_send(conn->loop, &conn->watch_async_data_ready);
    }
  }

  nxweb_log_error("worker thread exited");
  return 0;
}

static void* net_thread_main(void* pdata) {
  nxweb_net_thread* tdata=(nxweb_net_thread*)pdata;
  ev_run(tdata->loop, 0);
  ev_loop_destroy(tdata->loop);
  nxweb_log_error("net thread exited");
  return 0;
}

// Signal server to shutdown. Async function. Can be called from worker threads.
void nxweb_shutdown() {
  pthread_kill(main_thread_id, SIGTERM);
}

static void sigterm_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
  nxweb_log_error("SIGTERM/SIGINT(%d) received", w->signum);
  if (shutdown_in_progress) return;
  shutdown_in_progress=1; // tells net_threads to finish their work
  ev_break(main_loop, EVBREAK_ONE); // exit main loop listening to signals

  int i;
  nxweb_net_thread* tdata;
  for (i=0, tdata=net_threads; i<N_NET_THREADS; i++, tdata++) {
    // wake up workers
    pthread_mutex_lock(&tdata->job_queue_mux);
    pthread_cond_broadcast(&tdata->job_queue_cond);
    pthread_mutex_unlock(&tdata->job_queue_mux);

    ev_async_send(tdata->loop, &tdata->watch_shutdown);
  }
  alarm(5); // make sure we terminate via SIGALRM if some connections do not close in 5 seconds
}

static void on_sigalrm(int sig) {
  nxweb_log_error("SIGALRM received. Exiting");
  exit(EXIT_SUCCESS);
}

void _nxweb_main() {
  _nxweb_register_printf_extensions();

  struct ev_loop *loop=EV_DEFAULT;
  int i, j;

  pid_t pid=getpid();

  main_loop=loop;
  main_thread_id=pthread_self();

  nxweb_log_error("*** NXWEB startup: pid=%d port=%d ev_backend=%x N_NET_THREADS=%d N_WORKER_THREADS=%d"
                  " short=%d int=%d long=%d size_t=%d conn=%d req=%d",
                  (int)pid, NXWEB_LISTEN_PORT, ev_backend(loop), N_NET_THREADS, N_WORKER_THREADS,
                  (int)sizeof(short), (int)sizeof(int), (int)sizeof(long), (int)sizeof(size_t),
                  (int)sizeof(nxweb_connection), (int)sizeof(nxweb_request));

  // Block signals for all threads
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGQUIT);
  sigaddset(&set, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
    nxweb_log_error("Can't set pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  int listen_fd=_nxweb_bind_socket(NXWEB_LISTEN_PORT);
  if (listen_fd==-1) {
    // simulate succesful exit (error have been logged)
    // otherwise launcher will keep trying
    return;
  }

  nxweb_net_thread* tdata;
  for (i=0, tdata=net_threads; i<N_NET_THREADS; i++, tdata++) {
    tdata->loop=ev_loop_new(EVFLAG_AUTO);

    ev_async_init(&tdata->watch_shutdown, net_thread_shutdown_cb);
    ev_async_start(tdata->loop, &tdata->watch_shutdown);
    ev_io_init(&tdata->watch_accept, net_thread_accept_cb, listen_fd, EV_READ);
    ev_io_start(tdata->loop, &tdata->watch_accept);

    nxweb_job_queue_init(&tdata->job_queue);
    pthread_mutex_init(&tdata->job_queue_mux, 0);
    pthread_cond_init(&tdata->job_queue_cond, 0);
    for (j=0; j<N_WORKER_THREADS; j++) {
      pthread_create(&tdata->worker_threads[j], 0, worker_thread_main, tdata);
    }

    pthread_create(&tdata->thread_id, 0, net_thread_main, tdata);
  }


//  signal(SIGTERM, on_sigterm);
//  signal(SIGINT, on_sigterm);
  signal(SIGALRM, on_sigalrm);

  ev_signal watch_sigterm;
  ev_signal_init(&watch_sigterm, sigterm_cb, SIGTERM);
  ev_signal_start(loop, &watch_sigterm);

  ev_signal watch_sigint;
  ev_signal_init(&watch_sigint, sigterm_cb, SIGINT);
  ev_signal_start(loop, &watch_sigint);

  // Unblock signals for the main thread;
  // other threads have inherited sigmask we set earlier
  sigdelset(&set, SIGPIPE); // except SIGPIPE
  if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) {
    nxweb_log_error("Can't unset pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  FILE* f=fopen(NXWEB_PID_FILE, "w");
  if (f) {
    fprintf(f, "%d", (int)pid);
    fclose(f);
  }

  const nxweb_module* const * module=nxweb_modules;
  while (*module) {
    if ((*module)->server_startup_callback) {
      (*module)->server_startup_callback();
    }
    module++;
  }

  ev_run(loop, 0);

  close(listen_fd);

  for (i=0; i<N_NET_THREADS; i++) {
    pthread_join(net_threads[i].thread_id, NULL);
    for (j=0; j<N_WORKER_THREADS; j++) {
      pthread_join(net_threads[i].worker_threads[j], NULL);
    }
  }
}
