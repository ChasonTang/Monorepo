## 1. Purpose and design rationale

This RFC proposes `argcv_parse_u16`, a conversion function that reads an ASCII
decimal value from a byte slice and stores it as a `uint16_t`. An explicit length
lets callers use the same interface for command-line values and text fields
within protocol buffers, including fields without a terminating NUL.

The function follows the separation of parsing and application policy established
in [RFC-001](rfc_001_argument_parser_design.md). `argcv_parse` returns raw option
values; the caller chooses when to convert them and how to handle failure.
Application-specific limits, defaults, and diagnostic messages remain the
caller's responsibility. In particular, this is a general unsigned integer
conversion: zero is valid, even when an application would reject it as a port
number.

## 2. Public API and storage contract

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang assume_nonnull begin

bool argcv_parse_u16(const char *text, size_t len, uint16_t *port);

#pragma clang assume_nonnull end

#ifdef __cplusplus
}
#endif
```

The public header must apply the existing `ARGCV_EXPORT` convention to this
declaration and preserve the C linkage and nullability annotations shown above.

| Parameter | Meaning |
| --- | --- |
| `text` | Start of the input slice; no NUL terminator is required. |
| `len` | Number of input bytes. For a C string, callers normally pass `strlen(text)`. |
| `port` | Address of the output value, written only on success. The name does not impose port-specific validation. |

### 2.1 Caller preconditions

The nonnull annotations support compiler diagnostics and static analysis; they
do not add runtime pointer checks. The caller must provide valid storage:

- `text` is nonnull and provides at least `len` readable bytes. Even when
  `len == 0`, pass a valid nonnull address, such as `""`.
- `port` is nonnull and points to a writable `uint16_t` object.
- The output object does not overlap the input slice.
- Input storage remains valid and its contents remain unchanged throughout the
  call. The caller prevents conflicting concurrent access to the output object.

Violations of these preconditions are outside the parsing contract and need not
be detected at runtime. An empty slice backed by a valid nonnull pointer is
within the contract: it returns `false` without reading input or writing output.

### 2.2 Results and side effects

The return value reports whether the entire slice represents an acceptable
value. The function commits the result only after conversion succeeds.

| Return value | Output guarantee |
| --- | --- |
| `true` | `*port` contains the converted value. |
| `false` | `*port` retains its value from before the call. |

Empty input, invalid bytes, and values outside the supported range all return
`false`. The API does not distinguish these failures or return a partial value
or a consumed-prefix length. The output object need not be initialized before
the call, but an initially uninitialized output may be read only after a
successful conversion.

The function modifies no input, allocates no dynamic memory, performs no I/O,
does not exit the process, and retains no mutable global state.

## 3. Conversion rules

For a call satisfying the storage contract, conversion succeeds if and only if
all three conditions hold:

1. The slice is nonempty: `len > 0`.
2. Every byte in `text[0..len-1]` is an ASCII digit, in the range `0x30..0x39`
   (`'0'..'9'`).
3. The decimal value of the complete slice is in `0..UINT16_MAX`, or `0..65535`.

Leading zeros are allowed in any quantity and never select a different base.
Thus, `"010"` means decimal ten, and any nonempty sequence of zeros means zero.
There is no fixed input-length limit: a long sequence of leading zeros can still
represent a valid value. Limits on request sizes or command-line argument lengths
belong to the caller.

The grammar accepts digits alone. Signs, whitespace, hexadecimal prefixes,
decimal points, exponents, digit separators, and non-ASCII bytes are invalid.
Digit recognition is independent of locale; the function performs neither UTF-8
decoding nor Unicode digit conversion.

`len` defines the exact parsing boundary. A NUL byte inside the slice is invalid,
including at its final position. Bytes outside the slice are never read and
cannot affect the result, whether they are digits, delimiters, or a terminator.
Finding a valid numeric prefix is insufficient: the whole slice must satisfy
the grammar and range requirements.

## 4. Implementation strategy and module layout

### 4.1 Accumulate locally, then write the result

A single pass with a local `uint32_t` accumulator is sufficient:

1. Return `false` immediately if `len == 0`.
2. Initialize `value` to zero.
3. For each byte in the slice, check that it is an ASCII digit. Return `false`
   otherwise. Convert the byte to its digit value, compute
   `value = value * 10 + digit`, and return `false` if `value > UINT16_MAX`.
4. After every byte has passed, convert `value` to `uint16_t`, assign it to
   `*port`, and return `true`.

The range check after each digit establishes the loop invariant: the accumulator
is at most `65535` before the next digit is processed. The largest possible
intermediate result is therefore `65535 * 10 + 9 = 655359`, which fits in
`uint32_t`. This makes the arithmetic safe even for very long inputs. Narrowing
to `uint16_t` before checking the range would lose the evidence of overflow;
waiting until the end of the slice to check the range would allow the wider
accumulator itself to overflow.

An invalid byte or an out-of-range prefix can end the call immediately because
either makes success impossible. Keeping the accumulator local leaves every
failure path ahead of the only output assignment. Worst-case running time is
`O(len)`, with `O(1)` auxiliary space.

### 4.2 Keep value conversion independently usable

The proposed interface and implementation belong in `argcv/value.h` and
`argcv/value.c`. They should not depend on option descriptors or `argc`/`argv`,
so callers can use value conversion without invoking the argument parser.

A separate GN target, `//argcv:argcv_value`, could expose this module, with
`//argcv` declaring it as a public dependency.

## 5. Required test cases

### 5.1 Test conventions

The following cases specify tests to be implemented. They exercise the public
contract in Sections 2 and 3, independently of the implementation strategy.
Coverage includes syntax, range limits, slice boundaries, and output storage;
exhaustive enumeration of every value or byte is unnecessary.

Unless a case specifies otherwise, initialize the output to `43210` before each
call. On success, assert both a `true` return value and the expected output. On
failure, assert both a `false` return value and an unchanged output. Use writable
input buffers, save a copy before the call, and compare afterward to verify that
the input was not modified.

String lengths exclude the terminating NUL unless a case explicitly includes
it. When a row lists multiple inputs, run a separate call for each; the listed
lengths correspond to the inputs in order. Hexadecimal byte sequences denote
actual bytes, not text containing their hexadecimal spelling. A sequence of
1024 zeros means 1024 ASCII `'0'` bytes.

All test storage must satisfy Section 2.1, including the nonnull pointer
requirement for an empty slice.

### 5.2 Valid values, range boundaries, and leading zeros

| ID | Input | `len` | Expected output |
| --- | --- | --- | --- |
| V01 | `"0"` | 1 | `0` |
| V02 | `"789"` | 3 | `789` |
| V03 | `"65535"` | 5 | `65535` |
| V04 | `"010"` | 3 | `10` |
| V05 | `"000000"` | 6 | `0` |
| V06 | 1024 zeros followed by `65535` | 1029 | `65535` |

### 5.3 Invalid representations and overflow

Every case in this table must return `false` and leave the output unchanged.

| ID | Input | `len` | Requirement exercised |
| --- | --- | --- | --- |
| E01 | `""` | 0 | An empty slice is invalid. |
| E02 | `"65536"` | 5 | A value one above the upper bound is rejected. |
| E03 | `"4294967296"` | 10 | Integer wraparound must not make an oversized value appear valid. |
| E04 | 1024 zeros followed by `65536` | 1029 | Leading zeros do not change the range limit. |
| E05 | `"+42"`, `"-0"` | 3, 2 | Neither sign is accepted. |
| E06 | `" 42"`, `"4 2"`, `"42 "`, `"\t42"`, `"42\r\n"` | 3, 3, 3, 3, 4 | Whitespace is invalid at every position. |
| E07 | `"q42"`, `"4q2"`, `"42q"` | 3, 3, 3 | An invalid byte causes failure at the beginning, middle, or end. |
| E08 | `"0x2A"`, `"42.0"`, `"4e1"`, `"1_000"` | 4, 4, 3, 5 | Alternative numeric representations are rejected. |
| E09 | Bytes `EF BC 94 EF BC 92` (UTF-8 for `４２`) | 6 | Non-ASCII digits are rejected. |
| E10 | Byte `FF` | 1 | A byte with the high bit set is rejected regardless of whether `char` is signed. |
| E11 | Bytes `34 00 32`, `34 32 00` | 3, 3 | A NUL within the slice is invalid, including at the end; it must not terminate parsing or be ignored. |
| E12 | `"1/"`, `"1:"` | 2, 2 | Bytes immediately outside the ASCII digit range are rejected; V01 and V02 exercise the accepted boundaries. |

### 5.4 Slice boundaries and output storage

| ID | Setup and call | Expected outcome |
| --- | --- | --- |
| S01 | `char text[] = {'3', '1', '4'}`; pass `sizeof(text)` as the length. | Returns `true` and writes `314`; no NUL terminator is needed. |
| S02 | `char text[] = {'q', '4', '2', 'q'}`; pass `text + 1` with lengths `2` and `3` in separate calls. | Length `2` returns `true` and writes `42`. Length `3` returns `false` and leaves the output unchanged. |
| S03 | `uint16_t values[] = {111, 43210, 222}`; pass `&values[1]` as output. Parse `"42"` and `"65536"` separately, reinitializing the array before each call. | Success changes only the middle element to `42`. Failure leaves all three elements unchanged. |
| S04 | Reuse one output object to parse `"42"`, `"+42"`, and `"0"` in sequence. | Return values are `true`, `false`, `true`; output values after the calls are `42`, `42`, `0`. |
| S05 | `char text[] = {'4', '2', '9'}`; pass length `2`. | Returns `true` and writes `42`; a digit beyond the slice is not part of the value. |

## 6. Acceptance criteria

- The exported declaration matches Section 2, preserves its nonnull annotations,
  and is callable from both C and C++.
- The implementation satisfies the storage and conversion contracts in Sections
  2 and 3, recognizes ASCII digits independently of locale, and performs
  accumulation without integer overflow.
- Every case in Section 5 passes, including assertions that failures preserve
  the output and all calls preserve the input.
