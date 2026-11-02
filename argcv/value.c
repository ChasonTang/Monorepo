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

static bool validate_ipv4_tail(const char *text, size_t len) {
  size_t offset = 0;
  for (unsigned int octet = 0; octet < 4; ++octet) {
    const size_t start = offset;
    unsigned int value = 0;
    while (offset < len && text[offset] >= '0' && text[offset] <= '9') {
      if (offset - start == 3 || (offset > start && text[start] == '0')) {
        return false;
      }
      /* At most three digits are accumulated, so arithmetic cannot overflow. */
      value = value * 10 + (unsigned int)(text[offset] - '0');
      if (value > 255) {
        return false;
      }
      ++offset;
    }
    if (offset == start) {
      return false;
    }
    if (octet < 3) {
      if (offset == len || text[offset] != '.') {
        return false;
      }
      ++offset;
    }
  }
  return offset == len;
}

bool argcv_validate_ipv6(const char *text, size_t len) {
  /* The longest form has six four-digit groups and four three-digit octets. */
  if (len < 2 || len > 45) {
    return false;
  }

  size_t offset = 0;
  unsigned int groups = 0;
  bool compressed = false;
  if (text[0] == ':') {
    if (text[1] != ':') {
      return false;
    }
    offset = 2;
    compressed = true;
  }

  while (offset < len) {
    if (groups == 8) {
      return false;
    }
    const size_t start = offset;
    while (offset < len) {
      const unsigned char byte = (unsigned char)text[offset];
      if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
            (byte >= 'A' && byte <= 'F'))) {
        break;
      }
      if (offset - start == 4) {
        return false;
      }
      ++offset;
    }

    if (offset < len && text[offset] == '.') {
      /* Recheck the entire component as decimal, including its first octet. */
      if (!validate_ipv4_tail(text + start, len - start)) {
        return false;
      }
      groups += 2;
      return compressed ? groups < 8 : groups == 8;
    }
    if (offset == start) {
      return false;
    }
    ++groups;
    if (offset == len) {
      break;
    }
    if (text[offset] != ':') {
      return false;
    }
    ++offset;
    if (offset == len) {
      return false;
    }
    if (text[offset] == ':') {
      if (compressed) {
        return false;
      }
      compressed = true;
      ++offset;
    }
  }

  /* Compression must account for at least one of the eight groups. */
  return compressed ? groups < 8 : groups == 8;
}
