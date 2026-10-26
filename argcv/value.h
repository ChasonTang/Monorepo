/* Allocation-free parsing and splitting of byte slices. */

#ifndef ARGCV_VALUE_H_
#define ARGCV_VALUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argcv/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang assume_nonnull begin

/* Parse exactly len ASCII decimal digits into a value in 0..UINT16_MAX.
 * Leading zeros are allowed without a length limit. No NUL terminator is
 * required; a NUL within the slice, any other nondigit, an empty slice, or an
 * out-of-range value returns false and leaves *port unchanged. On success,
 * write the value to *port and return true. Zero is valid; the parameter name
 * does not impose port-specific validation.
 *
 * Preconditions: text is nonnull with at least len readable bytes (use a valid
 * nonnull address even for len == 0). port is nonnull and points to a writable
 * uint16_t that does not overlap the input. Input remains valid and unchanged
 * during the call; the caller prevents conflicting access to the output.
 * Output needs no initialization, but may be read only after success if it
 * was uninitialized. No input is modified and no storage is retained.
 * See docs/rfc_002_parse_u16_design.md for the full contract. */
ARGCV_EXPORT bool argcv_parse_u16(const char *text, size_t len, uint16_t *port);

typedef struct {
  const char *host;
  size_t host_len;
  const char *_Nullable port;
  size_t port_len;
  bool host_requires_ipv6;
} argcv_hostport_parts_t;

/* Split exactly len bytes into a nonempty host and an optional nonempty port.
 * A leading '[' uses the first ']' to end the host; the suffix must be empty
 * or ':' followed by a nonempty port. Without brackets, exactly one colon
 * separates host and port; two or more colons make the entire slice the host.
 * Bracketed hosts and unbracketed hosts with two or more colons set
 * host_requires_ipv6. This flag selects subsequent IPv6 validation; no host
 * content or numeric port validation is performed here.
 *
 * On success, write every field of *out and return true. Slices borrow the
 * input, exclude enclosing brackets and the port separator, and need not be
 * NUL-terminated. An absent port has port == NULL and port_len == 0. All bytes,
 * including embedded NULs and whitespace, are preserved. Empty input or a
 * structural error returns false and leaves the entire output unchanged.
 *
 * Preconditions: text is nonnull with at least len readable bytes (even for
 * len == 0). out is nonnull and points to a writable object that does not
 * overlap the input. Input remains valid and unchanged during the call and
 * while reading the borrowed slices; the caller prevents conflicting access
 * to the output. Output needs no initialization, but may be read only after
 * success if it was uninitialized. No allocation, I/O, or retained state.
 * See docs/rfc_003_split_hostport_design.md for the full contract. */
ARGCV_EXPORT bool argcv_split_hostport(const char *text, size_t len,
                                       argcv_hostport_parts_t *out);

#pragma clang assume_nonnull end

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ARGCV_VALUE_H_ */
