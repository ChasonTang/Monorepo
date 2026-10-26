# RFC 003: Splitting Host and Port Byte Slices

## 1. Purpose and design rationale

This RFC proposes `argcv_split_hostport`, a function that separates a byte slice
into a required host and an optional port. It also tells the caller whether the
host must be validated as IPv6. The API is still at the design stage.

The central design choice is how to interpret colons. An IPv6 address may appear
without brackets when no port is specified. To attach an explicit port, the
caller must use `[host]:port`. An unbracketed input containing two or more colons
is always treated as a complete host; the function never guesses that the final
component is a port. For example, `::1:443` is one host with no explicit port,
whereas `[::1]:443` has host `::1` and port `443`.

Splitting establishes boundaries and selects the host validation rule. The
caller remains responsible for validating IPv6, IPv4, or hostname content and
for checking the port's numeric format and range. The function performs no DNS
lookup, supplies no default port, and leaves whitespace, case, and all other
input bytes unchanged.

A host must always contain at least one byte. A port may be absent, but once the
structural rules identify a port separator, at least one byte must follow it.
Here, "nonempty" means only that the slice length is greater than zero. Success
therefore guarantees the structure defined in Section 3, not a valid address or
port value.

## 2. Proposed API and storage contract

The declaration follows argcv's existing C linkage, export, and nullability
conventions:

```c
#include <stdbool.h>
#include <stddef.h>

#include "argcv/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang assume_nonnull begin

typedef struct {
  const char *host;
  size_t host_len;
  const char *_Nullable port;
  size_t port_len;
  bool host_requires_ipv6;
} argcv_hostport_parts_t;

ARGCV_EXPORT bool argcv_split_hostport(const char *text, size_t len,
                                     argcv_hostport_parts_t *out);

#pragma clang assume_nonnull end

#ifdef __cplusplus
}
#endif
```

### 2.1 Caller preconditions

`text` and `len` define the entire input range, `[text, text + len)`. No NUL
terminator is required. For a C string, callers normally pass `strlen(text)`,
excluding the terminator.

- `text` is nonnull and provides at least `len` readable bytes. Even for
  `len == 0`, pass a valid nonnull address, such as `""`.
- `out` is nonnull and points to a writable `argcv_hostport_parts_t` object that
  does not overlap the input slice.
- Input storage remains valid and unchanged throughout the call. The caller
  prevents conflicting concurrent access to the output object.

These are preconditions, not errors the function must detect at runtime. An
empty slice with a valid nonnull pointer is within the contract: it returns
`false` without reading input or modifying output.

### 2.2 Successful results

A `true` return value writes every field of `*out`:

| Field | Guarantee on success |
| --- | --- |
| `host` | Points to the first host byte in the input, after the opening `[` when brackets are used. |
| `host_len` | Positive host length in bytes, excluding the enclosing brackets and any port separator. |
| `host_requires_ipv6` | Selects IPv6 validation when `true`, or IPv4/hostname validation when `false`. |
| `port` | Points to the first byte after the port separator, or is `NULL` if no port is present. |
| `port_len` | Positive port length when present, or zero when absent. |

`host_requires_ipv6` is an instruction for subsequent validation. It neither
certifies that the host is valid nor indicates whether a port is present.
Section 3 defines the flag and slice boundaries together.

Port presence is represented by `port != NULL`. Every successful result
satisfies `(port != NULL) == (port_len > 0)`. The text `0` is an explicit port,
regardless of how the caller later interprets its value.

### 2.3 Failure, ownership, and side effects

A `false` return value leaves the entire output object unchanged. No partial
result or detailed error code is returned. The output need not be initialized
before the call, but an initially uninitialized object may be read only after
success.

Both result slices borrow the input storage. The caller must keep that storage
valid for as long as it reads the slices and must use their lengths: neither
slice is guaranteed to end with a NUL. The function copies no input bytes and
does not insert terminators or otherwise modify the input.

The function allocates no dynamic memory, performs no I/O, does not exit the
process, and retains no internal state.

## 3. Structural rules

Reject `len == 0` first. For a nonempty slice, the first byte selects one of two
rules: a leading `[` introduces a bracketed host; every other input uses the
unbracketed rule.

### 3.1 Bracketed host

Search for the first `]` starting at offset 1, within the input slice. A missing
`]` or a `]` at offset 1 is an error: the host must exist and be nonempty. The
bytes between `[` and that first `]` form the host, with
`host_requires_ipv6 = true`.

The suffix after that closing bracket determines the rest of the result:

| Suffix after the first `]` | Outcome |
| --- | --- |
| Empty | Success, with `port = NULL` and `port_len = 0`. |
| `:` followed by at least one byte | Success; all bytes after this colon form the port. |
| `:` alone | Failure: the explicit port is empty. |
| Anything that does not begin with `:` | Failure: unexpected byte after the closing bracket. |

Brackets are not matched recursively. For example, `[a[b]:443` produces host
`a[b`, while `[::1]]:443` fails because the byte after the first `]` is another
`]`. The port consumes the entire suffix after `]:`, including any additional
colons or nondigit bytes. Thus, `[::1]:4:4:3` produces port `4:4:3`; port
validation belongs to the caller.

### 3.2 Unbracketed host

Find the first colon and, if one exists, look for a second. Their presence
fully determines the interpretation:

| Colons within the slice | Host and port | `host_requires_ipv6` |
| --- | --- | --- |
| None | The entire slice is the host; the port is absent. | `false` |
| Exactly one | Split at that colon; both the host before it and the port after it must be nonempty. | `false` |
| Two or more | The entire slice is the host; the port is absent. | `true` |

An absent port always means `port = NULL` and `port_len = 0`. In the single-colon
case, a colon at either end of the slice causes failure. Otherwise, the host
and port are the bytes before and after that colon, respectively.

Check for a second colon before applying the single-colon emptiness checks.
Doing so admits `::` and `::1` as hosts that require IPv6 validation. Once a
second colon is found, further counting is unnecessary, and no attempt is made
to split at the last colon. `::1:443` therefore has no explicit port. A port
returned by the unbracketed rule can never contain a colon.

### 3.3 Exact byte boundaries

Every search is bounded by `len`. Functions that require NUL termination cannot
replace bounded searches. A NUL inside the slice is an ordinary byte for
scanning and contributes to the length of whichever result slice contains it.
It does not stop the search or make a slice empty. Structural delimiter rules
still apply: a NUL immediately after `]` causes failure because it is not `:`.
Bytes outside the input slice cannot affect the result and must not be read.

## 4. Implementation strategy and module layout

Build the result in local variables and assign it to `*out` only after every
structural check has passed. This keeps failure paths from changing the output,
including when the caller reuses a result from an earlier successful call.
Bounded scans require `O(len)` time in the worst case and `O(1)` auxiliary space.

The proposed declaration belongs in `argcv/value.h`, with its implementation in
`argcv/value.c`, using the existing `//argcv:argcv_value` target. The function is
independent of option descriptors and `argc`/`argv`, so it can operate on any
byte slice that satisfies the storage contract.

Add the unit tests to `argcv/testing/value_unittests.cpp`, run by the existing
`argcv_unittests` executable. These tests exercise splitting alone; address and
numeric port validation are outside their scope.

## 5. Required test cases

### 5.1 Assertions and notation

The cases below specify tests for `argcv_split_hostport`. Unless stated
otherwise, each row is an independent call with a C string whose terminating
NUL is excluded from `len`. The quotation marks in `""` and `" : "` identify an
empty string or preserve visible spaces; they are not input bytes.

Slices are written as `(offset, length)`, with offsets relative to the `text`
passed to that call. A host slice of `(1, 3)`, for example, requires
`out.host == text + 1` and `out.host_len == 3`. A port shown as `—` means both
`out.port == NULL` and `out.port_len == 0`; it does not mean failure.

For every successful call, assert `true` and check both slice pointers, both
lengths, and `host_requires_ipv6`. Compare slice contents byte for byte against
the corresponding ranges in the original input, including any NUL bytes. Also
assert `host_len > 0` and `(port != NULL) == (port_len > 0)`.

For an independent failure case, initialize every output field to a known value
and take a snapshot before calling. When reusing an output object, snapshot the
previous result instead. Assert `false` and compare every field against that
snapshot. `Unchanged` in a table means preservation, not zeroing.

Use writable arrays for the full input buffers in all cases. A shared test
helper should copy each entire array before the call and compare every byte
afterward, including bytes outside the supplied slice and any terminating NUL.
Every call must leave that full buffer unchanged.

### 5.2 Accepted structures

Every case below returns `true`. Some contain invalid host or port content on
purpose: they establish the boundary between splitting and validation.

| Input | `len` | Host slice | Port slice | `host_requires_ipv6` |
| --- | --- | --- | --- | --- |
| `a` | 1 | `(0, 1)` | — | `false` |
| `[a]` | 3 | `(1, 1)` | — | `true` |
| `host` | 4 | `(0, 4)` | — | `false` |
| `host:80` | 7 | `(0, 4)` | `(5, 2)` | `false` |
| `[::1]` | 5 | `(1, 3)` | — | `true` |
| `[::1]:443` | 9 | `(1, 3)` | `(6, 3)` | `true` |
| `::` | 2 | `(0, 2)` | — | `true` |
| `::1` | 3 | `(0, 3)` | — | `true` |
| `::1:443` | 7 | `(0, 7)` | — | `true` |
| `face:443` | 8 | `(0, 4)` | `(5, 3)` | `false` |
| `face::1` | 7 | `(0, 7)` | — | `true` |
| `[::1]:4:4:3` | 11 | `(1, 3)` | `(6, 5)` | `true` |
| `[a]::` | 5 | `(1, 1)` | `(4, 1)` | `true` |
| `host:4:4:3` | 10 | `(0, 10)` | — | `true` |
| `::1:` | 4 | `(0, 4)` | — | `true` |
| `host:abc` | 8 | `(0, 4)` | `(5, 3)` | `false` |
| `host:65536` | 10 | `(0, 4)` | `(5, 5)` | `false` |
| `host:000443` | 11 | `(0, 4)` | `(5, 6)` | `false` |
| `host:0` | 6 | `(0, 4)` | `(5, 1)` | `false` |
| `" : "` | 3 | `(0, 1)` | `(2, 1)` | `false` |
| `[example.com]:443` | 17 | `(1, 11)` | `(14, 3)` | `true` |
| `ho]st:443` | 9 | `(0, 5)` | `(6, 3)` | `false` |
| `h:[x]` | 5 | `(0, 1)` | `(2, 3)` | `false` |
| `[a[b]:443` | 9 | `(1, 3)` | `(6, 3)` | `true` |

### 5.3 Structural errors

Every case below returns `false` and leaves `out` unchanged.

| Input | `len` | Reason for failure |
| --- | --- | --- |
| `""` | 0 | Empty input. |
| `:` | 1 | Empty host before the only colon. |
| `:443` | 4 | Empty host. |
| `[]` | 2 | Empty bracketed host. |
| `[]:443` | 6 | Empty bracketed host. |
| `host:` | 5 | Empty port. |
| `[::1]:` | 6 | Empty port. |
| `[` | 1 | No bytes remain in which to find `]`. |
| `[::1` | 4 | Missing `]`. |
| `[::1]443` | 8 | The byte immediately after `]` is not a colon. |
| `[::1]]:443` | 10 | Another `]` immediately follows the first one. |

### 5.4 Length controls the search boundary

Store each complete string below in a buffer, but pass only the specified `len`.
Bytes beyond that length must have no effect on the result.

| Input buffer | `len` | Return value | Host slice | Port slice | `host_requires_ipv6` |
| --- | --- | --- | --- | --- | --- |
| `[::1]:443` | 0 | `false` | Unchanged | Unchanged | Unchanged |
| `[::1]:443` | 1 | `false` | Unchanged | Unchanged | Unchanged |
| `[::1]:443` | 4 | `false` | Unchanged | Unchanged | Unchanged |
| `[::1]:443` | 5 | `true` | `(1, 3)` | — | `true` |
| `[::1]:443` | 6 | `false` | Unchanged | Unchanged | Unchanged |
| `[::1]:443` | 7 | `true` | `(1, 3)` | `(6, 1)` | `true` |
| `[::1]:443` | 9 | `true` | `(1, 3)` | `(6, 3)` | `true` |
| `h:4:4` | 1 | `true` | `(0, 1)` | — | `false` |
| `h:4:4` | 2 | `false` | Unchanged | Unchanged | Unchanged |
| `h:4:4` | 3 | `true` | `(0, 1)` | `(2, 1)` | `false` |
| `h:4:4` | 5 | `true` | `(0, 5)` | — | `true` |
| `::1` | 1 | `false` | Unchanged | Unchanged | Unchanged |
| `::1` | 2 | `true` | `(0, 2)` | — | `true` |
| `::1` | 3 | `true` | `(0, 3)` | — | `true` |

### 5.5 Arrays without terminators and embedded NUL bytes

Declare each input as `char array[] = {...}` using exactly the listed elements,
with no implicit terminator. Pass `sizeof(array)` as `len`.

| Byte array | `len` | Return value | Host slice | Port slice | `host_requires_ipv6` |
| --- | --- | --- | --- | --- | --- |
| `{'['}` | 1 | `false` | Unchanged | Unchanged | Unchanged |
| `{'[', '\0', ']'}` | 3 | `true` | `(1, 1)` | — | `true` |
| `{'h', ':', '4', '4', '3'}` | 5 | `true` | `(0, 1)` | `(2, 3)` | `false` |
| `{'h', '\0', ':', '4', '4', '3'}` | 6 | `true` | `(0, 2)` | `(3, 3)` | `false` |
| `{'h', ':', '4', '\0', '3'}` | 5 | `true` | `(0, 1)` | `(2, 3)` | `false` |
| `{'h', ':', '\0'}` | 3 | `true` | `(0, 1)` | `(2, 1)` | `false` |
| `{'h', ':', '4', '\0', ':'}` | 5 | `true` | `(0, 5)` | — | `true` |
| `{'[', ':', ':', '1', ']', '\0'}` | 6 | `false` | Unchanged | Unchanged | Unchanged |

### 5.6 Reusing the output object

Run these calls in order with the same `out`. Keep every input buffer alive for
the entire test. On success, check every field against the matching case in
Section 5.2, with pointers relative to the current input. On failure, compare
every field against the preceding result.

| Step | Input | `len` | Return value | Output assertion |
| --- | --- | --- | --- | --- |
| 1 | `[::1]:443` | 9 | `true` | Matches Section 5.2. |
| 2 | `host:` | 5 | `false` | Preserves the result from step 1. |
| 3 | `::1` | 3 | `true` | Matches Section 5.2; port is `NULL` with length zero. |
| 4 | `host:80` | 7 | `true` | Matches Section 5.2; `host_requires_ipv6` is `false`. |
| 5 | `[::1` | 4 | `false` | Preserves the result from step 4. |
| 6 | `host` | 4 | `true` | Matches Section 5.2; port is `NULL` with length zero. |
| 7 | `[::1]:` | 6 | `false` | Preserves the result from step 6. |
| 8 | `[::1]:443` | 9 | `true` | Matches Section 5.2; `host_requires_ipv6` is `true` and the port is present. |
| 9 | `host:80` | 7 | `true` | Matches Section 5.2; `host_requires_ipv6` is `false` and the port is present. |
| 10 | `[::1]` | 5 | `true` | Matches Section 5.2; `host_requires_ipv6` is `true` and the port is `NULL` with length zero. |
