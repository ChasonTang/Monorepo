/* Allocation-free conversion of byte slices to values. */

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

#pragma clang assume_nonnull end

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ARGCV_VALUE_H_ */
