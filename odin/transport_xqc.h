/* odin/transport_xqc.h
 *
 * xquic stream-backed Odin transport (RFC-016).
 *
 * The caller owns the xqc_stream_t, the xquic engine/connection integration,
 * stream creation, and stream close. The Odin wrapper adapts only stream-level
 * read/write/readiness callbacks to odin_transport_t and must be destroyed
 * while the xqc_stream_t is still valid so destroy can clear the stream
 * user-data slot it installed.
 */

#ifndef ODIN_TRANSPORT_XQC_H_
#define ODIN_TRANSPORT_XQC_H_

#include "odin/transport.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_xqc_stream_transport_create(xqc_stream_t *stream,
                                     odin_transport_ready_cb on_ready,
                                     void *user_data, odin_transport_t **out);

xqc_int_t odin_xqc_stream_transport_read_notify(xqc_stream_t *stream,
                                                void *strm_user_data);
xqc_int_t odin_xqc_stream_transport_write_notify(xqc_stream_t *stream,
                                                 void *strm_user_data);
void odin_xqc_stream_transport_closing_notify(xqc_stream_t *stream,
                                              xqc_int_t err_code,
                                              void *strm_user_data);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_TRANSPORT_XQC_H_ */
