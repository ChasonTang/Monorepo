## 1. Interface design

### 1.1 C API

Add the following declarations to [odin/cli.h](../cli.h), retaining the existing
C/C++ linkage and standard type headers. Place public declarations within
`assume_nonnull`, mark nullable fields explicitly, and annotate both pointer
levels of `argv`. Apply the same `argv` annotations to the v1 declarations;
output objects and stream parameters remain nonnull.

```c
#pragma clang assume_nonnull begin

/* Keep these defaults in the public header. */
#define ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT 8080
#define ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER 4433

typedef enum odin_cli_host_type_t {
  ODIN_CLI_HOST_NONE = 0,
  ODIN_CLI_HOST_NAME = 1,
  ODIN_CLI_HOST_IPV4 = 2,
  ODIN_CLI_HOST_IPV6 = 3,
} odin_cli_host_type_t;

typedef struct odin_cli_args_v2_t {
  const char *_Nullable server_host;
  size_t server_host_len;
  const char *_Nullable quic_cert_file;
  const char *_Nullable quic_key_file;
  const char *_Nullable quic_ca_file;
  uint16_t listen_port;
  uint16_t server_port;
  odin_cli_host_type_t server_host_type;
} odin_cli_args_v2_t;

odin_cli_status_t odin_cli_parse_v2(
    int argc,
    char *_Nullable const *_Nonnull argv,
    odin_cli_args_v2_t *out);

#pragma clang assume_nonnull end
```

Append `ODIN_CLI_ERR_MISSING_VALUE = 10` and
`ODIN_CLI_ERR_INVALID_VALUE = 11` to `odin_cli_status_t`. Preserve values
`0..9`, including if v1 is removed later.

The following table lists the statuses v2 can return. Throughout this document,
status names omit `ODIN_CLI_` and, for errors, `ERR_`; host types omit
`ODIN_CLI_HOST_`.

| Status | Value | Meaning |
|---|---|---|
| `OK_CLIENT` / `OK_SERVER` | 0 / 1 | Parsing succeeded and produced a complete configuration. |
| `HELP_CLIENT` / `HELP_SERVER` | 2 / 3 | Help was requested for the selected mode; no configuration is produced. |
| `UNKNOWN_MODE` | 4 | The program name does not identify a supported mode. |
| `MISSING_REQUIRED` | 5 | A required option was not supplied. |
| `UNKNOWN_FLAG` | 6 | An argument in option position is unknown, unsupported in the selected mode, or uses unsupported syntax. |
| `MISSING_VALUE` | 10 | An option was supplied without its required value. |
| `INVALID_VALUE` | 11 | A final value is empty or fails Odin's validation rules. |

Values `7..9` (`BAD_LISTEN_PORT`, `BAD_SERVER`, and `BAD_QUIC_TLS`) remain
exclusive to v1. The v2 API exposes no error location, diagnostic structure, or
port-presence flags. Both ports are final values in host byte order.

Use natural structure alignment, without `packed`. On an ABI where pointers and
`size_t` each have size and alignment 8, `uint16_t` has size and alignment 2,
and the host enum has size and alignment 4, the fields have offsets
`0, 8, 16, 24, 32, 40, 42, 44`; the structure has size 48 and alignment 8.
These numbers apply only to that ABI. The structure is not a serialization
format.

### 1.2 Command-line options

| Option | Client mode | Server mode | Takes a value when present |
|---|---|---|---|
| `--listen` | Optional | Optional | Yes |
| `--server` | Required | Unsupported | Yes |
| `--ca-file` | Required | Unsupported | Yes |
| `--quic-cert` | Unsupported | Required | Yes |
| `--quic-key` | Unsupported | Required | Yes |
| `--help` | Optional | Optional | No |

The argcv descriptor's `requires_value` field describes whether an occurrence
consumes a value. Odin separately decides whether the option must occur and
which default applies when it is absent.

### 1.3 Compatibility and dependency boundaries

Add v2 alongside v1. Retain v1's declarations, layout, status values and meanings,
implementation, call sites, and tests, subject to the header additions and
nullability annotations above. `odin_cli_main` continues to call v1; migration
and removal belong to later work.

The public interface uses Odin-owned types and exposes no argcv types. The
implementation uses argcv's public APIs for option parsing and value validation,
with responsibilities defined in section 2.2. The v2 tests must run independently
and derive their inputs and expectations from this RFC and the dependency
contracts, rather than from v1's implementation or test results.

## 2. Contracts

Contract identifiers distinguish caller preconditions (`P`), behavior delegated
in full to argcv (`D`), behavior owned by this RFC (`C`), and other guarantees
(`O`).

These contracts specify observable results. Implementations may choose their
check order, temporary storage, assignment strategy and count, and internal
state organization wherever those choices do not affect a promised result.

### 2.1 Caller preconditions

| ID | Requirement | Responsible party |
|---|---|---|
| P01 | `argc >= 0`. `argv` is nonnull and has at least `argc + 1` accessible elements. `argv[argc] == NULL`; each preceding element points to a valid NUL-terminated string, which may be empty. | Execution environment, direct callers, and test fixtures |
| P02 | `out` is valid, writable, and disjoint from input storage. It needs no prior initialization. Input storage remains valid and is not modified concurrently during the call; callers prevent conflicting access to the output. | Caller |
| P03 | Input strings remain valid while a configuration that borrows them is in use, including an older configuration retained after a non-OK result. Consumers read the host by pointer and length, without assuming NUL termination. After changing host bytes, they must parse again before relying on the validated host type. | Configuration consumers |

Violating a precondition is outside the API's supported error handling. Tests
therefore do not supply negative `argc`, null argument slots before the
sentinel, a missing sentinel, or invalid storage. The empty argument vector
`argc = 0, argv = {NULL}` is valid and returns `UNKNOWN_MODE` under C01.
An initially uninitialized `out` becomes readable only after success.

### 2.2 Behavioral contracts

#### 2.2.1 Behavior owned by argcv

The implementation and full test matrix for D01-D06 belong to the cited argcv
RFCs. The current public contracts in [argcv.h](../../argcv/argcv.h) and
[value.h](../../argcv/value.h) are authoritative. Odin remains responsible for
integrating these APIs, mapping their results, and assembling configurations
under the C contracts. Changes to a dependency contract or the delegation
boundary must be reflected in both projects' tests.

| ID | Delegated behavior | Owning RFC and API |
|---|---|---|
| D01 | Match option names exactly and case-sensitively. Accept only `--flag` and `--option value`; reject short options, abbreviations, equals syntax, positional arguments, and a standalone `--` in option position. A value is missing if there is no next argument or the next argument exactly matches a registered option. Every other string, including an empty string or an unregistered `--name`, is a value. Repeated options retain the last value and its original pointer. Return the first syntax error in argument order. | [RFC-001](../../argcv/docs/rfc_001_argument_parser_design.md), sections 2.3 and 3-4: `argcv_parse` |
| D02 | Convert ASCII `[0-9]+` to `0..65535`. Allow any number of leading zeros; reject nondigits and overflow. | [RFC-002](../../argcv/docs/rfc_002_parse_u16_design.md), section 3: `argcv_parse_u16` |
| D03 | Split a nonempty host and an optional nonempty port. A bracketed host ends at the first `]`, followed by either nothing or `:PORT`. Without brackets, exactly one colon separates host and port; zero or multiple colons leave the entire input as the host. Bracketed hosts and unbracketed hosts with multiple colons set `host_requires_ipv6`. Return borrowed slices and lengths, with NULL/0 for an absent port. Do not validate host contents or the numeric port value. | [RFC-003](../../argcv/docs/rfc_003_split_hostport_design.md), sections 2.2-2.3 and 3: `argcv_split_hostport` |
| D04 | Validate IPv6 syntax, including compression and a final embedded IPv4 address. Reject zone identifiers, network prefixes, and other invalid text. | [RFC-004](../../argcv/docs/rfc_004_validate_ipv6_design.md), section 3: `argcv_validate_ipv6` |
| D05 | Validate ASCII host labels of 1-63 letters, digits, or hyphens, with a letter or digit at each end. Allow one trailing root dot; excluding that dot, the total length is 1-253 bytes. On success, report `only_digits_and_dots` without deciding IPv4 validity. | [RFC-005](../../argcv/docs/rfc_005_validate_host_labels_design.md), sections 2.2 and 3: `argcv_validate_host_labels` |
| D06 | Validate strict IPv4 syntax: four ASCII decimal octets in `0..255`, with no leading zero unless the octet is exactly `0`. | [RFC-006](../../argcv/docs/rfc_006_validate_ipv4_design.md), section 2.2: `argcv_validate_ipv4` |

#### 2.2.2 Behavior owned by Odin

Only the explicitly named D clauses are delegated in the following table. Odin
owns every remaining requirement, with test coverage mapped in section 3.

| ID | Required behavior | Delegated portion |
|---|---|---|
| C01 | Return `UNKNOWN_MODE` when `argc == 0`. Otherwise, use the basename after the final `/` in `argv[0]`, recognizing only exact `odin-client` and `odin-server`. Empty or unrecognized names return `UNKNOWN_MODE` before help handling or option parsing. | None |
| C02 | Accept only the selected mode's options from section 1.2. An option exclusive to the other mode returns `UNKNOWN_FLAG` when encountered in option position. During semantic validation, a check that finds a required option absent returns `MISSING_REQUIRED`. | None |
| C03 | Apply the status mappings, precedence between phases, and first-failure rule for semantic validation defined below. | D01 selects the first syntax error. |
| C04 | Once syntax is valid, semantic validation depends only on each option's final value. An invalid earlier value does not affect the result; an invalid final value cannot fall back to an earlier valid one. | D01 selects the last value. |
| C05 | Default `listen_port` to 8080 for clients and 4433 for servers. Default the remote port to 4433 when `--server` has no port. Preserve every explicit value accepted by D02, including `0`, `65535`, and arbitrarily many leading zeros. An empty value, empty port suffix, or invalid number returns `INVALID_VALUE` rather than selecting a default. The listening and remote ports are independent. | D02 converts numbers; D03 distinguishes an absent port. |
| C06 | Classify the final server host using the rules below. Invalid structure or failure of the required host grammar returns `INVALID_VALUE`. Preserve the original host slice without normalization or additional character or length restrictions. | D03-D06 split and validate the input. |
| C07 | For each of the three file options, reject an empty final value with `INVALID_VALUE` and accept every nonempty final value verbatim. Path format, length, and file availability do not affect acceptance. Do not expand `~`, environment variables, or wildcards, or normalize the path. | D01 selects the last value. |
| C08 | On `OK_CLIENT` or `OK_SERVER`, produce the complete configuration specified below, using the values defined by C05-C07. Preserve the required original pointers and host length; set unused fields to 0 or NULL. Field values are guaranteed; padding bytes are unspecified. | None |

#### 2.2.3 Result precedence and error mapping (C03)

The parser applies the following precedence, from highest to lowest:

```text
UNKNOWN_MODE > first syntax error > HELP_* > first semantic error > OK_*
```

Map `ARGCV_UNKNOWN_OPTION` to `UNKNOWN_FLAG` and
`ARGCV_MISSING_VALUE` to `MISSING_VALUE`. These syntax errors have no separate
ranking: whichever argcv encounters first determines the result.

Help takes effect only after mode recognition and successful syntax parsing.
At that point, a help request returns the selected mode's `HELP_*` status even
if required options are absent or their values are invalid.

Without a help request, semantic validation checks required-option presence and
final values. A failed presence check returns `MISSING_REQUIRED`; a failed value
check returns `INVALID_VALUE`. Classifying a host as `NAME` under C06 is a
successful result, including when strict IPv4 validation does not match.

The implementation chooses the order of semantic checks and returns on the first
failure. That order need not follow `argv` and is not part of the contract:

- If a required option is absent and a supplied final value is invalid, either
  `MISSING_REQUIRED` or `INVALID_VALUE` is allowed.
- If all required options are present but multiple final values are invalid,
  the result is `INVALID_VALUE`.
- Only final values are validated, with the default for an absent `--listen`
  defined by C05.

The API does not identify the failing option or its position. Tests must allow
all results permitted by this check-order freedom.

#### 2.2.4 Host classification (C06)

First use D03 to determine the host slice and whether IPv6 is required. The
classification rules apply to that original slice:

| Form identified by D03 | Validation result for the original host slice | Outcome |
|---|---|---|
| IPv6 required | Passes D04 | `IPV6` |
| IPv6 required | Fails D04 | `INVALID_VALUE`; no fallback to another type |
| IPv6 not required | Fails D05 | `INVALID_VALUE` |
| IPv6 not required | Passes both D05 and D06 | `IPV4` |
| IPv6 not required | Passes D05 but fails D06 | `NAME` |

Keep spelling and any trailing root dot intact. The table defines results,
without prescribing validator call order or requiring use of
`only_digits_and_dots`.

For example, `::1:443` is a complete IPv6 host with the default port;
`[::1]:443` specifies port 443. The brackets in `[127.0.0.1]:80` require IPv6
validation, which fails. By contrast, `256.1.1.1`, `127.1`, `01.2.3.4`, and
`1.2.3.4.` satisfy the hostname grammar and are classified as `NAME`.

A host type describes the text's syntax at parse time. It makes no guarantee
about DNS resolution, reachability, or TLS identity, and does not specify how
the runtime interprets a numeric `NAME`.

#### 2.2.5 Successful output (C08)

| Field | `OK_CLIENT` | `OK_SERVER` |
|---|---|---|
| `listen_port` | Final listening port | Final listening port |
| `server_host` / `server_host_len` | Host slice within the final server value and its byte length. The pointer is offset 1 for bracketed input and 0 otherwise; the length excludes brackets, the port suffix, and the NUL terminator. | NULL / 0 |
| `server_host_type` | `NAME`, `IPV4`, or `IPV6` | `NONE` |
| `server_port` | Final remote port | 0 |
| `quic_ca_file` | Original pointer to the final `--ca-file` value, a nonempty NUL-terminated string | NULL |
| `quic_cert_file` / `quic_key_file` | NULL / NULL | Original pointers to the corresponding final option values, each a nonempty NUL-terminated string |

### 2.3 Other guarantees

The argcv RFCs cited in the D table own the dependency's portion of these
guarantees. Odin owns the remaining input handling and result assembly.

| ID | Guarantee and scope | Responsibility |
|---|---|---|
| O01 | Every return path leaves the `argv` pointer array and its referenced strings unchanged. | argcv preserves inputs during its own operations; Odin preserves them throughout the rest of the call. |
| O02 | On help or error, every field in `out` retains its entry value, including host, ports, paths, and type. An initially uninitialized output remains unreadable. | Odin preserves the complete output object. |
| O03 | Successful configurations borrow input storage without transferring ownership. The host may lack a NUL terminator; its lifetime and access rules follow P03, and its pointer and length follow C08. | D01/D03 provide borrowed values and slices; Odin preserves their ownership and borrowing semantics. |
| O04 | Parsing performs no dynamic allocation, file or network I/O, DNS resolution, runtime startup, or printing of help or diagnostics. Runtime operations belong to the caller. | argcv provides these guarantees for its own operations; all new Odin code must do the same. |
| O05 | Repeated calls and concurrent calls with independent output objects do not interfere. A result cannot depend on another parse call. | argcv provides call independence internally; Odin preserves it in the remaining parsing and assembly. |

The O contracts do not require a dedicated test or a shared assertion by
default. Add coverage where risk and value justify the cost; exhaustive input
snapshots, resource interception, or concurrency infrastructure are not required
solely to cover each row. Behavioral tests must still verify C08's fields,
pointer identity, and lengths.

## 3. Test specification

### 3.1 Fixtures and shared assertions

Tests must satisfy the P preconditions, execute `odin_cli_parse_v2`, and assert
the status and, on success, the configuration. Cover every Odin-owned clause in
the C contracts, including the portions of partially delegated contracts that
remain Odin's responsibility.

Use the real argcv implementation to test integration and composition. Do not
unit-test the dependency directly, inject invalid descriptor tables, or
reconstruct the full D test matrices. Internal ordering and call timing are
outside the test contract. Compilation, layout checks, static analysis, manual
review, and build registration do not count as behavioral tests.

Use these valid client and server argument vectors as baselines:

```text
C = ["odin-client", "--server", "example.com", "--ca-file", "ca.pem"]
S = ["odin-server", "--quic-cert", "cert.pem", "--quic-key", "key.pem"]
```

These lists represent actual `argv` elements without shell processing. Fixtures
own writable strings, compute `argc`, and append the NULL sentinel.
`C/S + [...]` appends arguments. Setting an option replaces its value or adds it
if absent; deleting an option removes both its name and value. Option names in
test descriptions may omit `--`. All unspecified arguments retain their
baseline values, and file paths need not exist.

Every successful parse runs Q01. Non-OK cases assert the specified status.

| ID | Contract | Shared assertion |
|---|---|---|
| Q01 | C08 | Check the complete configuration field by field, including host type and 0/NULL in every unused field. The host pointer must equal the final server value's address plus the applicable offset, and its length must match C08; compare host contents by length. Each path pointer must equal the address of its final argument value. |

### 3.2 Behavioral cases

| ID | Odin-owned clauses covered | Input or operation | Expected result |
|---|---|---|---|
| T01 | C01, C02; C05 defaults; C06 classification | Parse C and S. Repeat with program names `/usr/local/bin/odin-client` and `./bin/odin-server`, respectively. | Success in the corresponding mode. C: listen=8080, server_port=4433, host=`example.com`, length=11, type=NAME. S: listen=4433, type=NONE. |
| T02 | C01; C03 mode precedence | In C, set the program name to each of `""`, `/bin/`, `odin`, `ODIN-CLIENT`, and `odin-client.exe`. Test each with the baseline options and again with `["--help", "--bogus"]` appended. Also test `argc=0, argv={NULL}`. | `UNKNOWN_MODE` in every case. |
| T03 | C02 required options | Remove server and ca-file from C in separate cases; remove quic-cert and quic-key from S in separate cases. Also test each mode with only its program name. | `MISSING_REQUIRED` in every case. |
| T04 | C02 mode-specific options; C03 status mapping | Append quic-cert or quic-key to C, or server or ca-file to S, each with value `x`. | `UNKNOWN_FLAG` in every case. |
| T05 | C03 missing-value mapping | `C + ["--listen"]`. | `MISSING_VALUE`. |
| T06 | C02 empty values are present; C03 multiple invalid values; C05-C07 empty-value mapping | For each mode, set each value-taking option to `""` in turn, keeping the other required values valid. Then set all value-taking options to empty strings together. | `INVALID_VALUE` in every case; empty values neither count as absent options nor select defaults. |
| T07 | C03 help statuses | Pass only `--help` after each mode's program name. Also append `--help` to C and S. | The corresponding `HELP_CLIENT` or `HELP_SERVER`. |
| T08 | C03 help precedes semantic errors | `["odin-client", "--help", "--listen", "abc"]`; `["odin-server", "--quic-cert", "", "--help"]`; and `["odin-client", "--server", V, "--help"]` for V = `[::1`, `[not-ip]`, `bad_host`, or `example.com:65536`. | The corresponding HELP status despite invalid values and absent required options. |
| T09 | C03 syntax mapping and precedence | `["odin-client", "--help", "--listen"]` and `["odin-server", "--help", "--ca-file", "x"]`. Repeat each with the `--help` element replaced by the two elements `--listen`, `abc`. | Client cases return `MISSING_VALUE`; server cases return `UNKNOWN_FLAG`. Syntax errors take precedence over help and semantic errors. |
| T10 | C02, C03 first semantic failure; C05-C07 failure mapping | `["odin-client", "--listen", "abc"]`; `["odin-server", "--quic-cert", ""]`; and `["odin-client", "--server", V]` for V = `[not-ip]` or `bad_host`. | Either `INVALID_VALUE` or `MISSING_REQUIRED` is valid for each case. Do not impose a check order. |
| T11 | C04; C05-C07 final-value semantics | Remove the option under test, then supply it twice: listen=`bad → 9000` in both modes; server=`bad_host → [::1]:8443`; each of the three paths=`"" → final.pem`. Repeat each pair in reverse order. | Invalid first, valid last: success using the final value and its original pointer where applicable. Valid first, invalid last: `INVALID_VALUE`. |
| T12 | C05 independent ports | Set C's server to `example.com:8443` and listen to `9000`. Set S's listen to `9000`. | C has listening/remote ports 9000/8443; S has listen=9000. |
| T13 | C05 explicit ports | Use each of `0`, `65535`, and `000080` for listen in both modes, and for the client server values `example.com:PORT` and `[::1]:PORT`. | Success with the corresponding port 0, 65535, or 80 and correct borrowed slices. Odin adds no further port restriction. |
| T14 | C05 conversion-failure mapping | Use `65536` and `abc` at each port position from T13. | `INVALID_VALUE`. |
| T15 | C05 empty suffix; C06 structural-failure mapping | Set C's server to `example.com:`, `[::1]:`, or `[::1`. | `INVALID_VALUE`; an empty suffix does not select the default port. |
| T16 | C06 NAME classification and original text | Set C's server to `localhost`, `Example.COM.`, `1-2`, `123.example`, or `face:443`. | NAME with original host spelling. Use the default remote port except for `face:443`, which uses 443. |
| T17 | C06 IPV4 classification | Set C's server to `127.0.0.1` or `127.0.0.1:80`. | IPV4 with remote port 4433 or 80, respectively. |
| T18 | C06 IPV6 classification and original text | Set C's server to `::1`, `[::1]`, `::1:443`, `[::1]:8443`, or `[::ffff:192.0.2.1]:8443`. | IPV6 with original host spelling. The first three use port 4433; the last two use 8443. An embedded IPv4 tail does not change the outer type. |
| T19 | C03 NAME fallback is not an error; C06 fallback, text preservation, and port composition | Set C's server to `256.1.1.1`, `01.2.3.4:80`, or `1.2.3.4.`. Separately parse `["odin-client", "--server", "256.1.1.1"]`. | The first three succeed as NAME, preserving leading zeros or the trailing dot, with ports 4433, 80, and 4433. The last case returns `MISSING_REQUIRED`. |
| T20 | C06 host-failure mapping and restricted fallback | Set C's server to `[not-ip]`, `[127.0.0.1]:80`, `a:b:443`, or `bad_host`. | `INVALID_VALUE` in every case. Failure of required IPv6 or host-label validation cannot fall back to NAME. |
| T21 | C06 slice and length composition | Let `D = a×63 + "." + b×63 + "." + c×63 + "." + d×61`. Set C's server to `D + ".:80"`. | NAME, host length 254 including the root dot, remote port 80. The host length excludes the port, and Odin imposes no additional limit on the complete argument length. |
| T22 | C07 verbatim paths without additional restrictions | In separate cases, set C's ca-file to the literal `~/$HOME/*.pem`; S's quic-cert to `a//\u542b \u7a7a\u683c.pem` (the fixture ensures this path does not exist); and S's quic-key to 8192 copies of `a`. | All succeed and borrow the original strings. Respectively verify no expansion; no normalization or rejection based on path format or inability to load the file; and no length limit or truncation. |
| T23 | C08 complete replacement | Reuse one `out` for successful client parses with NAME, IPV4, and IPV6 hosts, then a successful server parse, then another successful client parse. | Run Q01 after every call, including checks that all unused fields are reset to 0/NULL. |

T19 and T21 cover composition. The full IPv4 grammar and host-label length
boundary matrices belong to D06 and D05, respectively.

T22 distributes special path cases across the three fields. Its Unicode escapes
denote the actual characters in the argument, not literal backslash sequences.
Empty values, repeated values, and final configuration fields are covered for
each path by T06, T11, and Q01.
