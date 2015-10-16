/* Copyright 2015- Benyu Zhang */

#ifndef _EXAMPLE_HANDLER_H_
#define _EXAMPLE_HANDLER_H_

#include <nxweb/nxweb.h>

#ifdef __cplusplus
extern "C"
{
#endif
extern nxweb_result compute(nxweb_http_server_connection* conn, nxweb_http_request* req, nxweb_http_response* resp);
#ifdef __cplusplus
}
#endif
#endif
