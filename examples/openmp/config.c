/*  Copyright 2015- Benyu Zhang */

#include "handler.h"
#include <nxweb/nxweb.h>

#define NXWEB_DEFAULT_CHARSET "utf-8"
#define NXWEB_DEFAULT_INDEX_FILE "index.htm"

NXWEB_DEFINE_HANDLER(compute_handler, .on_request = compute,
                     .flags = NXWEB_HANDLE_ANY | NXWEB_PARSE_PARAMETERS);

void handler_config_run(uint16_t max_net_threads)
{
  // Bind listening interfaces:
  if (nxweb_listen(nxweb_main_args.http_listening_host_and_port, 4096)) return;

  ////////////////////
  // Setup handlers:
  ////////////////////
  NXWEB_HANDLER_SETUP(compute, "/compute", compute_handler, .priority = 100);

  // Go!
  nxweb_run(max_net_threads);
}

