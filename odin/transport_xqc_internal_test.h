/* odin/transport_xqc_internal_test.h */

#ifndef ODIN_TRANSPORT_XQC_INTERNAL_TEST_H_
#define ODIN_TRANSPORT_XQC_INTERNAL_TEST_H_

#if defined(ODIN_TRANSPORT_XQC_TESTING)

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "odin/transport.h"
#include "odin/transport_xqc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_xqc_stream_transport_test_ops_t {
  ssize_t (*recv)(xqc_stream_t *stream, unsigned char *recv_buf,
                  size_t recv_buf_size, uint8_t *fin);
  ssize_t (*send)(xqc_stream_t *stream, unsigned char *send_data,
                  size_t send_data_size, uint8_t fin);
  void (*set_user_data)(xqc_stream_t *stream, void *user_data);
} odin_xqc_stream_transport_test_ops_t;

void odin_xqc_stream_transport_test_set_ops(
    const odin_xqc_stream_transport_test_ops_t *ops);
unsigned int odin_xqc_stream_transport_test_interest(odin_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_TRANSPORT_XQC_TESTING) */

#endif /* ODIN_TRANSPORT_XQC_INTERNAL_TEST_H_ */
