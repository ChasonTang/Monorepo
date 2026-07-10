# RFC-033: Server Client Certificate Authentication

## 1. Summary

Add optional QUIC client-certificate authentication to `odin-server` by letting odin configure the server BoringSSL `SSL_CTX` obtained from xquic's existing internal TLS context, without modifying xquic source files.

## 2. Goals

- **G1.** A server invocation can opt into client-certificate authentication with a non-empty CA-file path, while omitted client-auth configuration preserves the current unauthenticated-client behavior.
- **G2.** When client-certificate authentication is configured, the server requires each QUIC client to present a certificate chain that BoringSSL validates against the configured CA file before any Odin stream session can relay bytes.
- **G3.** Every client-auth setup failure reports the existing deterministic QUIC startup failure shape and releases server-runtime resources created before the failure.
- **G4.** The implementation leaves files under `xquic/` unchanged and uses only odin-owned code plus existing xquic internal headers/symbols to reach the server `SSL_CTX` (non-testable: this is an absence-of-diff constraint enforced by §6 review commands, not a runtime behavior that can be made red without deliberately making a forbidden xquic edit).

## 3. Design

### 3.1 Overview

The CA-file path starts as a server-mode CLI option, flows through the internal server runner config, and lands in `odin_xqc_server_runtime_create`. After `odin_xqc_udp_create` creates the xquic server engine, odin obtains the engine's TLS context through existing xquic internals, retrieves the BoringSSL `SSL_CTX`, loads a fresh client-auth trust store and client certificate authority list from the CA file, then installs the store, installs the CA list, and sets BoringSSL verify mode only after both loads succeed.

The xquic source tree remains unchanged. Odin owns the bridge code and its build include path for xquic internal headers. `build/secondary/xquic/BUILD.gn` compiles the odin-owned bridge source into the `//xquic` component, so the hidden xquic TLS accessor is called inside the xquic shared library in debug builds and odin links only to an exported odin-named wrapper. Production client behavior is unchanged; positive mTLS tests use a test-only odin client-runtime helper to install a fixture client certificate on the client engine `SSL_CTX` before `odin_xqc_client_runtime_start` calls `xqc_connect`.

```text
odin-server argv
    |
    v
odin_cli_parse --client-ca-file FILE
    |
    v
odin_cli_run_server config.quic_client_ca_file
    |
    v
odin_xqc_server_runtime_create config.client_ca_file
    |
    v
odin_xqc_udp_create -> xqc_engine_t -> engine->tls_ctx
    |
    v
odin_xquic_tls_ctx_get_ssl_ctx -> SSL_CTX
    |
    v
X509_STORE_new + X509_STORE_load_locations
    |
    v
SSL_load_client_CA_file
    |
    v
SSL_CTX_set_cert_store + SSL_CTX_set_client_CA_list
    |
    v
SSL_CTX_set_verify(SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL)
```

### 3.2 Detailed Design

#### 3.2.1 Server CLI and Runtime Config

Extend the existing server parser and runner surfaces:

```c
typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_client_transport_t client_transport;
  odin_cli_server_transport_t server_transport;
  const char *quic_cert_file;
  const char *quic_key_file;
  const char *quic_ca_file;
  const char *quic_client_ca_file;
} odin_cli_args_t;

typedef struct odin_cli_server_config_t {
  uint16_t listen_port;
  odin_cli_server_transport_t transport;
  const char *quic_cert_file;
  const char *quic_key_file;
  const char *quic_client_ca_file;
} odin_cli_server_config_t;

typedef struct odin_xqc_server_runtime_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  const char *client_ca_file;
} odin_xqc_server_runtime_config_t;
```

Server long options become:

```c
static const struct option kServerLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"quic-cert", required_argument, NULL, 1001},
    {"quic-key", required_argument, NULL, 1002},
    {"client-ca-file", required_argument, NULL, 1004},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};
```

Server usage becomes:

```text
usage: odin-server --listen ADDR --quic-cert FILE --quic-key FILE [--client-ca-file FILE]
```

**Unstated contract.** `quic_client_ca_file` and `client_ca_file` are optional. `NULL` means the server does not request or require client certificates. `--client-ca-file FILE` and `--client-ca-file=FILE` are server-mode only, alias the argv value without allocation or canonicalization, and are accepted only when `FILE[0] != '\0'`. `--client-ca-file ""` and `--client-ca-file=` return `ODIN_CLI_ERR_BAD_QUIC_TLS` after higher-precedence help, unknown-flag, and bad-listen checks. A terminal `--client-ca-file` with no following argument returns `ODIN_CLI_ERR_UNKNOWN_FLAG`, matching the existing missing-option-argument contract. Missing required server `--quic-cert` or `--quic-key` still returns the existing missing/invalid TLS result before any runner call. A direct internal `odin_cli_run_server` call that supplies `quic_client_ca_file == ""` reaches `odin_xqc_server_runtime_create`; the runner reports that failure with the existing `xqc_server_runtime_create` startup step. `odin-client --client-ca-file FILE` remains `ODIN_CLI_ERR_UNKNOWN_FLAG`, and server-mode `--ca-file` remains `ODIN_CLI_ERR_UNKNOWN_FLAG`.

**Mechanism.**

```text
parse(argc, argv, out):
  zero out, including quic_client_ca_file = NULL
  if mode == SERVER:
    parse --listen, --quic-cert, --quic-key, --client-ca-file, --help
  preserve existing precedence through BAD_LISTEN_PORT
  if server TLS cert/key missing or empty:
    return BAD_QUIC_TLS
  if --client-ca-file was present with an empty argument:
    return BAD_QUIC_TLS
  on Server OK:
    out.quic_client_ca_file = client_ca_arg or NULL

main(argc, argv, out, err):
  on Server OK:
    config.quic_client_ca_file = args.quic_client_ca_file
    return odin_cli_run_server(&config, err)

run_quic_server(config, err):
  rt_config.client_ca_file = config.quic_client_ca_file
  call odin_xqc_server_runtime_create(&rt_config, &runtime)
```

Satisfies: G1 via the optional server-mode CA-file contract and default `NULL` behavior; G3 via parser-side rejection of empty argv client-auth configuration and runner propagation of runtime-create failures.

#### 3.2.2 Server SSL_CTX Bridge

Odin adds an exported bridge symbol whose source and header live under `odin/`, while the bridge implementation is compiled into the existing `//xquic` component by `build/secondary/xquic/BUILD.gn`:

```c
/* odin/xquic_tls_ctx_bridge.h */
#include <openssl/ssl.h>
#include "src/tls/xqc_tls_defs.h"
#include "xquic/xquic_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

XQC_EXPORT_PUBLIC_API SSL_CTX *
odin_xquic_tls_ctx_get_ssl_ctx(xqc_tls_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
```

```c
/* odin/xquic_tls_ctx_bridge.c */
#include "odin/xquic_tls_ctx_bridge.h"
#include "src/tls/xqc_tls_ctx.h"

XQC_EXPORT_PUBLIC_API SSL_CTX *
odin_xquic_tls_ctx_get_ssl_ctx(xqc_tls_ctx_t *ctx) {
  return xqc_tls_ctx_get_ssl_ctx(ctx);
}
```

The bridge uses the existing `odin_xqc_udp_engine` accessor in `odin/xqc_udp.h`, the existing `xqc_engine_t.tls_ctx` field in `xquic/src/transport/xqc_engine.h`, and the existing `xqc_tls_ctx_get_ssl_ctx` accessor in `xquic/src/tls/xqc_tls_ctx.h`. Odin runtime targets add the xquic repository root as a private include path only for the translation units that read `xqc_engine_t.tls_ctx`. They do not link against `xqc_tls_ctx_get_ssl_ctx` directly. `build/secondary/xquic/BUILD.gn` appends `//odin/xquic_tls_ctx_bridge.c` to `component("xquic")`; in debug, `component()` is a shared library, so the wrapper and hidden `xqc_tls_ctx_get_ssl_ctx` are linked inside the same dylib, and `XQC_EXPORT_PUBLIC_API` exports only `odin_xquic_tls_ctx_get_ssl_ctx` for odin callers. In release, the same component is a source set, so the wrapper remains in the final link without a cross-dylib symbol boundary. No file under `xquic/` is edited.

Under `ODIN_XQC_SERVER_RUNTIME_TESTING`, `odin/testing/server_xqc_runtime_internal_test.h` declares:

```c
typedef enum odin_xqc_server_runtime_test_client_auth_null_t {
  ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_ENGINE = 1,
  ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_TLS_CTX = 2,
  ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_SSL_CTX = 3,
} odin_xqc_server_runtime_test_client_auth_null_t;

int odin_xqc_server_runtime_test_fail_next_x509_store_new(int errnum);
int odin_xqc_server_runtime_test_fail_next_ssl_load_client_ca_file(void);
int odin_xqc_server_runtime_test_preseed_next_existing_ssl_ctx_store(
    const char *ca_file);
int odin_xqc_server_runtime_test_force_next_client_auth_null(
    odin_xqc_server_runtime_test_client_auth_null_t which);
```

It configures BoringSSL through the existing `X509_STORE_new`, `X509_STORE_load_locations`, `X509_STORE_free`, `SSL_CTX_get_cert_store`, and `SSL_CTX_set_cert_store` APIs declared in `boringssl/include/openssl/x509.h` and `boringssl/include/openssl/ssl.h`, plus `SSL_load_client_CA_file`, `SSL_CTX_set_client_CA_list`, `SSL_CTX_get_verify_mode`, and `SSL_CTX_set_verify` declared in `boringssl/include/openssl/ssl.h`.

**Unstated contract.** The bridge runs after `odin_xqc_udp_create` succeeds and before `xqc_engine_register_alpn`, `odin_xqc_server_runtime_start`, or any packet processing can occur. `client_ca_file == NULL` performs no SSL_CTX mutation and increments the test-only null-config decision counter. `client_ca_file[0] == '\0'` returns `-1/EINVAL` before UDP creation. The bridge rejects null engine, null `engine->tls_ctx`, or null `SSL_CTX` as `-1/EINVAL` before any CA-store allocation; the test-only null selector above is consumed at the odin-owned wrappers around `odin_xqc_udp_engine`, the `engine->tls_ctx` read, and `odin_xquic_tls_ctx_get_ssl_ctx`, so the `xqc_tls_ctx_get_ssl_ctx(NULL)` dereference in xquic is never invoked. Client-auth trust is isolated from xquic defaults: odin does not call `SSL_CTX_load_verify_locations` for production client-auth setup because xquic has already called `SSL_CTX_set_default_verify_paths` on the server `SSL_CTX` and BoringSSL appends `SSL_CTX_load_verify_locations` input to the current store. Instead, odin allocates a fresh `X509_STORE`, loads only `client_ca_file` into that store with `X509_STORE_load_locations`, loads the CA-name stack with `SSL_load_client_CA_file`, and installs neither object on the `SSL_CTX` until both loads succeed. On success, `SSL_CTX_set_cert_store` takes ownership of the new store and frees the previous `ctx->cert_store`, then `SSL_CTX_set_client_CA_list` takes ownership of the name stack before verify mode is enabled. A test-only preseed helper may load a CA into the inherited `SSL_CTX_get_cert_store(ctx)` before the replacement step; that helper is not compiled outside `ODIN_XQC_SERVER_RUNTIME_TESTING` and exists only to prove the inherited store is not used for client-auth verification. `X509_STORE_new` failure returns `-1/ENOMEM` after destroying the just-created UDP/xquic engine and DNS resolver. An unreadable, missing, or non-PEM CA file returns `-1/EINVAL` after the same cleanup. The `SSL_load_client_CA_file` branch is reached through a private odin wrapper; the test helper above arms only the next wrapper call, forces it to return `NULL`, increments the forced-failure test counter, and lets `configure_client_auth` return `-1/EINVAL` after the fresh `X509_STORE_load_locations` call has succeeded and before the fresh store is installed. `SSL_CTX_set_verify` uses a `NULL` callback, so BoringSSL's default verifier owns chain validation; odin does not implement a custom certificate-acceptance callback with `SSL_CTX_set_verify`. The server runtime never stores or reopens the CA path after successful setup; the configured `SSL_CTX` owns the trust configuration for subsequent handshakes. The bridge depends on xquic's current internal `xqc_engine_t.tls_ctx` layout and `xqc_tls_ctx_get_ssl_ctx` symbol; this is reviewed as an odin-owned internal integration point, not as a public xquic API contract, and debug/ASan link gates fail if `odin_xquic_tls_ctx_get_ssl_ctx` is not exported from the xquic component.

**Mechanism.**

```text
server_runtime_create(config, out):
  validate existing config fields
  if config.client_ca_file != NULL and config.client_ca_file[0] == '\0':
    errno = EINVAL; return -1
  allocate runtime and DNS resolver
  create odin_xqc_udp / xqc_engine as today
  if config.client_ca_file != NULL:
    if configure_client_auth(rt, config.client_ca_file) != 0:
      destroy odin_xqc_udp
      destroy DNS resolver
      free runtime
      return -1 with configure_client_auth errno
  register ALPN and finish create as today

configure_client_auth(rt, ca_file):
  engine = runtime_client_auth_engine(rt->xu)
  if engine == NULL:
    errno = EINVAL; return -1
  tls_ctx = runtime_client_auth_tls_ctx(engine)
  if tls_ctx == NULL:
    errno = EINVAL; return -1
  ssl_ctx = runtime_client_auth_ssl_ctx(tls_ctx)
  # runtime_client_auth_ssl_ctx calls odin_xquic_tls_ctx_get_ssl_ctx.
  if ssl_ctx == NULL:
    errno = EINVAL; return -1
  if test-only preseed is armed:
    X509_STORE_load_locations(SSL_CTX_get_cert_store(ssl_ctx), preseed_ca, NULL)
  store = X509_STORE_new()
  if store == NULL:
    errno = ENOMEM; return -1
  if X509_STORE_load_locations(store, ca_file, NULL) != 1:
    X509_STORE_free(store)
    errno = EINVAL; return -1
  names = runtime_load_client_ca_file(ca_file)
  if names == NULL:
    X509_STORE_free(store)
    errno = EINVAL; return -1
  SSL_CTX_set_cert_store(ssl_ctx, store)
  SSL_CTX_set_client_CA_list(ssl_ctx, names)
  SSL_CTX_set_verify(ssl_ctx,
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL)
  return 0
```

Satisfies: G2 via BoringSSL's server-side peer-certificate request, mandatory-cert flag, and CA-file trust store; G3 via cleanup on every post-UDP setup failure; G4 via the odin-owned bridge over existing xquic internals.

#### 3.2.3 Test-Only Client Certificate Injection

The positive mTLS integration row needs a QUIC client that presents a certificate. This RFC does not add production client CLI flags or a public client certificate API. Under `ODIN_XQC_CLIENT_RUNTIME_TESTING`, `odin/testing/client_xqc_runtime_internal_test.h` exposes focused test helpers:

```c
typedef enum odin_xqc_client_runtime_test_client_cert_null_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_ENGINE = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_TLS_CTX = 2,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_SSL_CTX = 3,
} odin_xqc_client_runtime_test_client_cert_null_t;

int odin_xqc_client_runtime_test_force_next_client_cert_null(
    odin_xqc_client_runtime_test_client_cert_null_t which);

int odin_xqc_client_runtime_test_set_client_certificate(
    odin_xqc_client_runtime_t *rt,
    const char *cert_file,
    const char *key_file);
```

**Unstated contract.** The helper is valid only after `odin_xqc_client_runtime_create` or `odin_xqc_client_runtime_create_default` succeeds and before `odin_xqc_client_runtime_start` calls `xqc_connect`. It returns `-1/EINVAL` for null runtime, null/empty paths, missing client engine, missing client `engine->tls_ctx`, or missing client `SSL_CTX`; returns `-1/EINVAL` when BoringSSL cannot load the certificate chain, private key, or matching keypair; and does not start UDP or create a connection. The null selector is consumed once at odin-owned wrappers around `odin_xqc_udp_engine`, the `engine->tls_ctx` read, and `odin_xquic_tls_ctx_get_ssl_ctx`, so the helper never calls `xqc_tls_ctx_get_ssl_ctx(NULL)` or links to hidden `xqc_tls_ctx_get_ssl_ctx` across the debug xquic dylib boundary. The hook and helper are compiled only into `//odin/testing:odin_unittests` through the existing target-wide `:odin_xqc_client_runtime_testing_config`; alternate-platform test binaries compile the same helper but are not executed in this environment.

**Mechanism.**

```text
test_set_client_certificate(rt, cert_file, key_file):
  reject invalid arguments
  engine = client_cert_engine(rt->xu)
  if engine == NULL:
    errno = EINVAL; return -1
  tls_ctx = client_cert_tls_ctx(engine)
  if tls_ctx == NULL:
    errno = EINVAL; return -1
  ssl_ctx = client_cert_ssl_ctx(tls_ctx)
  # client_cert_ssl_ctx calls odin_xquic_tls_ctx_get_ssl_ctx.
  if ssl_ctx == NULL:
    errno = EINVAL; return -1
  if SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1:
    errno = EINVAL; return -1
  if SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) != 1:
    errno = EINVAL; return -1
  if SSL_CTX_check_private_key(ssl_ctx) != 1:
    errno = EINVAL; return -1
  record test counter
  return 0
```

Satisfies: G2 via the host-runnable positive mTLS row that proves a valid client certificate is accepted through the production server runtime.

## 4. Security

- **S1.**
  - **Threat:** A client that presents no certificate completes the QUIC handshake and opens an Odin stream when the operator intended client-certificate authentication.
  - **Mitigation:** §3.2.2 configures the server `SSL_CTX` with `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` before the runtime starts, so BoringSSL rejects clients that decline to send a certificate.
  - **Enforcement:** T8 starts an authenticated server and proves an existing odin client without a client certificate cannot reach HTTP 200 or relay bytes.

- **S2.**
  - **Threat:** A client presents a certificate chain signed by an unrelated CA, including a CA present in the inherited xquic/default verification store, or a chain whose leaf is valid only for TLS-server authentication, and the server accepts it as a client identity.
  - **Mitigation:** §3.2.2 allocates a fresh `X509_STORE`, loads only the configured CA file into it with `X509_STORE_load_locations`, loads the CA-name stack with `SSL_load_client_CA_file`, installs the store and name stack only after both loads succeed, and delegates peer-chain validation and TLS client-certificate purpose checks to BoringSSL's default verifier.
  - **Enforcement:** T9 separately drives a trusted wrong-purpose client certificate and an unrelated-CA clientAuth certificate; T11 pre-seeds the inherited store with that unrelated CA before configuration. Both rows assert rejection before any Odin stream relay.

- **S3.**
  - **Threat:** A bad CA-file path or malformed CA file leaves the server partially started, with UDP bound but client-auth policy not enforced.
  - **Mitigation:** §3.2.2 treats fresh-store allocation, CA-store load, and CA-name-list load failures as startup failures and destroys the UDP/xquic engine, DNS resolver, and runtime before returning.
  - **Enforcement:** T5 supplies empty, allocation-failure, missing, and malformed CA-file inputs for the fresh `X509_STORE` branch; T6 forces the later `SSL_load_client_CA_file` branch after the trust-store load succeeds. Both rows assert deterministic failure plus cleanup.

## 5. Testing Strategy

`<cert-dir>` means the GN-generated Thor fixture directory resolved the same way existing Odin tests resolve it: `Dirname(g_test_argv0) + "/gen/thor/odin_test_certs"`. These rows use only GN-declared fixture outputs; the Thor test leaves share `<cert-dir>/odin-test-leaf-key.pem`, including T9's untrusted clientAuth leaf.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 optional server CA-file config | T1, T3, T4, T10, T13 |
| G# | G2 authenticated handshakes and client-cert injection support | T4, T7, T8, T9, T10, T11, T14 |
| G# | G3 deterministic setup failure and cleanup | T2, T5, T6, T12 |
| State | Server parser omitted client CA | T1 |
| State | Server parser supplied client CA | T1, T10 |
| State | Server parser empty client CA | T1, T13 |
| State | Server parser missing client CA argument | T1 |
| State | Server parser precedence around empty client CA | T13 |
| State | Client-mode `--client-ca-file` | T1 |
| State | Runtime `client_ca_file == NULL` | T3 |
| State | Runtime valid CA configures SSL_CTX | T4, T10, T11 |
| State | Runtime invalid CA setup fails | T5, T6, T12 |
| State | Runtime inherited trust store contains unrelated CA before replacement | T11 |
| State | Authenticated server receives client without cert | T8 |
| State | Authenticated server receives valid client cert | T7 |
| State | Authenticated server receives trusted wrong-purpose client cert | T9 |
| State | Authenticated server receives clientAuth cert signed by unrelated CA | T9, T11 |
| Completion mode | Happy single-call parser/constructor | T1, T3, T4 |
| Completion mode | Parser rejection precedence | T1, T13 |
| Completion mode | Production CLI handoff stopped before serving | T10 |
| Completion mode | Happy event-loop handshake and relay | T3, T7 |
| Completion mode | Failed setup | T2, T5, T6, T12 |
| Completion mode | Failed handshake before relay | T8, T9, T11 |
| Decoder branch | Client-auth bridge handles are non-null | T4, T5, T6, T7, T8, T9, T10, T11 |
| Decoder branch | Client-auth bridge engine / TLS context / SSL_CTX missing | T12 |
| Decoder branch | Test client helper engine / TLS context / SSL_CTX missing | T14 |
| Decoder branch | `X509_STORE_new` succeeds | T4, T5, T6, T7, T8, T9, T10, T11 |
| Decoder branch | `X509_STORE_new` fails | T5 |
| Decoder branch | `X509_STORE_load_locations` succeeds on fresh store | T4, T6, T7, T8, T9, T10, T11 |
| Decoder branch | `X509_STORE_load_locations` fails on fresh store | T5 |
| Decoder branch | `SSL_CTX_set_cert_store` replaces inherited store | T4, T10, T11 |
| Decoder branch | `SSL_load_client_CA_file` succeeds | T4, T7, T8, T9, T10, T11 |
| Decoder branch | `SSL_load_client_CA_file` fails after trust-store load succeeds | T6 |
| Decoder branch | Client certificate/key PEM load succeeds | T7, T9, T11 |
| Decoder branch | Client certificate/key PEM load fails | T2 |
| Decoder branch | Client certificate helper success path reaches production handshakes | T7, T9, T11 |
| Benign-vs-fatal split | Omitted client CA leaves peer cert optional | T3 |
| Benign-vs-fatal split | Configured client CA requires peer cert | T7, T8, T9, T10, T11 |
| Constructor / factory precondition | Empty client CA path | T1, T2, T5 |
| Constructor / factory precondition | Test-only client cert helper invalid inputs | T2, T14 |
| Post-syscall sub-branch | `X509_STORE_new` or `X509_STORE_load_locations` fails after UDP create | T5 |
| Post-syscall sub-branch | `SSL_load_client_CA_file` fails after UDP create | T6 |
| Cleanup branch | Post-UDP CA setup failure destroys runtime resources | T5, T6, T12 |
| Callback-safe lifecycle hand-off | Existing on-close destroy behavior unchanged under authenticated handshakes | T7, T8, T9, T11 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Server parser accepts optional client CA and rejects malformed option forms | Subcases: valid omitted client CA; valid `--client-ca-file CA`; valid `--client-ca-file=CA`; empty separate `--client-ca-file ""`; empty equals `--client-ca-file=`; otherwise-valid server argv ending in terminal missing argument `--client-ca-file`; client-mode `odin-client --listen 8080 --server 127.0.0.1:4433 --ca-file CA --client-ca-file CA`; and server-mode `odin-server --ca-file CA --quic-cert C --quic-key K` | Omitted case returns `ODIN_CLI_OK` with `quic_client_ca_file == NULL`; supplied separate and equals forms return `ODIN_CLI_OK` and store the exact argv path pointer/slice; both empty-value forms return `ODIN_CLI_ERR_BAD_QUIC_TLS` with `quic_client_ca_file == NULL`; terminal missing argument, client-mode `--client-ca-file`, and server-mode `--ca-file` return `ODIN_CLI_ERR_UNKNOWN_FLAG` with `quic_client_ca_file == NULL`; `odin_cli_main` records no server runner call for the terminal missing-argument cell; updated server usage includes `[--client-ca-file FILE]` | G1 | unit |
| T2 | Server runner and test client helper reject bad client-auth config | Run `odin_cli_run_server` with QUIC cert/key and `quic_client_ca_file == ""` in a child process with stdout/stderr pipes, a dedicated pre-exit snapshot pipe, and 3 s parent deadlines for both snapshot read and `waitpid`; the child writes `OdinT2RunnerExitSnapshot` immediately after `odin_cli_run_server` returns and before `_exit`. Arm `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START` only as an anti-block failpoint if a skeleton ignores the empty CA path. Direct `odin_xqc_client_runtime_test_set_client_certificate` after client default create using null runtime, null cert path, empty key path, nonexistent cert path, and mismatched `<cert-dir>/odin-client-auth-only.pem` with `<cert-dir>/root-ca-key.pem` | The parent receives the snapshot before `waitpid`; it records return code `1`, exact stderr `odin: quic server startup failed at xqc_server_runtime_create\n`, anti-block failpoint not consumed, and zero live runtime, UDP, resolver, signal timer, and event-loop handles before process exit. If the anti-block failpoint is consumed and the error names `xqc_server_runtime_start`, or if no snapshot arrives by deadline, the row stays red. Each helper subcase returns `-1/EINVAL`, records no `xqc_connect`, and leaves the client runtime startable until row cleanup destroys it | G3 | unit |
| T3 | Omitted client CA preserves current QUIC server behavior | Install `signal(SIGPIPE, SIG_IGN)`. Start production server runtime through `odin_cli_main` or the existing runtime harness with `<cert-dir>/odin-server.pem`, key, and no `--client-ca-file`; the server-runtime test record captures `client_auth_null_config_decisions` and `client_auth_configure_calls`. Connect the existing odin client default with server verification CA `<cert-dir>/root-ca.pem` and no client certificate. All accept/read/write/wait operations use 2 s deadlines | Handshake succeeds, downstream receives exact HTTP 200 then `server-tail!`, upstream receives exact `client-tail!`, the server test record reports `client_auth_null_config_decisions == 1` and `client_auth_configure_calls == 0`, and cleanup reaches zero live sessions, registered CIDs, runtime objects, and listener fds | G1 | integration |
| T4 | Valid client CA configures server SSL_CTX before start | Direct `odin_xqc_server_runtime_create` with `client_ca_file == <cert-dir>/root-ca.pem`, valid server cert/key, and a test record that snapshots the server `SSL_CTX` immediately after create and before start | Create succeeds; the snapshot records the exact CA path, one client-auth configure call, one `X509_STORE_new`, one successful `X509_STORE_load_locations`, one successful `SSL_load_client_CA_file`, then one `SSL_CTX_set_cert_store`, and an installed store pointer different from the inherited `SSL_CTX_get_cert_store(ctx)` pointer observed before replacement. `SSL_CTX_get_verify_mode(ctx)` has both `SSL_VERIFY_PEER` and `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`, `SSL_CTX_get_client_CA_list(ctx)` is non-null with at least one name, ALPN registration still occurs once, and no xquic packet processing has occurred yet | G1, G2 | unit |
| T5 | Invalid client CA path or fresh-store allocation fails and cleans resources | Direct server runtime create with valid server cert/key and `client_ca_file` values `""`, nonexistent temp path, and temp file containing `not a certificate\n`; one additional subcase arms `odin_xqc_server_runtime_test_fail_next_x509_store_new(ENOMEM)` with `client_ca_file == <cert-dir>/root-ca.pem`. For every post-UDP failure, the test hook records whether UDP create happened, snapshots `odin_xqc_udp_local_addr` into `sockaddr_storage` immediately before UDP destroy, then probes a new UDP socket bind to that exact captured address/port after create returns | Empty path returns `-1/EINVAL` before UDP create and records no local-address snapshot. The forced allocation subcase returns `-1/ENOMEM` after exactly one UDP create and destroy, with resolver and runtime liveness returning to zero, no `X509_STORE_load_locations`, no `SSL_load_client_CA_file`, no `SSL_CTX_set_cert_store`, no ALPN registration, a captured nonzero local port, and a successful rebind probe. Nonexistent and malformed paths return `-1/EINVAL` from the fresh `X509_STORE_load_locations` branch after exactly one UDP create and destroy, with resolver and runtime liveness returning to zero, no `SSL_load_client_CA_file` call, no `SSL_CTX_set_cert_store`, no ALPN registration, no start call, captured local ports, and successful rebind probes | G3, S3 | unit |
| T6 | Valid CA path with forced client-CA-list load failure cleans resources | Direct server runtime create with valid server cert/key, `client_ca_file == <cert-dir>/root-ca.pem`, and `odin_xqc_server_runtime_test_fail_next_ssl_load_client_ca_file()` armed through `odin/testing/server_xqc_runtime_internal_test.h`; the `//odin/testing:odin_unittests` target-wide `:odin_xqc_server_runtime_testing_config` enables the hook. The failure-path test hook snapshots `odin_xqc_udp_local_addr` immediately before UDP destroy and probes a new UDP bind to that captured address/port after create returns | Create returns `-1/EINVAL` after the test record shows one successful fresh `X509_STORE_load_locations` call and one forced `SSL_load_client_CA_file` failure. The record shows the fresh store freed without `SSL_CTX_set_cert_store`, no `SSL_CTX_set_client_CA_list`, no `SSL_CTX_set_verify`, no ALPN registration, no start call, exactly one UDP create and destroy, resolver and runtime liveness returning to zero, a captured nonzero local port, and a successful rebind probe | G3, S3 | unit |
| T7 | Authenticated server accepts valid client certificate | Install `signal(SIGPIPE, SIG_IGN)`. Start production server runtime on `127.0.0.1:0` with `client_ca_file == <cert-dir>/root-ca.pem`. Create production client default with CA verification for the server, call `odin_xqc_client_runtime_test_set_client_certificate(client_rt, <cert-dir>/odin-client-auth-only.pem, <cert-dir>/odin-test-leaf-key.pem)` before start, then relay the same sentinel bytes as T3 through one CONNECT stream with 2 s deadlines | Client start and handshake succeed; the server accepts the client-auth-only certificate signed by `root-ca.pem`; downstream receives exact HTTP 200 then `server-tail!`; upstream receives exact `client-tail!`; cleanup reaches zero live sessions, registered CIDs, runtime objects, and listener fds | G2 | integration |
| T8 | Authenticated server rejects client without certificate | Same server setup as T7, but use the existing odin client default with no test client certificate injection. A local CONNECT fd is added only if the test state reports handshake done; all waits use 2 s deadlines | Handshake fails before HTTP 200 or relay bytes; no upstream bytes arrive by deadline; server session creation count remains zero; cleanup reaches zero live sessions, registered CIDs, runtime objects, and listener fds | G2, S1 | integration |
| T9 | Authenticated server rejects wrong-purpose and unrelated-CA client certificates | Same server setup as T7. Subcase A injects `<cert-dir>/odin-server.pem` and `<cert-dir>/odin-test-leaf-key.pem` as a trusted-by-root but serverAuth-only client certificate/key. Subcase B injects `<cert-dir>/untrusted-intermediate-client-auth-chain.pem` and `<cert-dir>/odin-test-leaf-key.pem`; Thor generates that leaf-first chain with a clientAuth leaf signed by `untrusted-intermediate-ca.pem`, which is not in the server's configured `<cert-dir>/root-ca.pem` trust store. Both clients still verify the server against `<cert-dir>/root-ca.pem`; all waits use 2 s deadlines | Both subcases fail before HTTP 200 or relay bytes; no upstream bytes arrive by deadline; server session creation count remains zero for each subcase; subcase A proves TLS client-purpose enforcement while the issuer is trusted, subcase B proves CA-store enforcement while the leaf purpose is clientAuth; cleanup reaches zero live sessions, registered CIDs, runtime objects, and listener fds | G2, S2 | integration |
| T10 | Production CLI forwards non-empty client CA to server runtime before serving | Call `odin_cli_main` in-process as `odin-server --listen 0 --quic-cert <cert-dir>/odin-server.pem --quic-key <cert-dir>/odin-test-leaf-key.pem --client-ca-file <cert-dir>/root-ca.pem` with `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START` armed. The same test binary has `ODIN_CLI_SERVER_TESTING` and `ODIN_XQC_SERVER_RUNTIME_TESTING` enabled target-wide | `odin_cli_main` returns `1` with `odin: quic server startup failed at xqc_server_runtime_start\n`, proving the existing failpoint stopped before serving. The server-runtime test record saw the exact CA path from argv, one client-auth configure call, one fresh store replacement, and verify mode containing both mandatory peer-certificate flags before the runner attempted `odin_xqc_server_runtime_start`. A bug that parses the flag but omits the `odin_cli_main` -> `odin_cli_run_server` -> runtime handoff leaves the record path null or verify mode unset and fails this row | G1, G2 | unit |
| T11 | Fresh server trust store ignores inherited default-store CA | Arm `odin_xqc_server_runtime_test_preseed_next_existing_ssl_ctx_store(<cert-dir>/untrusted-intermediate-ca.pem)`, then start the same authenticated production server runtime as T7 with `client_ca_file == <cert-dir>/root-ca.pem`. Inject `<cert-dir>/untrusted-intermediate-client-auth-chain.pem` and `<cert-dir>/odin-test-leaf-key.pem` into the client before start; all waits use 2 s deadlines | The test record shows the inherited `SSL_CTX_get_cert_store(ctx)` was preseeded before configuration, then `SSL_CTX_set_cert_store` installed a different fresh store loaded from `<cert-dir>/root-ca.pem`. The clientAuth chain signed by the preseeded unrelated CA still fails before HTTP 200 or relay bytes; server session creation count remains zero; cleanup reaches zero live sessions, registered CIDs, runtime objects, and listener fds | G2, S2 | integration |
| T12 | Missing xquic TLS bridge handles fail before ALPN and clean resources | Direct server runtime create with valid server cert/key, `client_ca_file == <cert-dir>/root-ca.pem`, and `odin_xqc_server_runtime_test_force_next_client_auth_null(which)` armed once for each enum value `ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_ENGINE`, `ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_TLS_CTX`, and `ODIN_XQC_SERVER_RUNTIME_TEST_CLIENT_AUTH_NULL_SSL_CTX`; the hook is declared in `odin/testing/server_xqc_runtime_internal_test.h`, consumed only at the §3.2.2 odin-owned bridge wrapper call sites, and enabled by `:odin_xqc_server_runtime_testing_config` target-wide for `//odin/testing:odin_unittests`. The failure-path test hook snapshots `odin_xqc_udp_local_addr` immediately before UDP destroy and probes a new UDP bind to that captured address/port after create returns | Each subcase returns `-1/EINVAL` after exactly one UDP create and exactly one UDP destroy, with no `X509_STORE_new`, no `X509_STORE_load_locations`, no `SSL_load_client_CA_file`, no `SSL_CTX_set_cert_store`, no `SSL_CTX_set_client_CA_list`, no `SSL_CTX_set_verify`, no ALPN registration, no start call, resolver and runtime liveness returning to zero, a captured nonzero local port, and a successful rebind probe | G3 | unit |
| T13 | Server parser preserves precedence around empty client CA | Subcases: `odin-server --help --client-ca-file ""`, `odin-server --bad-flag --client-ca-file "" --quic-cert C --quic-key K`, `odin-server --listen abc --client-ca-file "" --quic-cert C --quic-key K`, `odin-server --listen 4433 --quic-key K --client-ca-file ""`, and `odin-server --listen 4433 --quic-cert C --client-ca-file ""`. The `odin_cli_main` variants use the `ODIN_CLI_SERVER_TESTING` runner-call record to assert no server runner call for every non-OK result | Results are respectively `ODIN_CLI_HELP`, `ODIN_CLI_ERR_UNKNOWN_FLAG`, `ODIN_CLI_ERR_BAD_LISTEN_PORT`, `ODIN_CLI_ERR_BAD_QUIC_TLS`, and `ODIN_CLI_ERR_BAD_QUIC_TLS`; `quic_client_ca_file` remains `NULL` on every error result, and `odin_cli_main` never calls `odin_cli_run_server` for these parse failures | G1 | unit |
| T14 | Test client certificate helper rejects missing xquic TLS handles | After `odin_xqc_client_runtime_create_default` succeeds and before start, call `odin_xqc_client_runtime_test_force_next_client_cert_null(which)` once for each enum value `ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_ENGINE`, `ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_TLS_CTX`, and `ODIN_XQC_CLIENT_RUNTIME_TEST_CLIENT_CERT_NULL_SSL_CTX`, then call `odin_xqc_client_runtime_test_set_client_certificate` with valid `<cert-dir>/odin-client-auth-only.pem` and `<cert-dir>/odin-test-leaf-key.pem`; the hook is declared in `odin/testing/client_xqc_runtime_internal_test.h`, consumed only at the §3.2.3 odin-owned helper wrappers, and enabled by `:odin_xqc_client_runtime_testing_config` target-wide for `//odin/testing:odin_unittests` | Each subcase returns `-1/EINVAL` before `SSL_CTX_use_certificate_chain_file`, `SSL_CTX_use_PrivateKey_file`, `SSL_CTX_check_private_key`, or `xqc_connect` is called; the record captures the forced null kind exactly once, leaves the client runtime startable until row cleanup destroys it, and is paired with T7/T9/T11 where the same helper succeeds and reaches production handshakes | G2 | unit |
## 6. Implementation Plan

- **P1. Land contract surfaces and red-verifiable tests.**
  - **Scope:** add `quic_client_ca_file` / `client_ca_file` fields to `odin/cli.h`, `odin/cli_server.h`, and `odin/server_xqc_runtime.h`; add `odin/xquic_tls_ctx_bridge.h` and `odin/xquic_tls_ctx_bridge.c` with exported `odin_xquic_tls_ctx_get_ssl_ctx`, append the bridge source to `component("xquic")` from `build/secondary/xquic/BUILD.gn`, and add private xquic-internal include paths only to the odin runtime targets that read `xqc_engine_t.tls_ctx`; extend `thor/scripts/issue-ca-file-test-fixtures.py` and `//thor:odin_test_certs` outputs with `<cert-dir>/untrusted-intermediate-client-auth-chain.pem`, generated as a leaf-first chain from `untrusted-intermediate-ca.pem` with EKU `clientAuth` and `<cert-dir>/odin-test-leaf-key.pem`; add the T2 child pre-exit snapshot struct/test harness; add server-runtime test-record fields for client-auth CA path, null-config decision count, configure-call count, inherited and installed cert-store pointers, `X509_STORE_new` call/failure/free count, `X509_STORE_load_locations` call/result count, `SSL_CTX_set_cert_store` call count, `SSL_load_client_CA_file` call/forced-failure count, verify-mode value/set-call count, client-CA-list set-call/name count, inherited-store preseed call/result count, forced null bridge-handle kind, UDP-create-before-failure, UDP-destroy-after-failure, captured `odin_xqc_udp_local_addr` before destroy, rebind-probe result, and server-session creation count; add `odin_xqc_server_runtime_test_fail_next_x509_store_new(int errnum)`, `odin_xqc_server_runtime_test_fail_next_ssl_load_client_ca_file()`, `odin_xqc_server_runtime_test_preseed_next_existing_ssl_ctx_store(const char *ca_file)`, `odin_xqc_server_runtime_test_client_auth_null_t`, and `odin_xqc_server_runtime_test_force_next_client_auth_null(odin_xqc_server_runtime_test_client_auth_null_t which)` to `odin/testing/server_xqc_runtime_internal_test.h` under the existing `ODIN_XQC_SERVER_RUNTIME_TESTING` define supplied target-wide by `//odin/testing:odin_unittests`'s `:odin_xqc_server_runtime_testing_config`; add client-runtime test-record fields for client certificate helper calls, forced null helper kind, BoringSSL certificate-chain/private-key/check calls, and helper install-success count; add `odin_xqc_client_runtime_test_client_cert_null_t`, `odin_xqc_client_runtime_test_force_next_client_cert_null(odin_xqc_client_runtime_test_client_cert_null_t which)`, and the `odin_xqc_client_runtime_test_set_client_certificate` declaration plus a P1 stub returning `-1/EINVAL` to `odin/testing/client_xqc_runtime_internal_test.h` under the existing `ODIN_XQC_CLIENT_RUNTIME_TESTING` define supplied target-wide by `//odin/testing:odin_unittests`'s `:odin_xqc_client_runtime_testing_config`; add T1-T14 behind `ODIN_SERVER_CLIENT_CERT_RED=1` red-verification gates or `GTEST_SKIP("pending RFC-033 P2")` in the default run. Do not parse `--client-ca-file`, configure BoringSSL, allocate or replace the fresh client-auth store, force CA-list load failure, preseed inherited trust, force server or client null-handle selectors, or install a client test certificate in this phase.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/server_client_cert_mac --args='target_os="mac"'`, matching GN generation for `out/server_client_cert_mac_arm64`, `out/server_client_cert_linux_x64`, `out/server_client_cert_ios_sim`, and `out/server_client_cert_ios_device`, and `./tool/ninja -C out/server_client_cert_mac odin_main odin_unittests tests` succeed in the default debug component build. `./tool/gn desc out/server_client_cert_mac //xquic sources` lists `//odin/xquic_tls_ctx_bridge.c`, and `nm -gU out/server_client_cert_mac/libxquic.dylib | rg ' _?odin_xquic_tls_ctx_get_ssl_ctx$'` finds the exported wrapper while `nm -gU out/server_client_cert_mac/libxquic.dylib | rg ' _?xqc_tls_ctx_get_ssl_ctx$'` finds no exported hidden accessor. The fixture gate `./tool/gn desc out/server_client_cert_mac //thor:odin_test_certs outputs` lists `untrusted-intermediate-client-auth-chain.pem`; the generated PEM is leaf-first, its leaf matches `<cert-dir>/odin-test-leaf-key.pem`, contains a TLS Web Client Authentication EKU, includes the untrusted CA certificate after the leaf, and fails client-purpose verification against `<cert-dir>/root-ca.pem`. The host red-verification command `ODIN_SERVER_CLIENT_CERT_RED=1 out/server_client_cert_mac/odin_unittests --gtest_filter='*ServerClientCert*'` executes T1-T14 and fails them against the skeleton: T1 because `--client-ca-file` is still unknown, including separate, equals, equals-empty, and missing-argument cells, T2 because the bounded runner snapshot and helper bad-argument records are not produced, T3 because the NULL-client-auth decision counter remains zero while the handshake still succeeds, T4 because no SSL_CTX snapshot or client-auth mutation exists, T5 because no post-UDP fresh-store allocation/load path or captured-port rebind probe exists, T6 because no odin-owned `SSL_load_client_CA_file` wrapper/failpoint exists, T7 because the test client cannot present a certificate and the server does not require one, T8 because a no-cert client still handshakes, T9 because neither the trusted serverAuth certificate nor the clientAuth-but-untrusted chain is rejected by server client-auth policy, T10 because the production `odin_cli_main` server path does not parse and hand off `--client-ca-file`, T11 because the inherited store is not preseeded then replaced by a fresh configured-only store, T12 because no odin-owned server bridge null-handle hook or captured-port cleanup path exists, T13 because the bad-listen and missing-cert/key empty-client-CA precedence cells cannot pass while `--client-ca-file` is unknown, and T14 because no odin-owned client helper null-handle hook or BoringSSL-load call record exists. The default host run `out/server_client_cert_mac/odin_unittests --gtest_brief=1` reports T1-T14 skipped or gated and exits zero with pre-existing tests green. `git diff --name-only HEAD -- xquic` and `git status --short -- xquic` both print no paths. Host-runnable enumeration: T1-T14 run only in `out/server_client_cert_mac/odin_unittests` on the host macOS architecture. Cross-compile-only enumeration: `out/server_client_cert_mac_arm64/odin_unittests`, `out/server_client_cert_linux_x64/odin_unittests`, `out/server_client_cert_ios_sim/odin_unittests`, and `out/server_client_cert_ios_device/odin_unittests` are built but not executed in this RFC.

- **P2. Implement parser, SSL_CTX bridge, cleanup, and test-only client cert injection.**
  - **Scope:** implement §3.2.1 parsing, usage text, `odin_cli_main`, and `run_quic_server` handoff for `--client-ca-file`; implement §3.2.2 empty-path precheck, xquic-internal `SSL_CTX` bridge callers through exported `odin_xquic_tls_ctx_get_ssl_ctx`, null-config decision record, null bridge-handle checks, fresh `X509_STORE` allocation/load, odin-owned `SSL_load_client_CA_file` wrapper and test failpoint, install-after-both-loads store/list replacement, inherited-store preseed test hook, mandatory peer-certificate verify mode, post-UDP cleanup with local-address capture, and test snapshots; implement §3.2.3 client-helper null-handle wrappers plus test-only client certificate/key loading before `xqc_connect`, also through `odin_xquic_tls_ctx_get_ssl_ctx`; remove the red gates and pending skips from T1-T14.
  - **Depends on:** P1.
  - **Done when:** `./tool/ninja -C out/server_client_cert_mac odin_main odin_unittests tests` succeeds in the default debug component build; `out/server_client_cert_mac/odin_unittests --gtest_filter='*ServerClientCert*'` passes T1-T14 un-gated, including T1's separate, equals, equals-empty, and missing-argument parser cells, T3's NULL-client-CA decision counter, T5/T6/T12 captured-port rebind probes, T9's trusted-wrong-purpose and clientAuth-but-untrusted subcases, T10's production `odin_cli_main` handoff record, T11's inherited-store preseed rejection, T12's three server null-handle cleanup subcases, T13's parser precedence cells, and T14's three client-helper null-handle subcases; `out/server_client_cert_mac/odin_unittests --gtest_brief=1` passes the full local unit suite; `nm -gU out/server_client_cert_mac/libxquic.dylib | rg ' _?odin_xquic_tls_ctx_get_ssl_ctx$'` still finds the exported wrapper; and `git diff --name-only HEAD -- xquic` plus `git status --short -- xquic` both print no paths. The ASan gate `./tool/gn gen out/server_client_cert_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/server_client_cert_mac_asan odin_main odin_unittests`, `nm -gU out/server_client_cert_mac_asan/libxquic.dylib | rg ' _?odin_xquic_tls_ctx_get_ssl_ctx$'`, and `out/server_client_cert_mac_asan/odin_unittests --gtest_filter='*ServerClientCert*'` exit without AddressSanitizer reports. The cross-compile targets named in P1 build successfully again but are not executed; their parser, SSL_CTX bridge, BoringSSL include path, install-after-both-loads fresh store replacement, CA-list load failpoint, server and client null-handle hooks, inherited-store preseed hook, and test-only client certificate helper branches are compile-verified only.
