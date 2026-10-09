## 1. Purpose and scope

`argcv` is a command-line argument parser written in C. A single call to
`argcv_parse` takes an option descriptor array, `argc`, `argv`, and a result
array. It records which options appeared and retains the argument strings
supplied as their values.

The API separates argument parsing from application policy. Every option may be
omitted, even if it requires a value when present. The application decides
whether values are valid, converts them to the desired types, supplies defaults,
and enforces relationships between options.

The supported syntax is deliberately small:

- A flag takes no value: `--name`.
- An option that requires a value consumes one argument: `--name value`.
- Short options, `--name=value`, positional arguments, subcommands, and a
  standalone `--` marker are not supported.

The caller owns the descriptor and result arrays, which may be allocated on the
stack. The library allocates no dynamic memory, modifies no input, and keeps no
mutable global state. It returns a status code and, optionally, an error
location; it neither prints diagnostics nor exits the process.

## 2. Public API and storage contract

```c
#pragma clang assume_nonnull begin

typedef struct {
  const char *name;
  bool requires_value;
} argcv_option_t;

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

argcv_status_t argcv_parse(int argc, char *_Nullable const *_Nonnull argv,
                          const argcv_option_t *options, size_t option_count,
                          argcv_result_t *results,
                          argcv_error_t *_Nullable error);

#pragma clang assume_nonnull end
```

Each descriptor defines an option's name and whether an occurrence requires a
value. `requires_value == false` defines a flag; `true` defines an option that
must be followed by a value whenever it appears. Section 4 defines the status
codes and error locations.

### 2.1 Layout and nullability

Structure members are ordered by decreasing size under the target ABI and use
natural alignment. The two members of `argcv_result_t` share storage. The
corresponding descriptor's `requires_value` field determines which member is
active, so the result needs no separate type tag.

On the repository's macOS x64 and arm64 targets, the layouts are:

| Type | Member offsets (bytes) | Alignment (bytes) | `sizeof` (bytes) |
| --- | --- | --- | --- |
| `argcv_option_t` | `name: 0`, `requires_value: 8` | 8 | 16 |
| `argcv_result_t` | `value: 0`, `present: 0` | 8 | 8 |
| `argcv_error_t` | `option_index: 0`, `argv_index: 8` | 8 | 16 |

Public declarations are enclosed in `#pragma clang assume_nonnull begin/end`.
Single-level pointers are nonnull by default; nullable pointers are explicitly
annotated with `_Nullable`. See the [Clang nullability documentation](https://clang.llvm.org/docs/AttributeReference.html#nullability-attributes).

Both pointer levels in `argv` are annotated explicitly: the outer pointer is
`_Nonnull`, and the element pointers are `_Nullable`, following
[Clang's rules for multilevel pointers](https://clang.llvm.org/doxygen/SemaType_8cpp_source.html#l04537).
The nullable element type accommodates the terminating `argv[argc]`; Section 2.2
requires every preceding element to be nonnull. The `const` qualifier prevents
the pointers stored in the array from being changed through this interface.

### 2.2 Caller preconditions

The caller must provide valid input and writable output storage. The `argc` and
`argv` requirements follow the [ISO C program startup conventions](https://www.open-std.org/JTC1/SC22/WG14/www/docs/n1570.pdf#page=31):

- `argc` is nonnegative. `argv` is nonnull and contains at least `argc + 1`
  elements. Every element in `argv[0..argc-1]` points to a valid string, and
  `argv[argc] == NULL`. If `argc == 0`, the array still contains at least one
  element, which is `NULL`. If `argc > 0`, `argv[0]` may point to an empty string.
- `options` and `results` are nonnull and provide at least `option_count`
  descriptors and writable results, respectively. When the count is zero, pass
  the addresses of valid placeholder objects; the library does not access them.
- Every descriptor's `name` is nonnull and points to a valid NUL-terminated
  string.
- `error` is either `NULL` or points to a writable `argcv_error_t` object.
- The result array and any error output object do not overlap each other or any
  input storage: the descriptors, `argv`, or their strings. All input remains
  valid and is not modified concurrently during the call.

The parser bounds its scan using `argc`; it does not check the terminating null
pointer. Tests and other callers that construct argument arrays must still
satisfy these preconditions. Violations are outside the API contract and are not
reported as parse errors.

### 2.3 Results, ownership, and reuse

`results[j]` corresponds to `options[j]`. Results are valid only when the call
returns `ARGCV_OK`:

| Option kind | Occurrence | Active member | Value |
| --- | --- | --- | --- |
| Flag | Absent | `present` | `false` |
| Flag | Present | `present` | `true` |
| Option requiring a value | Absent | `value` | `NULL` |
| Option requiring a value | Present; the final value is at `argv[k]` | `value` | Exactly the pointer stored in `argv[k]` |

The caller and parser access only the active member for each option. The result
array needs no initialization: every successful call sets the active member of
each of the first `option_count` entries, regardless of its previous contents.
No guarantees are made about the other storage bytes.

For an option that takes a value, test `value != NULL` to determine whether it
appeared. An empty string is a valid value and has a nonnull pointer. Repeated
options use the last occurrence: a flag remains `true`, and a value pointer is
replaced by the pointer from the final occurrence. An empty string replaces an
earlier value. Equal strings at different addresses still yield the pointer
from the final occurrence.

On failure, results may be untouched or partially written. The caller must not
read them.

The library neither copies nor frees strings. A returned value pointer remains
valid for as long as the corresponding string storage remains valid; later
changes to that storage are visible through the result. Descriptors need to
remain valid only for the duration of the call. They can be reused across calls
or shared by concurrent calls with separate result arrays, subject to the
preconditions in Section 2.2. There is no parser object to initialize or destroy.

### 2.4 Example

```c
#include "argcv/argcv.h"

int main(int argc, char **argv) {
  const argcv_option_t options[] = {
      {"verbose", false},
      {"host", true},
      {"port", true},
  };
  argcv_result_t results[sizeof(options) / sizeof(options[0])];
  argcv_error_t error;
  argcv_status_t status = argcv_parse(
      argc, argv, options, sizeof(options) / sizeof(options[0]), results, &error);
  if (status != ARGCV_OK) {
    return 1;
  }

  /* results[0].present records whether --verbose appeared.
   * results[1].value and results[2].value are the raw host and port strings.
   * A nonnull value means the option was supplied; the application validates it.
   */
  return 0;
}
```

## 3. Parsing model

Parsing has two phases: validate all descriptors, then consume arguments from
left to right. During argument parsing, an element is interpreted either as an
option or as the value requested by the preceding option. This distinction
determines how strings such as `--unknown` and `--` are handled.

### 3.1 Validate descriptors before parsing arguments

Descriptor names exclude the leading `--` and must match the ASCII pattern
`[a-z][a-z0-9-]*`. Starting at index zero, validate each descriptor in this order:

1. Check the name's syntax. An empty string or any other nonmatching name returns
   `ARGCV_INVALID_NAME`.
2. Compare the name with all earlier descriptors. A match returns
   `ARGCV_DUPLICATE_NAME` and identifies the current descriptor. The descriptors'
   `requires_value` fields do not affect duplicate detection.

Every descriptor must pass validation, including options that the command line
does not use. Only after all descriptors pass does the parser initialize flags
to `present = false` and options requiring values to `value = NULL`, writing only
each entry's active member. It then begins parsing arguments.

### 3.2 Read an option

Skip `argv[0]`. Starting at `i = 1`, process each unconsumed argument until
`i >= argc`:

1. Find a descriptor whose name, prefixed with `--`, exactly matches `argv[i]`.
   Matching is case-sensitive and covers the entire string. If no descriptor
   matches, return `ARGCV_UNKNOWN_OPTION`.
2. If the matching descriptor `j` defines a flag, set
   `results[j].present = true` and advance to `i + 1`.
3. Otherwise, apply the value rules in Section 3.3 to `argv[i + 1]`. If they
   succeed, set `results[j].value = argv[i + 1]`, replacing any previous value,
   and advance to `i + 2`.

Every occurrence is parsed, including repetitions. For example,
`--host a --host` fails at the second `--host` because that occurrence has no
value; the earlier valid occurrence does not make the call successful.

At an option position, ordinary strings, empty strings, and unsupported option
syntax all produce `ARGCV_UNKNOWN_OPTION`. Matching requires only a comparison
of the `--` prefix and the complete name; no concatenated string is needed.

### 3.3 Read a value

When an option requires a value, consider only the immediately following
argument:

1. If there is no next argument, return `ARGCV_MISSING_VALUE`.
2. If the next argument exactly matches any registered option, including its
   `--` prefix, return `ARGCV_MISSING_VALUE`. This check includes every
   descriptor: flags, options requiring values, options already seen, and the
   option currently waiting for a value.
3. Consume any other string as the value. Do not parse it again as an option or
   validate its contents.

For a descriptor set containing a `verbose` flag and a `host` option that
requires a value:

| Argument sequence | Outcome |
| --- | --- |
| `--host localhost` | `host` points to `"localhost"` |
| `--host --unknown` | `host` points to `"--unknown"` |
| `--host --verbose` | `ARGCV_MISSING_VALUE` |
| `--host --` | `host` points to `"--"` |

Thus, a string beginning with `--` can be a value if it does not exactly match a
registered option. `--` has no special role as an end-of-options marker, and `=`
does not separate a name from a value. The argument's position determines how
it is interpreted.

The parser receives strings that have already been separated into `argv`
elements. Quoting and escaping are handled by the shell or caller. Quoting a
registered option name does not make it eligible as a value: the shell command
`--host "--verbose"` still produces a missing-value error.

### 3.4 Implementation cost

Linear scans are sufficient for this design. Duplicate detection compares each
descriptor with those before it. Option lookup and the check for an option in a
value position scan the descriptor array. No sorting, hash table, or temporary
array is required.

With `R` descriptors, `A` command-line elements, and `L` as the maximum length of
a string involved in matching, worst-case time is `O((R² + A·R)·L)`. Auxiliary
space is `O(1)`, excluding the caller's `O(R)` result array.

## 4. Errors and precedence

The return value always reports the parse status. Passing `error == NULL`
suppresses location details without changing parsing behavior or the returned
status. Otherwise, every return writes both error fields according to the table
below. A dash means the field is not applicable: `SIZE_MAX` for `option_index`
and `-1` for `argv_index`.

| Status | Condition | `option_index` | `argv_index` |
| --- | --- | --- | --- |
| `ARGCV_OK` | Parsing completed successfully | — | — |
| `ARGCV_INVALID_NAME` | A descriptor name is invalid | Current descriptor | — |
| `ARGCV_DUPLICATE_NAME` | A descriptor repeats an earlier name | Later descriptor | — |
| `ARGCV_UNKNOWN_OPTION` | An argument at an option position has no exact descriptor match | — | Current argument |
| `ARGCV_MISSING_VALUE` | An option requiring a value has no next argument, or the next argument matches a registered option | Descriptor of the option waiting for a value | Argument containing the option waiting for a value |

For a missing value, `argv_index` always identifies the option that needs the
value. The application can distinguish the two causes by inspecting
`argv[argv_index + 1]`: `NULL` means the option was the last argument; a nonnull
pointer means the next argument matched a registered option. This access never
goes beyond `argv[argc]`.

Error precedence follows the scan order:

1. Descriptor errors take precedence over every argument error. Descriptors are
   checked in array order, with name validation before duplicate detection for
   each entry.
2. Arguments are processed from left to right. The first error ends the call;
   a later occurrence cannot repair it.
3. While waiting for a value, apply the value rules before treating the next
   argument as an option. For example, `--verbose --host --verbose` reports a
   missing value for `host` at argument index 2.

The application chooses diagnostic wording and process exit codes.

## 5. Required test cases

### 5.1 Notation and assertions

The default descriptor array, `S`, is:

```c
{
    {"verbose", false},  /* index 0 */
    {"host", true},      /* index 1 */
    {"port", true},      /* index 2 */
}
```

- `A` lists the elements of `argv[1..argc-1]`. Unless stated otherwise,
  `argv[0] == "app"`, `argc` is one plus the number of elements in `A`, and the
  array ends with `argv[argc] == NULL`. Quotes in the tables delimit strings.
- `F(name)` denotes `{name, false}`; `V(name)` denotes `{name, true}`.
- Unless stated otherwise, pass a valid error output object. It need not be
  initialized before the call.
- `OK` requires a return value of `ARGCV_OK` and, when provided, an error output
  of `{SIZE_MAX, -1}`. Results not explicitly listed must indicate absence:
  `present == false` for flags and `value == NULL` for options requiring values.
- `verbose` means its `present == true`. `host=@k` and `port=@k` require the
  corresponding `value == argv[k]`; assert pointer equality, not just equal
  string contents. Inspect only the active member for each option.
- `E(status, option index, argument index)` specifies the return status without
  its `ARGCV_` prefix and the two error output fields. A dash denotes the
  sentinel from Section 4. Do not assert anything about results after failure.
- Each case ID represents a separate test. In a row covering several IDs,
  assign the listed inputs to the IDs in order and call the parser separately
  for each. Cases involving repeated calls follow the sequence in the table.

### 5.2 Descriptor validation

| ID | Descriptors | `A` | Expected outcome |
| --- | --- | --- | --- |
| R01 | `option_count=0`; descriptor and result pointers refer to valid placeholder objects | `[]` | `OK`; neither placeholder is accessed |
| R02 | `S` | `[]` | `OK`; all options absent |
| R03 | `[F("a"), V("a1"), F("dry-run")]` | `["--dry-run", "--a1", "x", "--a"]` | `OK`; results in order: `present=true`, `value=argv[3]`, `present=true` |
| R04–R12 | `[F(name)]`; `name` is respectively `"-host"`, `""`, `"--host"`, `"Host"`, `"1host"`, `"dry_run"`, `"host=x"`, `"host name"`, `"主机"` | `[]` | `E(INVALID_NAME,0,—)` |
| R13 | `[F("verbose"), F("verbose")]` | `[]` | `E(DUPLICATE_NAME,1,—)` |
| R14 | `[F("a"), F("b"), F("c"), V("b")]` | `[]` | `E(DUPLICATE_NAME,3,—)` |
| R15 | `[F("a1"), F("a"), F("z09-"), F("a--b")]` | `[]` | `OK`; all options absent |
| R16–R17 | `[F("ok"), V(name)]`; `name` is respectively `"aZ"`, `"a中"` | `[]` | `E(INVALID_NAME,1,—)` |

### 5.3 Successful parsing and value boundaries

All cases in this table use `S`.

| ID | Input `A` | Expected outcome |
| --- | --- | --- |
| P01 | `["--verbose"]` | `OK`; `verbose` |
| P02 | `["--host", "localhost"]` | `OK`; `host=@2` |
| P03 | `["--verbose", "--host", "localhost", "--port", "8080"]` | `OK`; `verbose`, `host=@3`, `port=@5` |
| P04 | `["--port", "8080", "--host", "localhost", "--verbose"]` | `OK`; `verbose`, `host=@4`, `port=@2` |
| P05 | `["--port", "invalid-port"]` | `OK`; `port=@2`; no numeric validation |
| P06 | `["--host", ""]` | `OK`; `host=@2`; `value != NULL` points to an empty string |
| P07 | `["--host", " a b "]` | `OK`; `host=@2`; all whitespace preserved |
| P08 | `["--port", "-1"]` | `OK`; `port=@2` |
| P09 | `["--host", "-"]` | `OK`; `host=@2` |
| P10 | `["--host", "--", "--verbose"]` | `OK`; `host=@2`, `verbose`; `--` does not stop parsing |
| P11–P17 | `["--host", value]`; `value` is respectively `"--host=x"`, `"-v"`, `"--Host"`, `"---host"`, `"--ver"`, `"verbose"`, `"主机"` | `OK`; `host=@2` |
| P18 | `["--host", "--unknown", "--verbose"]` | `OK`; `host=@2`, `verbose` |
| P19–P21 | `["--host", value]`; `value` is respectively `"--verbose"`, `"--port"`, `"--host"` | `E(MISSING_VALUE,1,1)` |
| P22 | `["--host"]` | `E(MISSING_VALUE,1,1)` |
| P23 | `["--host", "a", "--port", "--host"]` | `E(MISSING_VALUE,2,3)`; an option already seen still cannot be consumed as a value |
| P24 | `["--verbose", "--verbose"]` | `OK`; `verbose` |
| P25 | `["--host", "a", "--host", "b"]` | `OK`; `host=@4` |
| P26 | `["--host", "a", "--host", "a"]`; the two `"a"` strings have distinct storage | `OK`; `host=@4`; the result pointer differs from `argv[2]` |
| P27 | `["--host", "", "--host", "b"]` | `OK`; `host=@4` |
| P28 | `["--host", "a", "--host", ""]` | `OK`; `host=@4`; `value != NULL` points to an empty string |
| P29 | `["--host", "a", "--host", "--unknown"]` | `OK`; `host=@4`; the value is `"--unknown"` |
| P30 | `["--host", "a", "--verbose", "--host", "b", "--verbose", "--host", "c"]` | `OK`; `verbose`, `host=@8` |

### 5.4 Argument errors

Use `S` unless stated otherwise.

| ID | Input `A` | Expected outcome |
| --- | --- | --- |
| E01 | `["--unknown", "value"]` | `E(UNKNOWN_OPTION,—,1)` |
| E02 | `["--verbose", "--host", "localhost", "--unknown", "value"]` | `E(UNKNOWN_OPTION,—,4)` |
| E03 | `["file.txt"]` | `E(UNKNOWN_OPTION,—,1)` |
| E04 | `["--verbose", "true"]` | `E(UNKNOWN_OPTION,—,2)` |
| E05–E11 | `[value]`; `value` is respectively `"-v"`, `"--"`, `""`, `"--host=x"`, `"--ver"`, `"--Host"`, `"---host"` | `E(UNKNOWN_OPTION,—,1)` |
| E12 | Zero descriptors, as in R01; `A=["--verbose"]` | `E(UNKNOWN_OPTION,—,1)` |
| E13 | `["--host", "--unknown", "value"]` | `E(UNKNOWN_OPTION,—,3)`; `--unknown` was consumed as a value |
| E14 | `["--host", "a", "--host"]` | `E(MISSING_VALUE,1,3)` |
| E15 | `["--host", "a", "--host", "--verbose"]` | `E(MISSING_VALUE,1,3)` |

### 5.5 Error precedence

| ID | Descriptors | Input `A` | Expected outcome |
| --- | --- | --- | --- |
| O01 | `[V("host"), V("host")]` | `["--unknown"]` | `E(DUPLICATE_NAME,1,—)` |
| O02 | `[F("")]` | `["--unknown"]` | `E(INVALID_NAME,0,—)` |
| O03 | `[V("host"), V("host"), F("")]` | `[]` | `E(DUPLICATE_NAME,1,—)` |
| O04 | `[V("host"), F(""), V("host")]` | `[]` | `E(INVALID_NAME,1,—)` |
| O05 | `S` | `["--unknown", "--host"]` | `E(UNKNOWN_OPTION,—,1)` |
| O06 | `S` | `["--host", "--verbose", "--unknown"]` | `E(MISSING_VALUE,1,1)` |
| O07 | `S` | `["--verbose", "--host", "--verbose"]` | `E(MISSING_VALUE,1,2)` |
| O08 | `S` | `["--host", "--verbose", "--host", "a"]` | `E(MISSING_VALUE,1,1)`; the first missing value ends the call |

### 5.6 Storage and repeated calls

| ID | Setup and input | Expected outcome |
| --- | --- | --- |
| L01 | `S`; preset `results[0].present=true` and point `results[1..2].value` to valid old strings; `A=[]` | `OK`; `results[0].present=false`, `results[1..2].value=NULL` |
| L02 | `S`; use the same result array to parse `["--host","a"]`, then `["--verbose"]` | First: `OK`, `host=@2`. Second: `OK`, `verbose`; `host` and `port` both have `value == NULL` |
| L03 | `S`; reuse the result array and error object to parse `["--verbose","--unknown"]`, then `["--host","b"]` | First: `E(UNKNOWN_OPTION,—,2)`. Second: `OK`, only `host=@2`; no previous flag state remains, and the error object is reset to `{SIZE_MAX, -1}` |
| L04 | `S`; `A=["--host","localhost"]`; provide four result entries, with the fourth entry's `value` pointing to a valid sentinel string | `OK`; `host=@2`; the fourth entry's `value` is unchanged |
| L05 | `S`; `argv={"--verbose",NULL}`, `argc=1` | `OK`; all options absent; `argv[0]` is skipped |
| L06 | `S`; `argc=0`, with `argv` pointing to an array containing `{NULL}` | `OK`; all options absent |
| L07 | `S`; use `char host_value[]="abc"` to construct `A=["--host",host_value]`; after success, change its first character to `x` | `OK`; the result pointer equals `host_value` and reads `"xbc"` after the change |
| L08 | `S`; parse `["--host","a"]` and `["--verbose","--unknown"]` separately, recording all input before each call | Returns `OK` and `E(UNKNOWN_OPTION,—,2)`, respectively; both calls leave descriptors, `argv` pointers, and string contents unchanged |
| L09 | `S`; `argv={"",NULL}`, `argc=1` | `OK`; all options absent; an empty program name does not affect parsing |
| L10 | Reuse one result object for four calls: `[V("host")]` with `["--host","a"]`; `[F("verbose")]` with `[]`; `[F("verbose")]` with `["--verbose"]`; `[V("host")]` with `[]` | All four return `OK`; active members in order: `value` equals the first call's `argv[2]`, `present=false`, `present=true`, `value=NULL` |
| L11 | Two concurrent calls share `S` and use separate `argv` arrays, result arrays, and error objects; `A` is respectively `["--host","a"]` and `["--verbose","--port","8080"]` | Both return `OK` and set their error objects to `{SIZE_MAX, -1}`. The first has only `host=@2`; the second has only `verbose`, `port=@3`. Each value pointer equals the corresponding element in its own `argv`, and the results are independent |

### 5.7 Optional error output

| ID | Setup and input | Expected outcome |
| --- | --- | --- |
| D01 | `S`; `A=["--verbose","--host","a","--host","b","--verbose"]`; `error=NULL` | Returns `ARGCV_OK`; `verbose`, `host=@5` |
| D02 | `S`; `A=["file.txt"]`; `error=NULL` | Returns `ARGCV_UNKNOWN_OPTION` |
| D03 | `S`; `A=["--host","--verbose"]`; `error=NULL` | Returns `ARGCV_MISSING_VALUE` |
| D04 | `[F("")]`; `A=[]`; `error=NULL` | Returns `ARGCV_INVALID_NAME` |
| D05 | `S`; initialize the error object to `{0,0}` before the first call, then reuse it to parse `["--unknown"]`, `["--host"]`, and `[]` | Outcomes in order: `E(UNKNOWN_OPTION,—,1)`, `E(MISSING_VALUE,1,1)`, `OK`; the final error object is `{SIZE_MAX,-1}` |
| D06 | `[F("a"), F("a")]`; `A=[]`; `error=NULL` | Returns `ARGCV_DUPLICATE_NAME` |
| D07 | `S`; `A=["--host"]`; `error=NULL` | Returns `ARGCV_MISSING_VALUE` |

## 6. Acceptance criteria

- The implementation satisfies the API, storage contract, parsing rules, and
  error location requirements in Sections 2–4.
- Test inputs satisfy the preconditions in Section 2.2, and every case in
  Section 5 passes. Tests of string results assert pointer equality.
- The implementation performs no dynamic allocation, modifies no input, and
  retains no global parsing state.
