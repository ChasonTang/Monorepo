/* A stateless, allocation-free parser for --flag and --option value. */

#ifndef ARGCV_ARGCV_H_
#define ARGCV_ARGCV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARGCV_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang assume_nonnull begin

/* Names exclude -- and must match the ASCII pattern [a-z][a-z0-9-]*. */
typedef struct {
  const char *name;
  bool requires_value;
} argcv_option_t;

/* Read only the member selected by the corresponding descriptor:
 * value for requires_value == true, present otherwise. */
typedef union {
  const char *_Nullable value;
  bool present;
} argcv_result_t;

typedef enum {
  ARGCV_OK = 0,
  ARGCV_INVALID_NAME,
  ARGCV_DUPLICATE_NAME,
  ARGCV_UNKNOWN_OPTION,
  ARGCV_MISSING_VALUE,
} argcv_status_t;

typedef struct {
  size_t option_index;
  int argv_index;
} argcv_error_t;

/* Parse argv[1..argc-1] after validating every descriptor. Matching is exact
 * and case-sensitive. A value is missing if there is no next argument or that
 * argument matches any registered option. All other strings, including empty
 * strings and unregistered --names, are accepted as values. Repetitions use the
 * last occurrence. No short options, positional arguments, = syntax, or --
 * end-of-options marker are supported.
 *
 * Preconditions: argc >= 0; argv is nonnull with argc + 1 elements, nonnull
 * strings before argv[argc], and argv[argc] == NULL. options and results are
 * nonnull with option_count entries; pass valid placeholders even for count 0
 * (neither is accessed). Each name is a nonnull NUL-terminated string. Output
 * storage must not overlap any input storage or the other output object. All
 * input remains valid and is not modified concurrently during the call.
 *
 * Results need no initialization and are valid only on ARGCV_OK. Each active
 * member is reset on success: absent flags are false, absent values are NULL.
 * Supplied values retain the exact argv pointer, borrowing its string storage;
 * later changes to that storage are visible. No input is modified or retained
 * elsewhere. Descriptors can be shared by calls with separate output storage.
 *
 * If error is nonnull, both fields are written on every return:
 *   OK:             {SIZE_MAX, -1}
 *   INVALID_NAME:   {invalid descriptor index, -1}
 *   DUPLICATE_NAME: {later descriptor index, -1}
 *   UNKNOWN_OPTION: {SIZE_MAX, offending argument index}
 *   MISSING_VALUE:  {descriptor index, argument index of the waiting option}
 * Descriptor errors precede argument errors; the first error ends the call.
 * See docs/rfc_001_argument_parser_design.md for the full contract. */
ARGCV_EXPORT argcv_status_t argcv_parse(int argc,
                                        char *_Nullable const *_Nonnull argv,
                                        const argcv_option_t *options,
                                        size_t option_count,
                                        argcv_result_t *results,
                                        argcv_error_t *_Nullable error);

#pragma clang assume_nonnull end

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ARGCV_ARGCV_H_ */
