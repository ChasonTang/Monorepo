## 1. Purpose and design rationale

This RFC proposes `argcv_validate_host_labels`, a validator for a complete byte
slice containing ASCII host labels. On success, it also reports whether every
byte is an ASCII digit or a dot. The function can be used independently or as
the host validation step for the `host_requires_ipv6 == false` branch of
[RFC 003](rfc_003_split_hostport_design.md).

Label validity and address validity are separate decisions. For example,
`999.1.2.3` satisfies the label rules even though it is not a valid IPv4 address.
The classification result lets an endpoint validator route such input to a
strict IPv4 check. A name such as `1-2` also satisfies the label rules, but its
hyphen makes the classification `false`. The flag therefore describes the
input's bytes; it does not establish whether the input is an address.

The function checks label structure and character classes only. It does not
validate IPv4 addresses, ports, domain registration, or top-level domains,
convert addresses, or perform DNS queries. Success says nothing about whether
a name exists, is reachable, or will pass certificate identity verification.
Local network names and public domain names follow the same rules.

## 2. Public API and contract

Add the declaration to `argcv/value.h`, inside the existing C linkage and
nonnull annotation regions:

```c
ARGCV_EXPORT bool argcv_validate_host_labels(
    const char *text, size_t len, bool *only_digits_and_dots);
```

### 2.1 Caller preconditions and slice boundaries

- `text` is a valid nonnull pointer with at least `len` readable bytes, including
  when `len == 0`. Input storage remains valid and unchanged throughout the
  call.
- `only_digits_and_dots` is nonnull and points to a writable `bool` object that
  does not overlap the input slice. The caller prevents conflicting access to
  that output object.

These are caller obligations. The function need not detect violations at
runtime, and tests must not violate them to construct failure cases.

The input is exactly `[text, text + len)`. No NUL terminator is required, and
bytes outside that range must neither be read nor affect the result. A NUL at
any position within the slice is invalid. For a C string, pass the length
excluding its terminator. An empty slice returns `false` without reading input.

### 2.2 Validation result and classification

For a call that satisfies the preconditions, the return value is `true` if and
only if the entire slice meets every rule in Section 3.

| Return value | Effect on `*only_digits_and_dots` |
| --- | --- |
| `true` | Writes `true` if every byte of the original slice is an ASCII digit or `.`; otherwise writes `false`. |
| `false` | Leaves the output unchanged. Its previous value does not classify the current input. |

The classification includes any permitted trailing dot. Either a letter or a
hyphen makes it `false`; checking only for the absence of letters would be
incorrect. No IPv4 rules are applied to the number of components, their values,
their digit counts, or leading zeros.

The output object need not be initialized before the call. If it was
uninitialized, read it only after success. The API does not report an error
position, a consumed length, or an error category.

### 2.3 Preservation and side effects

The function leaves the input unchanged, makes no copy of it, and retains no
reference to it. It allocates no dynamic memory, performs no I/O, and depends
on no mutable global state. It is reentrant when the preconditions hold.
Character recognition uses explicit ASCII byte comparisons, independent of
locale and whether plain `char` is signed.

Validation performs no normalization: it does not trim whitespace, fold case,
remove a trailing dot, convert Unicode or IDNA names, or decode percent or
backslash escapes. The caller keeps the original slice throughout validation
and any subsequent processing.

## 3. Accepted names

### 3.1 Label grammar

The character structure follows the preferred name syntax in
[RFC 1035, Section 2.3.1](https://www.rfc-editor.org/rfc/rfc1035.html#section-2.3.1),
with the leading-digit allowance from
[RFC 1123, Section 2.1](https://www.rfc-editor.org/rfc/rfc1123.html#section-2.1):

```abnf
alpha       = %x41-5A / %x61-7A
digit       = %x30-39
alnum       = alpha / digit
ldh         = alnum / %x2D
label       = alnum / (alnum *61ldh alnum)
host-labels = label *("." label) ["."]
```

Each label contains 1 to 63 bytes. Its first and last bytes must be ASCII
letters or digits; interior bytes may also be ASCII hyphens. Consecutive
hyphens are allowed.

A name contains at least one label and may end in one dot representing the
root. A root dot by itself, a leading dot, and consecutive dots are invalid.
Single-label names, entirely numeric labels, and numeric final labels are
valid. This validator imposes no public TLD policy.

### 3.2 Total length and the DNS wire limit

Let `core_len` be the slice length after excluding at most one trailing dot
for counting purposes. A valid name must satisfy `1 <= core_len <= 253`.
The maximum input length is therefore 253 bytes without a trailing dot and
254 bytes with one. A 254-byte name without a trailing dot is invalid.
Excluding the dot for this calculation does not alter the input.

The bound follows from the uncompressed DNS representation. For `k` labels
containing a total of `S` content bytes, the textual core uses `k - 1`
separating dots. The wire representation instead uses one length byte per
label and one terminating root byte:

```text
core_len = S + k - 1
wire_len = S + k + 1 = core_len + 2 <= 255
```

See the size limits in
[RFC 1035, Section 2.3.4](https://www.rfc-editor.org/rfc/rfc1035.html#section-2.3.4)
and the label encoding in
[Section 3.1](https://www.rfc-editor.org/rfc/rfc1035.html#section-3.1).

### 3.3 ASCII input and internationalized names

The grammar excludes underscores, asterisks, slashes, backslashes, percent
signs, colons, brackets, whitespace, control characters, and all bytes with
the high bit set. Endpoint strings, IPv6 addresses, and names supplied
directly as UTF-8 therefore fail validation.

Callers must perform any internationalization conversion before calling this
function. An input such as `xn--bcher-kva.example` is checked only against the
ASCII label rules; acceptance does not certify its Punycode or IDNA semantics.

## 4. Use in endpoint validation and resolver integration

### 4.1 Endpoint validation policy

The contract of `argcv_split_hostport` remains unchanged. A split failure
rejects the endpoint. After a successful split, `host_requires_ipv6` selects
the validation path:

| Split result | Host validation |
| --- | --- |
| `host_requires_ipv6 == true` | Call `argcv_validate_ipv6` from [RFC 004](rfc_004_validate_ipv6_design.md). |
| `host_requires_ipv6 == false` | Call `argcv_validate_host_labels`; read its classification only after success. |

A successful label check then determines how to interpret the host:

| `only_digits_and_dots` | Next step |
| --- | --- |
| `true` | Validate the original host slice as strict IPv4. |
| `false` | Accept it as a hostname; its labels have already been validated. |

Failure at any step rejects the endpoint. There is no fallback to a different
host interpretation. Validate an explicit port separately with
`argcv_parse_u16`; the caller chooses the default port and whether zero is
permitted.

Strict IPv4 validation requires exactly four decimal components, each in
`0..255`, with no leading zeros except for `0` itself. It must consume the
entire slice. Consequently, `1234`, `127.1`, `999.1.2.3`, `01.2.3.4`, and
`1.2.3.4.` pass label validation but fail this endpoint policy. In particular,
the trailing dot must not be removed before the IPv4 check. This routing rule
is an endpoint policy, not a general DNS restriction on numeric names.

The existing `static validate_ipv4_tail` helper in [value.c](../value.c)
already implements these IPv4 rules. It accepts a complete IPv4 slice and
does not require IPv6 context, so composition within the same source file can
reuse it. Other modules would need a separately exposed API before using it.
This RFC proposes only the public function declared in Section 2.

### 4.2 Compatibility with c-ares

The accepted language is a subset of the name encoding rules in the vendored
c-ares 1.34.9. It excludes c-ares extensions such as `_`, `*`, `/`, escapes,
and hyphens at label boundaries. The relevant implementation is in the
[character rules](../../c-ares/src/lib/include/ares_str.h) and
[name encoder](../../c-ares/src/lib/record/ares_dns_name.c).

Compatibility here means that the original name can be encoded. It does not
guarantee that a query will be sent or that resolution will succeed. Search
domains, hosts files, mDNS, `.onion` policy, and the availability of A or AAAA
records remain resolver concerns.

Before passing a slice to `ares_getaddrinfo`, the adapter must copy all of its
bytes and append a terminating NUL. Preserve case and any trailing dot: that
dot affects search-domain processing. See the
[ares_getaddrinfo documentation](https://c-ares.org/docs/ares_getaddrinfo.html)
for its string interface and the
[ares_search documentation](https://c-ares.org/docs/ares_search.html) for
trailing-dot behavior.

The validator introduces no production dependency on c-ares. Compatibility
tests belong in the adapter layer: construct a DNS query record and call
`ares_dns_write` to check encoding. Network resolution results must not be
used to decide whether labels are valid.

## 5. Implementation strategy

Implement the function in `argcv/value.c` under the existing
`//argcv:argcv_value` target. Like the other slice utilities, it is independent
of option descriptors, `argc`, and `argv`.

A bounded scan can validate the structure and compute the classification
together:

1. Reject `len == 0` or `len > 254` without reading input. Otherwise, inspect
   the final byte to compute `core_len`, then enforce the total-length rule
   from Section 3.2.
2. Scan `[0, core_len)`, treating each byte as `unsigned char` and comparing
   explicit ASCII values. Track the current label length, whether its previous
   byte was alphanumeric, and a local classification flag initially set to
   `true`.
3. For a label byte, check the allowed character set, the first-byte rule,
   and the 63-byte label limit. Clear the classification flag on a letter or
   hyphen. At each separating dot, require a nonempty label ending in an
   alphanumeric byte, then start a new label. Apply that same completion check
   after the scan. This final check rejects cases such as `a..`, where removing
   one trailing dot from the scan range still leaves an empty final label.
4. Only after every check succeeds, write the local classification to
   `*only_digits_and_dots` and return `true`.

Every failure path precedes the single output assignment, so failed calls
preserve the output even if some labels were valid. Worst-case running time is
`O(len)` with `O(1)` auxiliary space; oversized inputs are rejected using the
length alone.

## 6. Required tests

### 6.1 Test conventions and shared assertions

Add implementation tests to `argcv/testing/value_unittests.cpp`, using
`//argcv/testing:argcv_unittests`. The cases below specify the proposed API's
behavior.

Text examples denote literal bytes, excluding any terminating NUL; backslash
escapes are not decoded. Hexadecimal sequences denote actual bytes. Exercise
each listed input and length combination, always satisfying the preconditions
in Section 2.1.

Unless a case specifies an initial value or a call sequence, run it twice:
once with the output initialized to `false` and once to `true`. On success,
check both the return value and the classification. On failure, check for
`false` and verify that the output retains its previous value.

Use writable input buffers and compare each entire buffer before and after
the call. Include any terminating NUL actually present in storage and any
sentinel bytes outside the slice. Every call must leave the full buffer
unchanged.

### 6.2 Structure and classification

Every input in this table must succeed:

| Coverage | Inputs | `only_digits_and_dots` |
| --- | --- | --- |
| Single labels and letter case | `a`, `Z`, `nas`, `localhost`, `EXAMPLE.CoM` | `false` |
| Trailing root dot | `a.`, `example.com.` | `false` |
| Leading digits and numeric labels | `3com`, `123.example`, `999.example.com`, `node.123` | `false` |
| Interior hyphens | `a-b`, `a--b`, `ab--cd.example`, `1-2`, `1-2.` | `false` |
| Acceptance based only on label syntax | `xn--bcher-kva.example`, `0x7f000001`, `nas.local`, `node.onion` | `false` |
| Strict IPv4 forms | `192.0.2.1`, `0.0.0.0`, `255.255.255.255` | `true` |
| No IPv4 range or leading-zero checks | `256.1.2.3`, `999.999.999.999`, `01.2.3.4`, `000.000.000.000` | `true` |
| No IPv4 digit-count or component-count limits | `0`, `9`, `1234`, `00000`, `127.1`, `1.2.3`, `1.2.3.4.5` | `true` |
| Numeric names with a trailing dot | `1.2.3.4.`, `1234.` | `true` |

Every input in this table must fail:

| Coverage | Inputs |
| --- | --- |
| Empty input or empty labels | Empty slice, `.`, `..`, `.a`, `a..b`, `a...b`, `1..2`, `a..`, `1.2.3.4..` |
| Hyphens at label boundaries | `-`, `-a`, `a.-b`, `a-`, `a-.b`, `a-.` |
| c-ares extensions and escape forms | `_a`, `a_b`, `a_`, `*.example`, `a*b`, `a/b.example`, `a\.b.example`, `a\046b.example`, `a%2Eb.example` |
| Endpoints, addresses, and network prefixes | `example.com:443`, `::1`, `2001:db8::1`, `[example.com]`, `[::1]`, `a]`, `192.0.2.1/24` |
| Other punctuation and whitespace | `a+b`, `a@b`, `a=b`, `a,b`, a leading space followed by `a`, `a b`, `a` followed by a trailing space |
| Control characters, NUL, and high-bit bytes | `09 61`, `61 0A`, `61 0D 0A`, `61 00 62`, `61 00`, `00 61`, `61 7F 62`, `61 FF 62` |
| UTF-8 input | `例子.example`, `１２３`, `a−b` (U+2212), `a。b` (U+3002) |

### 6.3 Length boundaries

Let `A(n)` be `n` ASCII `a` bytes and `D(n)` be `n` ASCII `1` bytes. The `+`
operator denotes byte-string concatenation. Define:

```text
H(m) = A(63) + "." + A(63) + "." + A(63) + "." + A(m)  // length 192 + m
N(m) = D(63) + "." + D(63) + "." + D(63) + "." + D(m)  // length 192 + m
P(k) = k single-byte "a" labels separated by dots, with no trailing dot
       // length 2*k - 1
```

| Inputs | Expected result |
| --- | --- |
| `A(63)`, `A(63) + ".a"`, `"a" + 61 hyphens + "b"` | Success; classification `false`. |
| `D(63)` | Success; classification `true`. |
| `A(64)`, `D(64)`, `A(64) + "."`, `"a" + 62 hyphens + "b"` | Failure: a label exceeds 63 bytes. |
| `A(64) + ".a"`, `"a." + A(64) + ".b"`, `"a." + A(64)` | Failure: an oversized first, middle, or final label, respectively. |
| `H(60)`, `H(61)`, `H(61) + "."` | Success; classification `false`. Total lengths are 252, 253, and 254 bytes, respectively. |
| `H(62)` | Failure: 254 bytes without a trailing dot. |
| `H(62) + "."`, `H(63)`, `P(128)`, `A(1024)` | Failure: total length exceeds 254 bytes. |
| `N(61)`, `N(61) + "."` | Success; classification `true`. IPv4 length and integer-range restrictions must not apply. |
| `P(127)`, `P(127) + "."` | Success; classification `false`. Exercises many short labels. |
| `H(60) + ".."` | Failure: `len == 254` and `core_len == 253`, but the trailing dots create an empty label. |

### 6.4 Slice boundaries and object boundaries

| Storage and call | Expected result |
| --- | --- |
| Separate arrays `{'a', 'b'}`, `{'1', '.', '2'}`, and `{'a', '.'}`, each passed at its full length | All succeed; classifications are `false`, `true`, and `false`, respectively. No NUL terminator is needed. |
| A separate array `{'a', '.', '.'}`, called with lengths 2 and 3 | Length 2 succeeds with classification `false`; length 3 fails. |
| Bytes `FF 61 62 FF`, passing the address of the second byte and length 2 | Success; classification `false`. Neither surrounding byte participates in validation. |
| Bytes `31 2E 32 58`, called with lengths 3 and 4 | Both succeed; classifications are `true` and `false`, respectively. |
| Bytes `61 2D 62`, called with lengths 1, 2, and 3 | Success, failure, success. Both successful calls classify as `false`. |
| Bytes `61 00`, called with lengths 1 and 2 | Length 1 succeeds with classification `false`; length 2 fails. |
| Bytes `31 2E 32 00`, called with lengths 3 and 4 | Length 3 succeeds with classification `true`; length 4 fails. |
| `char text[] = {'_'}`, called with length 0 | Failure; output unchanged. |
| `bool out[] = {true, false, true}`, passing `&out[1]` for input `1.2` | Success; only the middle element changes to `true`. |
| `bool out[] = {false, true, false}`, passing `&out[1]` for input `a` | Success; only the middle element changes to `false`. |
| Each of the preceding output arrays, passing `&out[1]` for input `a..b` | Failure; the entire output array remains unchanged. |

When implementing the function, build with `is_asan = true` and run the tests
on platforms where the repository supports AddressSanitizer. The four
independent input arrays in the first two rows must contain exactly the
listed bytes, with no terminator or spare capacity. Test each array at its
full object length at least once, so the slice boundary coincides with the
object boundary.

Buffer comparisons and sentinels detect writes, not out-of-bounds reads.
A slice inside a larger allocation cannot replace these ASan cases.

### 6.5 Reusing output and preserving it on failure

Run each sequence with one output object, without reinitializing it between
steps:

| Initial output | Input sequence | Return values | Output after each call |
| --- | --- | --- | --- |
| `false` | `1.2`, `a..b`, `a` | `true`, `false`, `true` | `true`, `true`, `false` |
| `true` | `a`, `1..2`, `1234` | `true`, `false`, `true` | `false`, `false`, `true` |

Also test successful calls for `a` and `1.2`, each with an uninitialized
`bool` output object. Read the object only after the call succeeds.

### 6.6 Exhaustive byte coverage

For each row, enumerate `byte = 0x00..0xFF`. Construct a complete writable
array without appending a NUL or using `strlen`. Compute expectations from an
independent, explicit ASCII allowlist; do not reuse the implementation's
character-classification helpers. Apply the two output initializations and
input-preservation checks from Section 6.1.

| Array | Condition for success | Condition for classification `true` on success |
| --- | --- | --- |
| `{'1', byte, '2'}` | `byte` is an ASCII letter, digit, `-`, or `.`. | `byte` is a digit or `.`. |
| `{byte, '1'}` | `byte` is an ASCII letter or digit. | `byte` is a digit. |
| `{'1', byte}` | `byte` is an ASCII letter, digit, or `.`. | `byte` is a digit or `.`. |

All other inputs fail without changing the output. A successful input that
does not meet the classification condition writes `false`. These three
positions exercise label interiors and separators, leading bytes, and final
bytes including the root dot, while covering every control and high-bit byte.
