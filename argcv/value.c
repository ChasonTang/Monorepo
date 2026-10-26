#include "argcv/value.h"

#include <string.h>

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

bool argcv_split_hostport(const char *text, size_t len,
                          argcv_hostport_parts_t *out) {
  if (len == 0) {
    return false;
  }

  argcv_hostport_parts_t parts = {
      .host = text,
      .host_len = len,
      .port = NULL,
      .port_len = 0,
      .host_requires_ipv6 = false,
  };

  if (text[0] == '[') {
    const char *close = memchr(text + 1, ']', len - 1);
    if (close == NULL || close == text + 1) {
      return false;
    }

    parts.host = text + 1;
    parts.host_len = (size_t)(close - parts.host);
    parts.host_requires_ipv6 = true;
    const size_t suffix_offset = (size_t)(close - text) + 1;
    if (suffix_offset < len) {
      if (text[suffix_offset] != ':' || len - suffix_offset == 1) {
        return false;
      }
      parts.port = text + suffix_offset + 1;
      parts.port_len = len - suffix_offset - 1;
    }
  } else {
    const char *colon = memchr(text, ':', len);
    if (colon != NULL) {
      const size_t host_len = (size_t)(colon - text);
      const size_t port_len = len - host_len - 1;
      /* Check for a second colon before rejecting an empty host or port. */
      if (memchr(colon + 1, ':', port_len) != NULL) {
        parts.host_requires_ipv6 = true;
      } else {
        if (host_len == 0 || port_len == 0) {
          return false;
        }
        parts.host_len = host_len;
        parts.port = colon + 1;
        parts.port_len = port_len;
      }
    }
  }

  *out = parts;
  return true;
}
