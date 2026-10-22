#include "argcv/value.h"

bool argcv_parse_u16(const char *text, size_t len, uint16_t *port) {
  if (len == 0) {
    return false;
  }

  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char byte = (unsigned char)text[i];
    if (byte < '0' || byte > '9') {
      return false;
    }

    /* value <= UINT16_MAX here, so the intermediate is at most 655359. */
    value = value * 10 + (uint32_t)(byte - '0');
    if (value > UINT16_MAX) {
      return false;
    }
  }

  *port = (uint16_t)value;
  return true;
}
