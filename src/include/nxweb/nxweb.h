/*
 * Copyright (c) 2011-2012 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * This file is part of NXWEB.
 *
 * NXWEB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * NXWEB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with NXWEB. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef NXWEB_H
#define	NXWEB_H

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#define MEM_GUARD 64
#define nx_alloc(size) memalign(MEM_GUARD, (size)+MEM_GUARD)

#define REVISION VERSION

#include "nxweb_config.h"

#include <stdlib.h>

#include "nx_buffer.h"
#include "nx_file_reader.h"
#include "nx_event.h"
#include "misc.h"
#include "nx_workers.h"

#ifdef	__cplusplus
extern "C" {
#endif

struct stat;

enum nxweb_timers {
  NXWEB_TIMER_KEEP_ALIVE,
  NXWEB_TIMER_READ,
  NXWEB_TIMER_WRITE,
  NXWEB_TIMER_BACKEND,
  NXWEB_TIMER_100CONTINUE
};

typedef struct nx_simple_map_entry {
  const char* name;
  const char* value;
  struct nx_simple_map_entry* next;
} nx_simple_map_entry, nxweb_http_header, nxweb_http_parameter, nxweb_http_cookie;

enum nxweb_chunked_decoder_state_code {CDS_CR1=-2, CDS_LF1=-1, CDS_SIZE=0, CDS_LF2, CDS_DATA};

typedef struct nxweb_chunked_decoder_state {
  enum nxweb_chunked_decoder_state_code state;
  unsigned short final_chunk:1;
  unsigned short monitor_only:1;
  nxe_ssize_t chunk_bytes_left;
} nxweb_chunked_decoder_state;

typedef struct nxweb_http_request {

  nxb_buffer* nxb;

  // booleans
  unsigned http11:1;
  unsigned head_method:1;
  unsigned get_method:1;
  unsigned post_method:1;
  unsigned other_method:1;
  unsigned accept_gzip_encoding:1;
  unsigned expect_100_continue:1;
  unsigned chunked_encoding:1;
  unsigned chunked_content_complete:1;
  unsigned keep_alive:1;
  unsigned sending_100_continue:1;
  unsigned x_forwarded_ssl:1;

  // Parsed HTTP request info:
  const char* method;
  const char* uri;
  const char* http_version;
  const char* host;
  // const char* remote_addr; - get it from connection struct
  const char* cookie;
  const char* user_agent;
  const char* content_type;
  const char* content;
  nxe_ssize_t content_length; // -1 = unspecified: chunked or until close
  nxe_size_t content_received;
  const char* transfer_encoding;
  const char* accept_encoding;
  const char* range;
  const char* path_info; // points right after uri_handler's prefix

  time_t if_modified_since;

  const char* x_forwarded_for;
  const char* x_forwarded_host;

  nxweb_http_header* headers;
  nxweb_http_parameter* parameters;
  nxweb_http_cookie* cookies;

  struct nxweb_filter_data* filter_data[NXWEB_MAX_FILTERS];

  nxweb_chunked_decoder_state cdstate;

  nxe_data module_data;

} nxweb_http_request;

typedef struct nxweb_http_response {
  nxb_buffer* nxb;

  const char* host;
  const char* raw_headers;
  const char* content;
  nxe_ssize_t content_length;
  nxe_size_t content_received;

  unsigned keep_alive:1;
  unsigned http11:1;
  unsigned chunked_encoding:1;
  unsigned gzip_encoded:1;
  //unsigned chunked_content_complete:1;

  // Building response:
  const char* status;
  const char* content_type;
  const char* content_charset;
  nxweb_http_header* headers;
  time_t last_modified;

  int status_code;

  nxweb_chunked_decoder_state cdstate;

  const char* cache_key;
  int cache_key_root_len;
  const struct nxweb_mime_type* mtype;

  const char* sendfile_path;
  int sendfile_path_root_len;
  int sendfile_fd;
  off_t sendfile_offset;
  off_t sendfile_end;
  struct stat sendfile_info;

  nxe_istream* content_out;

} nxweb_http_response;

typedef struct nxweb_mime_type {
  const char* ext; // must be lowercase
  const char* mime;
  unsigned charset_required:1;
  unsigned gzippable:1;
  unsigned image:1;
} nxweb_mime_type;

#include "nxd.h"
#include "http_server.h"

extern const unsigned char PIXEL_GIF[43]; // transparent pixel


static inline char nx_tolower(char c) {
  return c>='A' && c<='Z' ? c+('a'-'A') : c;
}

static inline void nx_strtolower(char* dst, const char* src) { // dst==src is OK for inplace tolower
  while ((*dst++=nx_tolower(*src++))) ;
}

static inline void nx_strntolower(char* dst, const char* src, int len) { // dst==src is OK for inplace tolower
  while (len-- && (*dst++=nx_tolower(*src++))) ;
}

static inline int nx_strcasecmp(const char* s1, const char* s2) {
  const unsigned char* p1=(const unsigned char*)s1;
  const unsigned char* p2=(const unsigned char*)s2;
  int result;

  if (p1==p2) return 0;

  while ((result=nx_tolower(*p1)-nx_tolower(*p2++))==0)
    if (*p1++=='\0') break;

  return result;
}

static inline int nx_strncasecmp(const char* s1, const char* s2, int len) {
  const unsigned char* p1=(const unsigned char*)s1;
  const unsigned char* p2=(const unsigned char*)s2;
  int result;

  if (p1==p2 || len<=0) return 0;

  while ((result=nx_tolower(*p1)-nx_tolower(*p2++))==0)
    if (!len-- || *p1++=='\0') break;

  return result;
}

static inline const char* nx_simple_map_get(nx_simple_map_entry* map, const char* name) {
  while (map) {
    if (strcmp(map->name, name)==0) return map->value;
    map=map->next;
  }
  return 0;
}

static inline const char* nx_simple_map_get_nocase(nx_simple_map_entry* map, const char* name) {
  while (map) {
    if (strcasecmp(map->name, name)==0) return map->value;
    map=map->next;
  }
  return 0;
}

static inline nx_simple_map_entry* nx_simple_map_add(nx_simple_map_entry* map, nx_simple_map_entry* new_entry) {
  new_entry->next=map;
  return new_entry; // returns pointer to new map
}

static inline nx_simple_map_entry* nx_simple_map_itr_begin(nx_simple_map_entry* map) {
  return map;
}

static inline nx_simple_map_entry* nx_simple_map_itr_next(nx_simple_map_entry* itr) {
  return itr->next;
}


int nxweb_listen(const char* host_and_port, int backlog);
int nxweb_listen_ssl(const char* host_and_port, int backlog, _Bool secure, const char* cert_file, const char* key_file, const char* dh_params_file, const char* cipher_priority_string);
int nxweb_setup_http_proxy_pool(int idx, const char* host_and_port);
void nxweb_set_timeout(enum nxweb_timers timer_idx, nxe_time_t timeout);
void nxweb_run();

void nxweb_parse_request_parameters(nxweb_http_request *req, int preserve_uri); // Modifies conn->uri and request_body content (does url_decode inplace)
void nxweb_parse_request_cookies(nxweb_http_request *req); // Modifies conn->cookie content (does url_decode inplace)

static inline const char* nxweb_get_request_header(nxweb_http_request *req, const char* name) {
  return req->headers? nx_simple_map_get_nocase(req->headers, name) : 0;
}

static inline const char* nxweb_get_request_parameter(nxweb_http_request *req, const char* name) {
  return req->parameters? nx_simple_map_get(req->parameters, name) : 0;
}

static inline const char* nxweb_get_request_cookie(nxweb_http_request *req, const char* name) {
  return req->cookies? nx_simple_map_get(req->cookies, name) : 0;
}

static inline int nxweb_url_prefix_match(const char* url, const char* prefix, int prefix_len) {
  return !strncmp(url, prefix, prefix_len) && (!url[prefix_len] || url[prefix_len]=='/' || url[prefix_len]=='?' || url[prefix_len]==';');
}

static inline int nxweb_vhost_match(const char* host, int host_len, const char* vhost_suffix, int vhost_suffix_len) {
  if (*vhost_suffix=='.') {
    if (vhost_suffix_len==host_len+1) return !strncmp(host, vhost_suffix+1, host_len);
    if (vhost_suffix_len<=host_len) return !strncmp(host+(host_len-vhost_suffix_len), vhost_suffix, vhost_suffix_len);
    return 0;
  }
  else {
    return vhost_suffix_len==host_len && !strncmp(host, vhost_suffix, host_len);
  }
}

const nxweb_mime_type* nxweb_get_mime_type(const char* type_name);
const nxweb_mime_type* nxweb_get_mime_type_by_ext(const char* fpath_or_ext);

char* nxweb_url_decode(char* src, char* dst); // can do it inplace (dst=0)

void nxweb_set_response_status(nxweb_http_response* resp, int code, const char* message);
void nxweb_set_response_content_type(nxweb_http_response* resp, const char* content_type);
void nxweb_set_response_charset(nxweb_http_response* resp, const char* charset);
void nxweb_add_response_header(nxweb_http_response* resp, const char* name, const char* value);

static inline void nxweb_response_make_room(nxweb_http_response* resp, int min_size) {
  nxb_make_room(resp->nxb, min_size);
}
static inline void nxweb_response_printf(nxweb_http_response* resp, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  nxb_printf_va(resp->nxb, fmt, ap);
  va_end(ap);
}
static inline void nxweb_response_append_str(nxweb_http_response* resp, const char* str) {
  nxb_append_str(resp->nxb, str);
}
static inline void nxweb_response_append_data(nxweb_http_response* resp, const void* data, int size) {
  nxb_append(resp->nxb, data, size);
}
static inline void nxweb_response_append_char(nxweb_http_response* resp, char c) {
  nxb_append_char(resp->nxb, c);
}
static inline void nxweb_response_append_uint(nxweb_http_response* resp, unsigned long n) {
  nxb_append_uint(resp->nxb, n);
}

void nxweb_send_redirect(nxweb_http_response* resp, int code, const char* location);
void nxweb_send_redirect2(nxweb_http_response *resp, int code, const char* location, const char* location_path_info);
void nxweb_send_http_error(nxweb_http_response* resp, int code, const char* message);
int nxweb_send_file(nxweb_http_response *resp, char* fpath, int fpath_root_len, const struct stat* finfo, int gzip_encoded,
        off_t offset, size_t size, const nxweb_mime_type* mtype, const char* charset); // finfo and mtype could be null => autodetect
void nxweb_send_data(nxweb_http_response *resp, const void* data, size_t size, const char* content_type);

int nxweb_format_http_time(char* buf, struct tm* tm);
time_t nxweb_parse_http_time(const char* str);
int nxweb_remove_dots_from_uri_path(char* path);

// Internal use only:
char* _nxweb_find_end_of_http_headers(char* buf, int len, char** start_of_body);
int _nxweb_parse_http_request(nxweb_http_request* req, char* headers, char* end_of_headers);
void _nxweb_decode_chunked_request(nxweb_http_request* req);
nxe_ssize_t _nxweb_decode_chunked(char* buf, nxe_size_t buf_len);
nxe_ssize_t _nxweb_verify_chunked(const char* buf, nxe_size_t buf_len);
int _nxweb_decode_chunked_stream(nxweb_chunked_decoder_state* decoder_state, char* buf, nxe_size_t* buf_len);
void _nxweb_register_printf_extensions();
nxweb_http_response* _nxweb_http_response_init(nxweb_http_response* resp, nxb_buffer* nxb, nxweb_http_request* req);
void _nxweb_prepare_response_headers(nxe_loop* loop, nxweb_http_response* resp);
const char* _nxweb_prepare_client_request_headers(nxweb_http_request *req);
int _nxweb_parse_http_response(nxweb_http_response* resp, char* headers, char* end_of_headers);

#ifdef	__cplusplus
}
#endif

#endif	/* NXWEB_H */

