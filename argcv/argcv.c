#include "argcv/argcv.h"

#include <string.h>

#pragma clang assume_nonnull begin

static bool valid_name(const char *name);
static size_t find_option(const char *argument, const argcv_option_t *options,
                          size_t option_count);
static argcv_status_t finish(argcv_status_t status,
                             argcv_error_t *_Nullable error,
                             size_t option_index, int argv_index);

#pragma clang assume_nonnull end

static bool valid_name(const char *name) {
  if (*name < 'a' || *name > 'z') {
    return false;
  }
  for (++name; *name != '\0'; ++name) {
    if (!((*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9') ||
          *name == '-')) {
      return false;
    }
  }
  return true;
}

/* The short-circuit check also makes empty and one-character strings safe. */
static size_t find_option(const char *argument, const argcv_option_t *options,
                          size_t option_count) {
  if (argument[0] == '-' && argument[1] == '-') {
    for (size_t j = 0; j < option_count; ++j) {
      if (strcmp(argument + 2, options[j].name) == 0) {
        return j;
      }
    }
  }
  return SIZE_MAX;
}

static argcv_status_t finish(argcv_status_t status, argcv_error_t *error,
                             size_t option_index, int argv_index) {
  if (error != NULL) {
    error->option_index = option_index;
    error->argv_index = argv_index;
  }
  return status;
}

argcv_status_t argcv_parse(int argc, char *const *argv,
                           const argcv_option_t *options, size_t option_count,
                           argcv_result_t *results, argcv_error_t *error) {
  for (size_t j = 0; j < option_count; ++j) {
    if (!valid_name(options[j].name)) {
      return finish(ARGCV_INVALID_NAME, error, j, -1);
    }
    for (size_t k = 0; k < j; ++k) {
      if (strcmp(options[j].name, options[k].name) == 0) {
        return finish(ARGCV_DUPLICATE_NAME, error, j, -1);
      }
    }
  }

  for (size_t j = 0; j < option_count; ++j) {
    if (options[j].requires_value) {
      results[j].value = NULL;
    } else {
      results[j].present = false;
    }
  }

  for (int i = 1; i < argc;) {
    size_t j = find_option(argv[i], options, option_count);
    if (j == SIZE_MAX) {
      return finish(ARGCV_UNKNOWN_OPTION, error, SIZE_MAX, i);
    }
    if (!options[j].requires_value) {
      results[j].present = true;
      ++i;
      continue;
    }
    if (i + 1 >= argc ||
        find_option(argv[i + 1], options, option_count) != SIZE_MAX) {
      return finish(ARGCV_MISSING_VALUE, error, j, i);
    }
    results[j].value = argv[i + 1];
    i += 2;
  }

  return finish(ARGCV_OK, error, SIZE_MAX, -1);
}
