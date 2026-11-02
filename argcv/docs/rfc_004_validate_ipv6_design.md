## 1. Purpose and scope

This RFC proposes `argcv_validate_ipv6`, a predicate that accepts a byte slice
only when the entire slice is a syntactically valid IPv6 address. It supports
hexadecimal groups, zero compression, and an IPv4 dotted-decimal tail.

The function provides the IPv6 validation selected by `host_requires_ipv6` in
[RFC 003](rfc_003_split_hostport_design.md). It can also be called independently
on any slice that satisfies the storage contract below.

Validation establishes syntax only. The function neither converts an address
to binary nor rewrites its text. Callers decide whether an address is suitable
for their use and handle hostname validation, standalone IPv4 validation, port
validation, and DNS resolution separately. Zone identifiers and network
prefixes are outside the accepted syntax.

## 2. Public API and contract

Add the following exported declaration to `argcv/value.h`, inside its existing
C linkage and nonnull annotation regions. The header already includes
`<stdbool.h>`, `<stddef.h>`, and `"argcv/export.h"`.

```c
ARGCV_EXPORT bool argcv_validate_ipv6(const char *text, size_t len);
```

The input is exactly `len` bytes starting at `text`; no NUL terminator is
required. For a C string, the usual length is `strlen(text)`, which excludes
the terminator.

### 2.1 Caller preconditions

- `text` must be a valid nonnull pointer with at least `len` readable bytes.
  This requirement also applies when `len == 0`; `""` is a suitable argument.
- The input must remain valid and unchanged for the duration of the call.
  Concurrent readers are allowed, but the caller must prevent conflicting
  writes.

The function is not required to check these preconditions at runtime, and the
nonnull annotation does not perform such a check. Passing a null pointer or a
length beyond readable storage violates the contract. Neither is a valid
negative test with an expected `false` result.

### 2.2 Results and side effects

For an input that satisfies the preconditions, the function returns `true` if
and only if all `len` bytes form one complete address in the grammar defined in
Section 3. Every other input returns `false`. In particular, an empty slice
returns `false` without reading any input bytes.

The boolean is the entire result: there are no output parameters, partial
results, consumed-length reports, or detailed error codes. The function leaves
input storage unchanged, allocates no dynamic memory, and performs no I/O or
name resolution. It retains no input storage or mutable global state. Results
are independent of locale and earlier calls, and the function is reentrant
when its preconditions hold.

### 2.3 Byte boundaries and accepted lengths

`len` is the sole boundary for recognition. The function may inspect only
bytes at offsets `0` through `len - 1`; it must neither read bytes outside that
range nor let them affect the result. A NUL anywhere inside the slice,
including at its end, is invalid. It cannot terminate recognition or turn a
valid prefix into a successful result.

Recognition uses ASCII bytes directly. There is no whitespace trimming, case
conversion, Unicode conversion, percent decoding, or normalization. Whitespace
and bytes with the high bit set are invalid at every position.

Every valid representation is between 2 and 45 bytes long, inclusive. The
shortest is `::`. The longest combines six hexadecimal groups of four digits
with four decimal octets of three digits, separated by six colons and three
dots: `6 * 4 + 6 + 4 * 3 + 3 = 45` bytes. An implementation may reject lengths
outside this range immediately; a length inside the range does not establish
validity.

## 3. Accepted address grammar

The accepted language is `IPv6address` from
[RFC 3986, Section 3.2.2](https://www.rfc-editor.org/rfc/rfc3986#section-3.2.2),
including its `IPv4address` tail. The brackets used by `IP-literal` are not
part of the input. See also
[RFC 4291, Section 2.2](https://www.rfc-editor.org/rfc/rfc4291#section-2.2)
for IPv6 text representations.

### 3.1 A finite family of productions

The following equivalent grammar makes group counts explicit through a finite
family of productions. It is not ABNF. Quoted strings denote literal ASCII
sequences, `|` separates alternatives, juxtaposition means concatenation, and
`epsilon` denotes the empty string. The start symbol `IPv6` must match the
entire input slice.

```text
HEX       ::= "0" | ... | "9" | "a" | ... | "f" | "A" | ... | "F"
H16       ::= HEX | HEX HEX | HEX HEX HEX | HEX HEX HEX HEX
OCTET     ::= "0" | "1" | ... | "255"
V4        ::= OCTET "." OCTET "." OCTET "." OCTET

H[0]      ::= epsilon
H[1]      ::= H16
H[k]      ::= H[k - 1] ":" H16             for 2 <= k <= 8

M[0]      ::= V4
M[k]      ::= H[k] ":" V4                 for 1 <= k <= 5

IPv6      ::= H[8]
           | H[6] ":" V4
           | H[i] "::" H[j]               for i >= 0, j >= 0, i + j <= 7
           | H[i] "::" M[j]               for i >= 0, j >= 0, i + j <= 5
```

The ellipses in `HEX` enumerate the ASCII characters between each pair of
endpoints. `OCTET` enumerates the decimal spellings of 0 through 255 without
leading zeros: `0`, `10`, and `255` are allowed; `00`, `010`, and `256` are not.

Indices are shorthand for finite expansion. For example, one expanded rule is
`H[2] ::= H[1] ":" H16`. Expanding all indicated ranges yields an ordinary
context-free grammar, with no runtime attributes or context-sensitive rules.
Because both the alphabet and the maximum accepted length are finite, the
language is also regular. The nonterminals organize the specification; they
do not require a separate lexer, a parser stack, or a syntax tree.

### 3.2 Group counts and compression

Each `H16` contributes one 16-bit group. It contains one to four hexadecimal
digits, accepts either letter case, and may have leading zeros. An IPv4 tail
contributes two groups and must occupy the end of the address. Such tails are
not limited to the `::ffff:` prefix; `2001:db8::192.0.2.1` is also valid.

Without `::`, the address must supply exactly eight groups. With `::`, it must
supply fewer than eight explicit groups: the compression marker stands for
at least one all-zero group. At most one `::` is allowed. A lone colon at
either end, or a run of three or more colons anywhere, is invalid.

Compression and a preferred canonical spelling are optional. Syntactically
valid unspecified, loopback, link-local, multicast, and other address values
are all accepted. Validation imposes no connection or routing policy.

### 3.3 Representations outside the grammar

Every form below returns `false`.

| Form | Examples | Reason |
| --- | --- | --- |
| Bracketed address or endpoint | `[::1]`, `[::1]:443` | Bracket handling and port separation belong to `argcv_split_hostport`. |
| Address with a zone identifier | `fe80::1%eth0`, `fe80::1%3`, `fe80::1%25eth0` | Zone suffixes, including encoded forms, are outside this grammar. |
| Network prefix | `2001:db8::/64` | Prefix lengths require a separate API contract. |
| IPvFuture literal | `v1.fe80` | This function validates IPv6 only. |
| Standalone IPv4 address or hostname | `192.0.2.1`, `example.com` | Neither matches the start symbol `IPv6`. |

`::1:443` is a valid address in its entirety. The validator does not infer a
port from the final group. To express host `::1` with port `443`, first split
`[::1]:443`.

## 4. Composition with host and port splitting

After `argcv_split_hostport` succeeds, the caller uses its flag to select the
host validation rule:

```c
if (parts.host_requires_ipv6) {
  if (!argcv_validate_ipv6(parts.host, parts.host_len)) {
    return false;
  }
} else {
  /* Validate IPv4 or hostname syntax under a separately defined policy. */
}
/* Validate parts.port separately when a port is present. */
```

When `host_requires_ipv6` is true, a failed IPv6 check makes the host invalid.
The caller must not fall back to hostname or standalone IPv4 validation. For
example, `[example.com]:443` can be split successfully but fails host
validation. The rules for the false branch are outside this RFC's scope.

The validator receives only the host slice, with brackets already removed.
It needs no copy or added NUL terminator and has no dependency on the split
result structure. It does not inspect the flag or the port. As a result, the
host passes IPv6 validation in both `[::1]:65536` and `[::1]:4:4:3`, even though
the ports are invalid.

Caller tests cover validation dispatch, the prohibition on fallback after an
IPv6 failure, and composition with `argcv_parse_u16` for port parsing. The
required tests in this RFC cover only the public contract of
`argcv_validate_ipv6`.

## 5. Implementation strategy and module layout

### 5.1 Bounded byte scanning

A direct scanner can recognize the grammar while tracking the current offset,
the explicit group count, and whether `::` has appeared. If a component
contains a dot, validate the entire remaining suffix as `V4`, starting at that
component's first byte, and count it as two groups. Regardless of how the loop
is organized, success requires both valid component syntax and a valid final
group count.

Every read, including lookahead, must stay within `len`. Reject a hexadecimal
group as soon as it exceeds four digits. For each IPv4 octet, require one to
three ASCII decimal digits, reject a leading zero in a multi-digit spelling,
and establish that the value is at most 255 before proceeding. Enforcing
these bounds prevents arithmetic overflow on excessively long invalid input.
The existing public decimal parser allows arbitrarily many leading zeros, so
reusing it still requires the octet-format checks above.

The scan needs `O(1)` auxiliary space and at most `O(len)` time. It requires no
backtracking, dynamic allocation, NUL-terminated copy, regular-expression
engine, or platform networking API.

### 5.2 Source and test placement

Implement the function in `argcv/value.c`, using the existing
`//argcv:argcv_value` target. A private `V4` validation helper may be factored
out for later reuse; the only public API added by this RFC is the IPv6
validator.

Add tests to `argcv/testing/value_unittests.cpp`, built by
`//argcv/testing:argcv_unittests`. Also verify that the public header is usable
from both C and C++, and that the C++ tests link against the C implementation.

## 6. Required test cases

### 6.1 Assertions and notation

These tests are required when implementing the validator. They describe its
public contract without depending on the scanner's internal control flow.

- Each table row is an independent call unless stated otherwise. Rows marked
  as G01 or G02 representatives are covered by Section 6.7 and need no separate
  implementation.
- Quoted inputs use C string notation. The quotes and implicit terminating NUL
  are excluded from the input length. Explicit arrays contain only the listed
  elements, with no implicit terminator. Escapes such as `\t`, `\r`, `\n`, and
  `\0` denote actual bytes.
- Every call must satisfy Section 2.1 and assert the expected boolean result.
  Use writable input buffers, snapshot each entire buffer before the call,
  and compare every byte afterward. The full buffer must remain unchanged,
  including bytes outside the slice and any terminator.

The grammar and the expected results below define correctness. If platform
`inet_pton` is used for differential checking, account for possible differences
in zone suffixes, overlong hexadecimal groups, and decimal leading zeros.
Platform behavior must not replace this specification.

### 6.2 Valid hexadecimal addresses

Every case in this table returns `true`.

| ID | Input | `len` | Coverage |
| --- | --- | --- | --- |
| V01 | `"::"` | 2 | All eight groups compressed; the shortest valid input. |
| V02 | `"::1"` | 3 | Loopback with compression at the start. |
| V03 | `"2001:db8::1"` | 11 | Compression between explicit groups. |
| V04 | `"1:2:3:4:5:6:7:8"` | 15 | Eight uncompressed groups. |
| V05 | `"0:0:0:0:0:0:0:0"` | 15 | The unspecified address without compression. |
| V06 | `"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"` | 39 | Four digits in every group; the maximum length for a purely hexadecimal address. |
| V07 | `"2001:0DB8:0000:0000:0000:0000:0000:0001"` | 39 | Uppercase letters and retained leading zeros. |
| V08 | `"a:B:c:D:e:F:0:1"` | 15 | Mixed uppercase and lowercase letters. |
| V09 | `"0:00:000:0000:1:01:001:0001"` | 27 | Every permitted hexadecimal group width. |
| V10 | `"::1:2:3:4:5:6:7"` | 15 | G01 representative, `(i, j) = (0, 7)`; exactly one group compressed at the start. |
| V11 | `"1:2:3:4:5:6:7::"` | 15 | G01 representative, `(i, j) = (7, 0)`; exactly one group compressed at the end. |
| V12 | `"1:2:3::4:5:6:7"` | 14 | G01 representative, `(i, j) = (3, 4)`; exactly one group compressed in the middle. |
| V13 | `"fe80::1"` | 7 | A link-local address without a zone suffix. |
| V14 | `"ff02::1"` | 7 | Multicast address syntax is accepted. |
| V15 | `"::1:443"` | 7 | The final group belongs to the address and does not denote a port. |

### 6.3 Valid IPv4 tails

Every case in this table returns `true`.

| ID | Input | `len` | Coverage |
| --- | --- | --- | --- |
| M01 | `"::192.0.2.1"` | 11 | A decimal tail immediately after the compression marker. |
| M02 | `"::ffff:192.0.2.1"` | 16 | The common IPv4-mapped representation. |
| M03 | `"0:0:0:0:0:ffff:192.0.2.1"` | 24 | An IPv4-mapped address without compression. |
| M04 | `"1:2:3:4:5:6:192.0.2.1"` | 21 | Six hexadecimal groups followed by a tail that occupies two groups. |
| M05 | `"1:2:3:4:5::192.0.2.1"` | 20 | G02 representative, `(i, j) = (5, 0)`; exactly one group compressed immediately before the decimal tail. |
| M06 | `"::1:2:3:4:5:192.0.2.1"` | 21 | G02 representative, `(i, j) = (0, 5)`; exactly one group compressed at the start. |
| M07 | `"1:2::3:4:5:192.0.2.1"` | 20 | G02 representative, `(i, j) = (2, 3)`; compression between hexadecimal groups preceding the decimal tail. |
| M08 | `"::ffff:0.9.10.99"` | 16 | Boundary values for one-digit and two-digit decimal octets. |
| M09 | `"::ffff:100.199.249.255"` | 22 | Boundary values for three-digit decimal octets. |
| M10 | `"::ffff:255.255.255.255"` | 22 | All four octets at the upper bound. |
| M11 | `"ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"` | 45 | The maximum length of any valid input. |
| M12 | `"::0.0.0.0"` | 9 | Each zero octet is spelled with a single digit. |
| M13 | `"2001:db8::192.0.2.1"` | 19 | A mixed representation without the IPv4-mapped prefix. |

### 6.4 Invalid structures and excluded forms

Every case in this table returns `false`.

| ID | Input | `len` | Reason for rejection |
| --- | --- | --- | --- |
| E01 | `""` | 0 | Empty input. |
| E02 | `":"` | 1 | A single colon is not a compression marker. |
| E03 | `":1"` | 2 | A lone leading colon. |
| E04 | `"1:"` | 2 | A lone trailing colon. |
| E05 | `":::"` | 3 | Three consecutive colons. |
| E06 | `"1:::2"` | 5 | Three consecutive colons between groups. |
| E07 | `"1::2::3"` | 7 | Two compression markers. |
| E08 | `"1:2:3:4:5:6:7"` | 13 | Only seven groups, with no compression marker. |
| E09 | `"1:2:3:4:5:6:7:8:9"` | 17 | Nine groups. |
| E10 | `"1:2:3:4:5:6:7:8::"` | 17 | G01 representative, `(i, j) = (8, 0)`; compression follows eight explicit groups. |
| E11 | `"::1:2:3:4:5:6:7:8"` | 17 | G01 representative, `(i, j) = (0, 8)`; compression precedes eight explicit groups. |
| E12 | `"1:2:3:4::5:6:7:8"` | 16 | G01 representative, `(i, j) = (4, 4)`; compression appears among eight explicit groups. |
| E13 | `"12345::"` | 7 | A hexadecimal group exceeds four digits. |
| E14 | `"::00000"` | 7 | A five-digit group is invalid even when all digits are zero. |
| E15 | `"2001:db8::g"` | 11 | A letter outside the hexadecimal alphabet. |
| E16 | `"0x1::"` | 5 | A hexadecimal radix prefix. |
| E17 | `"+1::"` | 4 | A leading plus sign. |
| E18 | `"-1::"` | 4 | A leading minus sign. |
| E19 | `" ::1"` | 4 | A leading space. |
| E20 | `"::1 "` | 4 | A trailing space. |
| E21 | `":: 1"` | 4 | An internal space. |
| E22 | `"\t::1"` | 4 | A leading tab. |
| E23 | `"::1\r\n"` | 5 | A trailing line-ending sequence. |
| E24 | `"::1junk"` | 7 | A valid prefix does not excuse invalid trailing bytes. |
| E25 | `"[::1]"` | 5 | Brackets are not address bytes. |
| E26 | `"[::1]:443"` | 9 | A complete endpoint is outside the validator's input grammar. |
| E27 | `"2001:db8::/64"` | 13 | A prefix length is present. |
| E28 | `"fe80::1%eth0"` | 12 | A zone suffix names an interface. |
| E29 | `"fe80::1%3"` | 9 | A numeric zone suffix. |
| E30 | `"fe80::1%25eth0"` | 14 | Encoded zone syntax is not decoded. |
| E31 | `"v1.fe80"` | 7 | IPvFuture syntax. |
| E32 | `"192.0.2.1"` | 9 | A standalone IPv4 address. |
| E33 | `"example.com"` | 11 | A hostname. |
| E34 | `"1"` | 1 | A single group is not a complete address. |
| E35 | `"::1:"` | 4 | A lone trailing colon after compression. |
| E36 | `":1::"` | 4 | A lone leading colon before compression. |

### 6.5 Invalid IPv4 tails

Every case in this table returns `false`.

| ID | Input | `len` | Reason for rejection |
| --- | --- | --- | --- |
| X01 | `"::ffff:256.0.2.1"` | 16 | The first octet exceeds 255. |
| X02 | `"::ffff:192.0.2.256"` | 18 | The last octet exceeds 255. |
| X03 | `"::ffff:192.00.2.1"` | 17 | Zero spelled with multiple digits. |
| X04 | `"::ffff:192.0.2.01"` | 17 | A nonzero octet with a leading zero. |
| X05 | `"::ffff:000.0.0.0"` | 16 | Zero spelled with three digits. |
| X06 | `"::ffff:192.0.2"` | 14 | Only three decimal octets. |
| X07 | `"::ffff:192.0.2.1.5"` | 18 | Five decimal octets. |
| X08 | `"::ffff:192..2.1"` | 15 | An empty internal octet. |
| X09 | `"::ffff:.192.0.2.1"` | 17 | An empty first octet. |
| X10 | `"::ffff:192.0.2.1."` | 17 | An extra trailing dot. |
| X11 | `"::ffff:192.0.2.1:1"` | 18 | A hexadecimal group follows the decimal tail. |
| X12 | `"192.0.2.1::"` | 11 | A compression marker follows the decimal tail. |
| X13 | `"1:2:3:4:5:192.0.2.1"` | 19 | Only seven groups, with no compression marker. |
| X14 | `"1:2:3:4:5:6:7:192.0.2.1"` | 23 | Nine explicit groups. |
| X15 | `"1:2:3:4:5:6::192.0.2.1"` | 22 | G02 representative, `(i, j) = (6, 0)`; compression is present in addition to eight explicit groups. |
| X16 | `"::1:2:3:4:5:6:192.0.2.1"` | 23 | G02 representative, `(i, j) = (0, 6)`; leading compression precedes eight explicit groups. |
| X17 | `"::ffff:0xc0.0.2.1"` | 17 | Hexadecimal notation in a decimal octet. |
| X18 | `"::ffff:192.0.2.+1"` | 17 | A plus sign in a decimal octet. |
| X19 | `"::ffff:192.0.2.-1"` | 17 | A minus sign in a decimal octet. |
| X20 | `"::ffff:192.0.2.4294967296"` | 25 | An oversized value must not become valid through integer wraparound. |
| X21 | `"::ffff:192.0.2.a"` | 16 | A letter allowed in `H16` is invalid in a decimal octet. |
| X22 | `"::ffff:192.0.2."` | 15 | An empty fourth octet. |

### 6.6 Slice boundaries and arbitrary bytes

Declare each buffer with exactly the elements listed below. Pass
`buffer + offset` and the specified `len`, then verify that the entire buffer
is unchanged as required by Section 6.1.

| ID | Buffer elements | Offset | `len` | Result |
| --- | --- | --- | --- | --- |
| S01 | `{':', ':', '1'}` | 0 | 3 | `true` |
| S02 | `{'x', ':', ':', '1', 'y'}` | 1 | 3 | `true` |
| S03 | `{'x', ':', ':', '1', 'y'}` | 1 | 4 | `false` |
| S04 | `{'x', ':', ':', '1', 'y'}` | 0 | 4 | `false` |
| S05 | `{'x'}` | 0 | 0 | `false` |
| S06 | `{' ', ':', ':', '1', '\0'}` | 1 | 3 | `true` |
| S07 | `{':', ':', '1', '\0'}` | 0 | 4 | `false` |
| S08 | `{':', '\0', ':', '1'}` | 0 | 4 | `false` |
| S09 | `{'\0', ':', ':', '1'}` | 0 | 4 | `false` |
| S10 | `{':', ':', '1', 'g'}` | 0 | 3 | `true` |
| S11 | `{':', ':', '1', 'g'}` | 0 | 4 | `false` |
| S12 | `{':', ':', '1', ':'}` | 0 | 3 | `true` |
| S13 | `{':', ':', '1', ':'}` | 0 | 4 | `false` |
| S14 | `{':', ':', '1', '2', '3', '4', '5'}` | 0 | 6 | `true` |
| S15 | `{':', ':', '1', '2', '3', '4', '5'}` | 0 | 7 | `false` |
| S16 | `{(char)0xEF, (char)0xBC, (char)0x91, ':', ':'}` | 0 | 5 | `false` |
| S17 | `{':', ':', (char)0xFF}` | 0 | 3 | `false` |
| S18 | `{'x', '[', ':', ':', '1', ']', ':', '8', '0', 'y'}` | 2 | 3 | `true` |
| S19 | `{':', ':', 'f', 'f', 'f', 'f', ':', '1', '9', '2', '.', '0', '.', '2', '.', '1', '\0'}` | 0 | 17 | `false` |
| S20 | `{':', ':', 'f', 'f', 'f', 'f', ':', '1', '.', '2', '.', '3', '.', '2', '5', '6'}` | 0 | 15 | `true` |
| S21 | `{':', ':', 'f', 'f', 'f', 'f', ':', '1', '.', '2', '.', '3', '.', '2', '5', '6'}` | 0 | 16 | `false` |
| S22 | `{':', ':', 'f', 'f', 'f', 'f', ':', '1', '.', '2', '.', '3', '.', '4'}` | 0 | 12 | `false` |
| S23 | `{':', ':', 'f', 'f', 'f', 'f', ':', '1', '.', '2', '.', '3', '.', '4'}` | 0 | 14 | `true` |

S07-S09 place a NUL at the end, in the middle, and at the start of the slice.
S16 contains the UTF-8 encoding of the fullwidth digit `１` followed by `::`.
S12-S15 compare a valid slice with a longer invalid slice over the same
storage. S19 confirms that a NUL inside the slice after an IPv4 tail cannot
end validation early.

S20/S21 use the same buffer to distinguish a final octet of `25` from `256`:
a digit outside the slice must not participate in validation. S22/S23 use the
same buffer to distinguish three decimal octets from four; the validator must
not read the out-of-slice `.4` to complete the address.

When implementing this RFC, build and run the tests with `is_asan = true` on
platforms where the repository supports AddressSanitizer. E02, V01, M02, and
X22 must each use a separate writable array or allocated block of exactly
`len` bytes, with no NUL terminator, and pass the entire object as the slice.
These cases cover a single colon, the shortest valid address, a valid IPv4
tail, and a tail with an empty fourth octet. They need no duplicate calls.

For these four cases, the slice and object boundaries must coincide. A shorter
slice inside a larger buffer is not an adequate substitute: content comparison
cannot detect reads outside the slice, and AddressSanitizer may miss such
reads when they remain inside a valid object.

### 6.7 Generated grammar combinations and numeric boundaries

The finite families below cover the representative rows identified earlier
and add structural and numeric boundary coverage. A representative row's
`(i, j)` identifies its generated combination. Its group values illustrate the
structure only; generate the actual bytes as specified below, without needing
to reproduce the representative string byte for byte. Pass the exact byte
count of each generated string, excluding any additional terminator.

| ID | Construction | Expected result |
| --- | --- | --- |
| G01 | Enumerate every pair of nonnegative integers `(i, j)` with `i + j <= 9`. Join `i` copies of `1` with single colons, append `::`, then append `j` copies of `2` joined with single colons. An empty side contributes no bytes. | Return `true` if and only if `i + j <= 7`. This covers every compression position and explicit group counts one and two above the limit. |
| G02 | Enumerate every pair of nonnegative integers `(i, j)` with `i + j <= 7`. Construct the left side as in G01 and append `::`. For `j == 0`, append `192.0.2.1`; otherwise, append `j` copies of `2` joined with single colons, one more colon, and `192.0.2.1`. | Return `true` if and only if `i + j <= 5`. The tail occupies two groups, and compression must account for at least one more. |
| G03 | For every byte value from `0x00` through `0xFF`, validate the three-byte array `{':', ':', byte}`. | Return `true` only for ASCII `0` through `9`, `a` through `f`, and `A` through `F`; return `false` for every other byte value. |
| G04 | Start with `::ffff:1.2.3.4`. Select each of the four decimal octets in turn and replace it with each valid and invalid spelling listed below. | Every valid spelling returns `true` and every invalid spelling returns `false`, regardless of the octet's position. |
| G05 | Validate each of these inputs: 1024 ASCII `0` bytes followed by `::`; `::ffff:1.2.3.` followed by 1024 ASCII `9` bytes; and the 45-byte input from M11 with one ASCII `x` appended, passing all 46 bytes. | All return `false`. Overlong fields must not be accepted through overflow or truncation, and a valid 45-byte prefix is insufficient for success. |

For G04, the valid spellings are `0`, `9`, `10`, `99`, `100`, `199`, `200`,
`249`, `250`, and `255`. The invalid spellings are `256`, `999`, `00`, `01`,
`001`, `0000`, `+1`, `-1`, `0x1`, and `4294967296`.

### 6.8 Independence between calls

R01 validates the following sequence: `::1`, `:::`, `::`,
`::ffff:192.0.2.01`, and `::ffff:192.0.2.1`. Repeat the entire sequence using
the same buffers. Both passes must return `true`, `false`, `true`, `false`,
and `true`, respectively, demonstrating that earlier calls do not affect
later results.
