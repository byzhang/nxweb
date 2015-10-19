/* Copyright 2015- Benyu Zhang */

#define _Bool bool
#include "handler.h"

#include <blaze/Blaze.h>
#include <omp.h>
#include <vector>

using namespace std;

nxweb_result compute(nxweb_http_server_connection* conn, nxweb_http_request* nx_req, nxweb_http_response* resp) {
  (void)conn;
  (void)nx_req;
  const int size = 1000000;
  blaze::DynamicVector<float> a(size);
  blaze::DynamicVector<float> b(size);
  vector<unsigned int> seeds(omp_get_max_threads());
  for (size_t i = 0; i < seeds.size(); ++i) {
    seeds[i] = i;
  }

  #pragma omp parallel for schedule(static)
  for (int i = 0; i < size; ++i) {
    a[i] = rand_r(&seeds[omp_get_thread_num()]);
    b[i] = rand_r(&seeds[omp_get_thread_num()]);
  }

  for (int i = 0; i < 100; ++i) {
    a *= b;
  }
  nxweb_response_append_str(resp, "done");
  nxweb_set_response_content_type(resp, "application/json");
  return NXWEB_OK;
}
