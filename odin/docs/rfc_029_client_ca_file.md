# RFC-029: Client CA File

## 1. Summary

Add `--ca-file FILE` to `odin-client` and use that PEM CA file to verify the QUIC server certificate chain and server identity when the client connects.

## 2. Goals

- **G1.** The client CLI parser accepts a non-empty `--ca-file FILE`, stores the selected path for Client mode, preserves the current omitted-option behavior, and rejects malformed CA-file option forms without changing Server-mode parsing.
- **G2.** A successful `odin-client --ca-file FILE` startup passes that exact path into the client QUIC runtime default configuration after the local listener is ready, while a startup that omits `--ca-file` keeps the current default runtime TLS verification configuration.
- **G3.** When a CA file is configured, the default client QUIC runtime accepts only server certificate chains that validate against that CA file and whose leaf certificate matches the configured server identity.

## 3. Design

### 3.1 Overview

The CA-file path starts as Client-mode CLI input, flows through the existing client runner config, and ends in `odin_xqc_client_runtime_create_default`. The default runtime loads the CA file during runtime creation, installs an Odin-owned xquic certificate verification callback, and enables xquic certificate verification before `xqc_connect`.

Omitting `--ca-file` leaves the current default path unchanged: the parser records no CA path, the client runner passes `NULL`, and the default runtime leaves `conn_ssl_config.cert_verify_flag == 0` with no default cert callback. Supplying `--ca-file` makes the CA store runtime-owned before any UDP driver or QUIC connection is started.

```text
odin-client argv
    |
    v
odin_cli_parse --ca-file FILE
    |
    v
odin_cli_run_client config.quic_ca_file
    |
    v
odin_xqc_client_runtime_create_default config.ca_file
    |
    v
load PEM CA store, install cert_verify_cb, set NEED_VERIFY
    |
    v
xqc_connect -> peer cert DER chain -> Odin CA-file verifier
```

### 3.2 Detailed Design

#### 3.2.1 Client CLI CA-File Contract

Extend the existing parser and client-runner surfaces in `odin/cli.h` and `odin/cli_client.h`:

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
} odin_cli_args_t;

typedef struct odin_cli_client_config_t {
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_client_transport_t transport;
  const char *quic_ca_file;
} odin_cli_client_config_t;
```

Client long options become:

```c
static const struct option kClientLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"server", required_argument, NULL, 's'},
    {"ca-file", required_argument, NULL, 1003},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};
```

Client usage becomes:

```text
usage: odin-client --listen ADDR --server ADDR [--ca-file FILE]
```

**Unstated contract.** `quic_ca_file` is initialized to `NULL` before parsing. `--ca-file FILE` and `--ca-file=FILE` are Client-mode only, alias the option value provided by `getopt_long` without allocation or canonicalization, and are accepted only when `FILE[0] != '\0'`. With otherwise-valid Client required options, an empty value from either `--ca-file ""` or `--ca-file=` returns `ODIN_CLI_ERR_BAD_QUIC_TLS` with `mode == ODIN_CLI_MODE_CLIENT`, leaves `quic_ca_file == NULL`, and maps through `odin_cli_main` to `odin: invalid QUIC TLS configuration\n<U_C>\n` using the updated Client usage. If the same argv also has a bad `--listen` value, bad `--server` value, or omitted required Client option, the existing `ODIN_CLI_ERR_BAD_LISTEN_PORT`, `ODIN_CLI_ERR_BAD_SERVER`, or `ODIN_CLI_ERR_MISSING_REQUIRED` result wins before `ODIN_CLI_ERR_BAD_QUIC_TLS`, and `odin_cli_main` emits the corresponding existing Client banner with the updated `usage: odin-client --listen ADDR --server ADDR [--ca-file FILE]\n` line. A missing required argument, abbreviated long spelling such as `--ca`, stray positional value, or Server-mode `--ca-file` remains `ODIN_CLI_ERR_UNKNOWN_FLAG` under the existing precedence. Help and unknown-flag precedence do not change. Server-mode `--quic-cert` and `--quic-key` behavior remains unchanged, and Server mode never writes `quic_ca_file`.

`odin_cli_main` passes `args.quic_ca_file` into `odin_cli_client_config_t`. For `odin-client --listen 8080 --ca-file ""`, `odin_cli_main` emits the missing-required banner and Client usage because `--server` is still absent; it does not emit the QUIC TLS banner on that argv. The ready banner remains `odin: mode=client transport=quic listen=<port> server=<host>:<port>\n`; it does not echo the CA path.

**Mechanism.**

```text
parse(argc, argv, out):
  zero out, including quic_ca_file = NULL
  if mode == CLIENT:
    parse --listen, --server, --ca-file, --help
  if --ca-file was present with a separate or equals-form empty argument:
    bad_quic_tls = 1
  preserve existing status precedence:
    if help_seen: return ODIN_CLI_HELP
    if unknown_flag_seen: return ODIN_CLI_ERR_UNKNOWN_FLAG
    if bad listen, bad server, or missing required: return the existing error
  if bad_quic_tls:
    return ODIN_CLI_ERR_BAD_QUIC_TLS
  on Client OK:
    out.quic_ca_file = ca_file_arg_or_NULL

main(argc, argv, out, err):
  on Client OK:
    config.quic_ca_file = args.quic_ca_file
    return odin_cli_run_client(&config, err)
```

Satisfies: G1 via the exact Client option, empty-value rejection, omitted-option `NULL`, and Server-mode isolation; G2 via the client-runner config field.

#### 3.2.2 Default Client Runtime CA Verification

Extend the default runtime config in `odin/client_xqc_runtime.h`:

```c
typedef struct odin_xqc_client_runtime_default_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  const char *server_host;
  const char *ca_file;
} odin_xqc_client_runtime_default_config_t;
```

**Unstated contract.** `ca_file == NULL` preserves the current default-create behavior observed at [odin/client_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_xqc_runtime.c:1118): zeroed raw xquic TLS config, `transport_callbacks == NULL`, `conn_ssl_config.cert_verify_flag == 0`, and `no_crypto_flag == 0`. `ca_file != NULL` requires a non-empty NUL-terminated PEM file path. `create_default` loads the CA file into a runtime-owned `X509_STORE` before calling `odin_xqc_client_runtime_create`; an empty or unloadable file path returns `-1` with `errno = EINVAL`, leaves `*out` untouched, records no UDP create, and starts no QUIC connection. `X509_STORE_new()` allocation failure returns `-1` with `errno = ENOMEM`, leaves `*out` untouched, and records no load attempt. If the CA store loads successfully but `odin_xqc_client_runtime_create(&full_config, &rt)` fails, `create_default` frees the loaded store exactly once, preserves the constructor errno, and leaves `*out` untouched. Once loaded, verification uses the in-memory store rather than reopening the CA path during handshake; changing or unlinking that path after successful `create_default` does not change callback acceptance.

When `ca_file != NULL`, `create_default` sets `conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY`, installs the Odin CA-file callback in the transport callback table, and then calls the existing full runtime constructor. The existing full constructor already rejects `NEED_VERIFY` without a non-null `cert_verify_cb` at [odin/client_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_xqc_runtime.c:1045). The §3.2.3 xquic bridge invokes that callback both when default BoringSSL verification fails with an issuer-local error and when default BoringSSL verification succeeds at leaf depth, so a certificate chain trusted by xquic's default paths still must pass Odin's configured CA-file verifier. The runtime continues to default missing `save_token`, `save_session_cb`, and `save_tp_cb` to no-op callbacks as it does today. Destroy and force-destroy free the runtime-owned `X509_STORE` exactly once with the rest of the copied runtime config.

Under `ODIN_XQC_CLIENT_RUNTIME_TESTING`, `odin_xqc_client_runtime_test_fail_next_x509_store_new(int errnum)` is declared in `odin/testing/client_xqc_runtime_internal_test.h` and arms only the next `X509_STORE_new()` call in `create_default`'s `ca_file != NULL` branch. It returns `-1/EINVAL` without arming when `errnum <= 0`. The same header also declares `odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t` with values `ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_NEW`, `ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_INIT`, and `ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_SET_DEFAULT`, plus `odin_xqc_client_runtime_test_fail_next_x509_store_ctx(odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t failpoint)`. That hook arms only the next verifier callback, forces the named `X509_STORE_CTX_new`, `X509_STORE_CTX_init`, or `X509_STORE_CTX_set_default(ctx, "ssl_server")` setup branch to fail before `X509_verify_cert`, records the setup failure kind, frees temporaries allocated before the failure, increments callback failure count, and returns non-`XQC_OK`; an invalid enum returns `-1/EINVAL` without arming. The same test record exposes verifier temporary-object allocation/free counters for leaf `X509`, intermediate `X509`, intermediate `STACK_OF(X509)`, and `X509_STORE_CTX`; the verifier increments the allocation side only after it owns a temporary and increments the free side at the `X509_free`, `sk_X509_pop_free`, and `X509_STORE_CTX_free` call sites. T9/T10/T14 assert exact expected deltas as `(leaf X509, intermediate X509, intermediate stack, store context)` where each tuple entry means `alloc_delta == free_delta == value`; nonzero entries are required for every temporary category exercised by that branch, so an all-zero counter implementation fails any parsed-leaf, parsed-intermediate, stack, or context subcase.

The certificate callback receives xquic's DER certificate array. It recovers `odin_xqc_client_runtime_t *` from the callback `conn_user_data` by treating it as `odin_xqc_udp_t *` and calling `odin_xqc_udp_app_user_data`, matching the existing connection-callback recovery pattern in [odin/client_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_xqc_runtime.c:1357). It rejects missing callback context, missing runtime, missing runtime CA store, null `certs`, null `cert_len`, `certs_len == 0`, null `certs[0]`, malformed DER, DER with trailing bytes after `d2i_X509`, host or IP mismatch, and chains that `X509_verify_cert` cannot build to the loaded CA store. The verifier initializes a fresh `X509_STORE_CTX` with only `rt.ca_store`, calls `X509_STORE_CTX_set_default(ctx, "ssl_server")`, and rejects if that setup fails before `X509_verify_cert`; BoringSSL documents that `"ssl_server"` configures TLS-server trust and purpose checks at [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:3100), including the `X509_PURPOSE_SSL_SERVER` EKU/key-usage checks documented at [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:3399). The leaf identity check uses `X509_check_ip_asc` when `server_host` parses as an IPv4 or IPv6 literal and `X509_check_host` otherwise; those BoringSSL APIs are declared at [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:4573) and [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:4618). Intermediate certificates from `certs[1..certs_len)` are parsed as exact DER into a temporary `STACK_OF(X509)` that is passed to `X509_STORE_CTX_init` as untrusted chain material; the verifier never adds peer-supplied intermediates to `rt.ca_store` or any other trust store. Malformed intermediate DER or a valid intermediate followed by trailing bytes rejects the whole chain. A leaf signed by `thor/out/intermediate-ca.pem` succeeds against a store loaded only from `thor/out/root-ca.pem` only when the callback receives the intermediate DER in `certs[1]`; the same leaf without that intermediate DER fails chain building. T7 drives this requirement through the production xquic handshake by serving `thor/out/intermediate-server-chain.pem`, whose PEM blocks are the intermediate-signed leaf followed by `thor/out/intermediate-ca.pem`; T14 keeps the direct verifier proof that the leaf-only callback fails and the leaf-plus-intermediate callback succeeds. A leaf signed by valid but unconfigured `thor/out/untrusted-intermediate-ca.pem`, which is outside the `root-ca.pem` issuing hierarchy, still fails against `thor/out/root-ca.pem` even when that untrusted intermediate is supplied in `certs[1]`, proving peer-supplied intermediates are not trust anchors. Every temporary `X509`, `STACK_OF(X509)`, and `X509_STORE_CTX` is freed before the callback returns.

**Mechanism.**

```text
create_default(config, out):
  validate existing default config fields and address lengths
  ca_store = NULL
  if config.ca_file != NULL:
    if config.ca_file[0] == '\0': errno = EINVAL; return -1
    if test fail_next_x509_store_new is armed:
      errno = armed_errno; return -1
    ca_store = X509_STORE_new()
    if ca_store == NULL: errno = ENOMEM; return -1
    if X509_STORE_load_locations(ca_store, config.ca_file, NULL) != 1:
      free ca_store; errno = EINVAL; return -1
    conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY
    transport_callbacks.cert_verify_cb = runtime_ca_file_cert_verify
    full_config.transport_callbacks = &transport_callbacks
  else:
    full_config.transport_callbacks = NULL
  if odin_xqc_client_runtime_create(&full_config, &rt) != 0:
    free ca_store; return -1 with preserved errno
  rt.ca_store = ca_store
  *out = rt
  return 0

runtime_ca_file_cert_verify(certs, cert_len, certs_len, conn_user_data):
  rt = app_user_data_from_xqc_udp_user_data(conn_user_data)
  reject if rt, rt.ca_store, certs, cert_len, certs_len == 0, or certs[0] is missing
  leaf = parse_one_der_x509(certs[0], cert_len[0])
  after any temporary is allocated, rejection jumps to cleanup before returning non-XQC_OK
  reject if parse did not consume exactly cert_len[0]
  if rt.server_host is an IP literal:
    reject unless X509_check_ip_asc(leaf, rt.server_host, 0) == 1
  else:
    reject unless X509_check_host(leaf, rt.server_host, strlen(rt.server_host),
                                  X509_CHECK_FLAG_NEVER_CHECK_SUBJECT,
                                  NULL) == 1
  intermediates = parse certs[1..certs_len) as exact DER
  reject if any intermediate parse fails or leaves trailing bytes
  ctx = NULL if test fail_next_x509_store_ctx == FAIL_NEW else X509_STORE_CTX_new()
  reject unless ctx != NULL
  init_ok = 0 if test fail_next_x509_store_ctx == FAIL_INIT
            else X509_STORE_CTX_init(ctx, rt.ca_store, leaf, intermediates)
  reject unless init_ok
  default_ok = 0 if test fail_next_x509_store_ctx == FAIL_SET_DEFAULT
               else X509_STORE_CTX_set_default(ctx, "ssl_server")
  reject unless default_ok == 1
  result = XQC_OK if X509_verify_cert(ctx) == 1, otherwise -1
  free ctx, intermediates, and leaf, updating temporary-object free counters
  return result
```

Satisfies: G2 via the optional default config field and create-time CA load; G3 via the runtime-owned CA store, xquic verification flag, callback installation, chain verification, and identity check.

#### 3.2.3 XQUIC Client Verification Bridge

The current xquic TLS setup always calls `X509_VERIFY_PARAM_set1_host` when `XQC_TLS_CERT_FLAG_NEED_VERIFY` is set at [xquic/src/tls/xqc_tls.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/tls/xqc_tls.c:233). BoringSSL documents that API as a DNS-name check at [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:3334), while the Odin default runtime passes the configured `server_host` identity through to xquic and that identity can be an IPv4 or IPv6 literal.

The current xquic client SSL context does not load default verify paths in [xquic/src/tls/xqc_tls_ctx.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/tls/xqc_tls_ctx.c:39), while the server context calls `SSL_CTX_set_default_verify_paths` at [xquic/src/tls/xqc_tls_ctx.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/tls/xqc_tls_ctx.c:145). BoringSSL documents that `SSL_CTX_set_default_verify_paths` loads the context store from `X509_STORE_set_default_paths` at [boringssl/include/openssl/ssl.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/ssl.h:2975), and `X509_STORE_set_default_paths` uses the `SSL_CERT_FILE` environment variable at [boringssl/include/openssl/x509.h](/Users/tangjiacheng/Downloads/Monorepo/boringssl/include/openssl/x509.h:3672).

```c
/* xquic/src/tls/xqc_tls_ctx.c */
SSL_CTX_set_default_verify_paths(ssl_ctx);
```

```c
/* xquic/src/tls/xqc_tls.c */
if (cfg->cert_verify_flag & XQC_TLS_CERT_FLAG_NEED_VERIFY) {
    if (xqc_tls_set_verify_identity(SSL_get0_param(ssl), hostname) !=
        XQC_SSL_SUCCESS) {
        xqc_log(tls->log, XQC_LOG_DEBUG,
                "|certificate verify set identity failed|");
        ret = -XQC_TLS_INTERNAL;
        goto end;
    }
    SSL_set_verify(ssl, SSL_VERIFY_PEER, xqc_ssl_cert_verify_cb);
}
```

**Unstated contract.** `xqc_create_client_ssl_ctx` calls `SSL_CTX_set_default_verify_paths(ssl_ctx)` before publishing `ctx->ssl_ctx`; its return value is intentionally ignored, matching the current server-context behavior, so hosts without usable default paths still create a client context and rely on Odin's CA-file callback after issuer-local default-verifier failures. These default paths are not authoritative for Odin CA-file acceptance; they only let xquic's default verifier reach the `ok == XQC_SSL_SUCCESS` leaf-depth callback when the process trust environment already accepts the peer chain.

`xqc_tls_set_verify_identity` first tries `X509_VERIFY_PARAM_set1_ip_asc(param, hostname)`. A return value of `1` means the configured identity is an IPv4 or IPv6 literal and xquic will verify IP SANs. A return value of `0` is treated as "not an IP literal" after clearing the BoringSSL error queue, and the helper falls back to `X509_VERIFY_PARAM_set1_host(param, hostname, strlen(hostname))` for DNS names. This is not a platform-specific branch; T6 proves the IPv4 IP-literal success path through production xquic, T11 proves the IPv6 IP-literal success path through production xquic when IPv6 loopback is available, T7 proves the `set1_ip_asc == 0` to DNS-host success path and peer-intermediate delivery through production xquic, and alternate-platform binaries compile the same helper without runtime execution in this environment.

`xqc_ssl_cert_verify_cb` also changes its success branch. The existing callback returns `XQC_SSL_SUCCESS` immediately when BoringSSL calls it with `ok == XQC_SSL_SUCCESS`; that skips any application `cert_verify_cb`. The revised callback retrieves `SSL *`, `xqc_tls_t *`, and the DER chain before deciding the success branch. If `ok == XQC_SSL_SUCCESS` and `X509_STORE_CTX_get_error_depth(store_ctx) != 0`, xquic returns success because BoringSSL has not reached the leaf success callback yet. If `ok == XQC_SSL_SUCCESS` at depth 0, xquic invokes the application `cert_verify_cb` with the peer DER chain; an application rejection sets `X509_V_ERR_APPLICATION_VERIFICATION` with `X509_STORE_CTX_set_error` and returns `XQC_SSL_FAIL`. The existing failure branch still invokes the application callback for `X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY`, `X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE`, and the existing self-signed allowance case; all other BoringSSL verification errors fail before the application callback. T12 drives that fatal branch with a default-trusted chain whose hostname does not match the configured server identity and asserts that Odin's verifier counters do not change.

This RFC does not add a harness for the TLS-internal `xqc_tls_callbacks_t.cert_verify_cb == NULL` branch. `xqc_tls_callbacks_t` is internal to [xquic/src/tls/xqc_tls_defs.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/tls/xqc_tls_defs.h:150), and the exported client connection path used by Odin installs `xqc_conn_tls_cbs.cert_verify_cb` at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:6365); the Odin full runtime constructor rejects `XQC_TLS_CERT_FLAG_NEED_VERIFY` without public `xqc_transport_callbacks_t.cert_verify_cb` at [odin/client_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_xqc_runtime.c:1045). The CA-file behavior under test therefore never depends on a hidden TLS-layer no-app callback path. If xquic needs that branch as a tested contract, a focused follow-up xquic bridge RFC must add an internal xquic test target or exported API that can drive it.

Under `XQC_TLS_TESTING`, `xquic/src/tls/xqc_tls_internal_test.h` exposes two public test APIs and hidden cross-translation-unit record helpers. Only `xqc_tls_test_reset()` and `xqc_tls_test_record()` carry `XQC_EXPORT_PUBLIC_API` from `xquic/include/xquic/xquic_typedef.h`, so those two functions are externally visible from the debug `component("xquic_testing")` shared library even though default targets use `//build:visibility_hidden`. The helpers intentionally have external linkage but no export annotation: `xqc_tls.c` defines them against its file-static record, while `xqc_tls_ctx.c` and `xqc_tls.c` can both call them inside the same `//xquic:xquic_testing` linked object set. `xqc_tls_test_default_verify_paths_result(int result)` records the client default-path result and, only when the serial test has set `XQC_TLS_TEST_FORCE_DEFAULT_VERIFY_PATHS_RET=0`, returns `0` to force the ignored-return branch; production xquic and non-testing targets call `SSL_CTX_set_default_verify_paths` directly.

```c
/* xquic/src/tls/xqc_tls_internal_test.h */
#include <stdint.h>
#include "xquic/xquic_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XQC_TLS_TESTING)
typedef struct xqc_tls_test_record_s {
    uint64_t client_default_verify_paths_calls;
    uint64_t client_default_verify_paths_failures;
    uint64_t success_non_leaf_bypass_calls;
    uint64_t leaf_ok_app_verify_calls;
    uint64_t issuer_local_app_verify_calls;
} xqc_tls_test_record_t;

XQC_EXPORT_PUBLIC_API void xqc_tls_test_reset(void);
XQC_EXPORT_PUBLIC_API const xqc_tls_test_record_t *xqc_tls_test_record(void);

int xqc_tls_test_default_verify_paths_result(int result);
void xqc_tls_test_inc_success_non_leaf_bypass_calls(void);
void xqc_tls_test_inc_leaf_ok_app_verify_calls(void);
void xqc_tls_test_inc_issuer_local_app_verify_calls(void);
#endif

#ifdef __cplusplus
}
#endif
```

```c
/* xquic/src/tls/xqc_tls.c */
#if defined(XQC_TLS_TESTING)
static xqc_tls_test_record_t g_xqc_tls_test_record;

XQC_EXPORT_PUBLIC_API void
xqc_tls_test_reset(void)
{
    g_xqc_tls_test_record = (xqc_tls_test_record_t){0};
}

XQC_EXPORT_PUBLIC_API const xqc_tls_test_record_t *
xqc_tls_test_record(void)
{
    return &g_xqc_tls_test_record;
}

static int
xqc_tls_test_force_default_verify_paths_zero(void)
{
    const char *value = getenv("XQC_TLS_TEST_FORCE_DEFAULT_VERIFY_PATHS_RET");
    return value != NULL && strcmp(value, "0") == 0;
}

int
xqc_tls_test_default_verify_paths_result(int result)
{
    ++g_xqc_tls_test_record.client_default_verify_paths_calls;
    if (xqc_tls_test_force_default_verify_paths_zero()) {
        result = 0;
    }
    if (result != XQC_SSL_SUCCESS) {
        ++g_xqc_tls_test_record.client_default_verify_paths_failures;
    }
    return result;
}

void
xqc_tls_test_inc_success_non_leaf_bypass_calls(void)
{
    ++g_xqc_tls_test_record.success_non_leaf_bypass_calls;
}

void
xqc_tls_test_inc_leaf_ok_app_verify_calls(void)
{
    ++g_xqc_tls_test_record.leaf_ok_app_verify_calls;
}

void
xqc_tls_test_inc_issuer_local_app_verify_calls(void)
{
    ++g_xqc_tls_test_record.issuer_local_app_verify_calls;
}
#endif
```

The hook is compiled through a test-only xquic target, not by relying on consumer configs from `odin_unittests`:

```gn
# build/secondary/xquic/BUILD.gn
config("xquic_tls_testing_config") {
  defines = [ "XQC_TLS_TESTING" ]
}

component("xquic_testing") {
  testonly = true
  public_configs = [
    ":xquic_public_config",
    ":xquic_tls_testing_config",
  ]
  configs += [
    ":xquic_internal_config",
    ":xquic_tls_testing_config",
  ]
  sources = transport_sources + tls_sources + common_sources +
            congestion_control_sources
  deps = [
    "//boringssl:crypto",
    "//boringssl:ssl",
  ]
}
```

`//odin/testing:odin_unittests` replaces its direct `//xquic` link dependency with `//xquic:xquic_testing`; it must not link both xquic targets. The production `odin_main` path continues to use `//xquic` through `//odin:odin_cli_artifacts` data deps, but that production xquic object set is not linked into `odin_unittests`. Because `xquic_tls_testing_config` is in `configs` on `xquic_testing`, `xquic/src/tls/xqc_tls.c` and `xquic/src/tls/xqc_tls_ctx.c` compile their guarded declarations and call-site increments with `XQC_TLS_TESTING`; because the same config is also public, `odin_unittests` test files can include the header. Because only `xqc_tls_test_reset()` and `xqc_tls_test_record()` are annotated with `XQC_EXPORT_PUBLIC_API` at declaration and definition, the default debug `xquic_testing` component exports those two test symbols without opting the whole target into `//build:visibility_default`. The hidden helpers are declared in the same header but remain non-exported implementation details of the testing component. The record counts `client_default_verify_paths_calls` and `client_default_verify_paths_failures` from `xqc_tls_ctx.c` around the client `SSL_CTX_set_default_verify_paths` call, `success_non_leaf_bypass_calls` from the success branch that returns before any application callback at non-leaf depth, `leaf_ok_app_verify_calls` from `xqc_tls.c` immediately before the leaf-depth success-branch application `cert_verify_cb` call, and `issuer_local_app_verify_calls` from `xqc_tls.c` immediately before the issuer-local failure-branch application `cert_verify_cb` call. Except for the forced zero result when the serial test sets `XQC_TLS_TEST_FORCE_DEFAULT_VERIFY_PATHS_RET=0`, these counters do not alter verifier return values and exist only in the test-only xquic target used by `odin_unittests`.

**Mechanism.**

```text
xqc_create_client_ssl_ctx(ctx):
  ssl_ctx = SSL_CTX_new(TLS_method())
  perform existing client context setup
  default_paths_ok = SSL_CTX_set_default_verify_paths(ssl_ctx)
  default_paths_ok = xqc_tls_test_default_verify_paths_result(default_paths_ok)
  ignore default_paths_ok
  ctx->ssl_ctx = ssl_ctx
  return XQC_OK

xqc_tls_set_verify_identity(param, hostname):
  if X509_VERIFY_PARAM_set1_ip_asc(param, hostname) == 1:
    return XQC_SSL_SUCCESS
  ERR_clear_error()
  return X509_VERIFY_PARAM_set1_host(param, hostname, strlen(hostname))

xqc_ssl_cert_verify_cb(ok, store_ctx):
  ssl = X509_STORE_CTX_get_ex_data(store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx())
  reject unless ssl and SSL_get_app_data(ssl) produce tls
  if ok == XQC_SSL_SUCCESS:
    if X509_STORE_CTX_get_error_depth(store_ctx) != 0:
      xqc_tls_test_inc_success_non_leaf_bypass_calls()
      return XQC_SSL_SUCCESS
    xqc_tls_test_inc_leaf_ok_app_verify_calls()
    return xqc_tls_call_app_cert_verify_cb_or_set_application_error(tls, ssl,
                                                                    store_ctx)
  if X509_STORE_CTX_get_error(store_ctx) is not issuer-local or allowed self-signed:
    return XQC_SSL_FAIL
  xqc_tls_test_inc_issuer_local_app_verify_calls()
  return xqc_tls_call_app_cert_verify_cb_or_set_application_error(tls, ssl,
                                                                  store_ctx)
```

Satisfies: G3 via loading client default paths for xquic's default verifier, preserving DNS certificate verification, making configured IPv4 and IPv6 literal server identities verifiable before the Odin CA-file callback runs, and requiring Odin's configured CA-file callback even when xquic's default trust store accepts the peer chain.

## 4. Security

- **S1.**
  - **Threat:** A malicious QUIC server presents a certificate chain that is not rooted in the configured CA file, or presents a valid chain for a different DNS name or IP address, and the client accepts it.
  - **Mitigation:** §3.2.2 enables xquic certificate verification only when a CA file is configured, verifies the DER chain against only the loaded `X509_STORE`, rejects store-context setup failures before `X509_verify_cert`, applies `ssl_server` purpose checks, and checks the leaf identity; §3.2.3 ensures xquic's pre-callback identity check uses IP SAN verification for IP literals and DNS verification for DNS names, then invokes Odin's CA-file callback even when xquic's default trust paths accept the chain.
  - **Enforcement:** T6, T7, and T11 exercise successful IPv4 IP, DNS, and IPv6 IP CA verification through production handshakes; T7 also proves a valid root-to-intermediate-to-leaf chain succeeds through production xquic only when the peer-supplied intermediate reaches Odin's callback; T14 proves the same intermediate requirement at direct verifier level and proves verification still uses the loaded store after the CA path is unlinked; T8 proves production handshake rejection for untrusted, DNS-mismatched, IP-mismatched, and configured-CA CN-only no-SAN servers; T10 proves a valid but untrusted peer-supplied intermediate chain is rejected and store-context setup failures do not fall through to `X509_verify_cert`; T12 proves both that a default-trusted but CA-file-untrusted production chain reaches xquic's default-verifier leaf success branch before Odin rejects it and that a fatal default-verifier identity error fails before Odin is invoked; T15 proves xquic still creates a client context when default-path loading returns 0 and proves non-leaf success bypasses do not invoke the application verifier before leaf depth; T13 proves a configured-CA chain without TLS server authentication is rejected; T9 and T10 prove the callback rejection branches.

- **S2.**
  - **Threat:** An operator supplies an empty or unreadable CA-file path and the client silently continues without certificate verification.
  - **Mitigation:** §3.2.1 rejects empty CLI values, and §3.2.2 loads the CA file during `create_default` before UDP creation or `xqc_connect`; load failure returns `-1/EINVAL`.
  - **Enforcement:** T2 and T5 assert empty and unreadable path rejection with no runtime startup side effects.

- **S3.**
  - **Threat:** Peer-supplied malformed certificate DER reaches the verifier and is accepted, or causes a crash or leaked X509 objects.
  - **Mitigation:** §3.2.2 rejects null arrays, zero-length chains, malformed DER, and DER with trailing bytes, then frees every temporary `X509`, `STACK_OF(X509)`, and `X509_STORE_CTX` before returning.
  - **Enforcement:** T9 and T10 fire the malformed peer-certificate inputs and assert deterministic callback rejection; T9, T10, and T14 assert exact per-branch allocation/free deltas for the verifier's leaf `X509`, intermediate `X509`, intermediate `STACK_OF(X509)`, and `X509_STORE_CTX` temporaries, including positive expected deltas wherever a branch owns that object category.

## 5. Testing Strategy

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 parser option | T1, T2 |
| G# | G2 CLI-to-runtime handoff | T3, T4, T5 |
| G# | G3 CA-backed verification | T4, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15 |
| S# | S1 untrusted or wrong-identity server cert | T6, T7, T8, T9, T10, T11, T12, T13, T14, T15 |
| S# | S2 empty or unreadable CA path | T2, T5 |
| S# | S3 malformed peer cert DER | T9, T10 |
| State | CLI omitted CA file | T1, T3 |
| State | CLI supplied CA file | T1, T3 |
| State | CLI supplied CA file with equals form | T1 |
| State | CLI invalid CA option form | T2 |
| State | CLI empty CA value with equals form | T2 |
| State | CLI empty CA value plus bad listen | T2 |
| State | CLI empty CA value plus bad server | T2 |
| State | CLI empty CA value plus missing required Client option | T2 |
| State | CLI supplied CA file followed by stray positional | T2 |
| State | CLI CA-file help precedence | T2 |
| State | Server-mode `--ca-file` isolation | T2 |
| State | Runtime default omitted CA file | T4 |
| State | Runtime default supplied valid CA file | T3, T4, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15 |
| State | Runtime default supplied invalid CA file | T5 |
| State | Verifier receives valid IPv4 IP-identity chain | T6 |
| State | Production handshake receives valid IPv6 IP-identity chain | T11 |
| State | Production handshake receives valid intermediate-required DNS-identity chain | T7 |
| State | Direct verifier receives valid intermediate-required chain | T14 |
| State | Verifier receives valid but untrusted peer-supplied intermediate chain | T10 |
| State | Production handshake receives invalid DNS-identity chain | T8 |
| State | Production handshake receives invalid IP-identity chain | T8 |
| State | Production handshake receives untrusted chain | T8 |
| State | Production handshake receives configured-CA CN-only DNS leaf without SAN | T8 |
| State | Production handshake receives default-trusted but CA-file-untrusted chain | T12 |
| State | XQUIC default verifier reaches leaf-depth success before Odin rejection | T12 |
| State | XQUIC default verifier reaches fatal identity failure before Odin callback | T12 |
| State | XQUIC client context proceeds after forced default-path load failure | T15 |
| State | XQUIC success branch reaches non-leaf depth before leaf success | T15 |
| State | Production handshake receives configured-CA chain invalid for TLS server auth | T13 |
| State | Verifier receives invalid leaf or precondition input | T9 |
| State | Verifier receives invalid IP-identity chain | T9 |
| State | Verifier receives invalid context, intermediate, or trust input | T10 |
| Test hook | Store-context failpoint invalid enum is rejected without arming | T10 |
| State | Runtime verification uses loaded CA store after CA path unlink | T14 |
| Completion mode | Happy single-call parser or constructor | T1, T4 |
| Completion mode | Happy verifier callback | T14 |
| Completion mode | Happy event-loop startup and handshake | T3, T6, T7, T11, T15 |
| Completion mode | Failed production handshake | T8, T12, T13 |
| Decoder branch | PEM CA-file load succeeds | T3, T4, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15 |
| Decoder branch | PEM CA-file load fails | T5 |
| Decoder branch | CA-store allocation fails | T5 |
| Decoder branch | DER peer chain parses | T6, T7, T8, T9, T10, T11, T12, T13, T14, T15 |
| Decoder branch | DER peer chain parse fails | T9, T10 |
| Decoder branch | Store-context allocation, init, or default setup fails | T10 |
| Benign-vs-fatal split | Omitted CA leaves verification disabled | T1, T4 |
| Benign-vs-fatal split | Configured CA enables verification | T3, T4, T6, T7, T10, T11, T12, T13, T14, T15 |
| Benign-vs-fatal split | Bad CA path fails before UDP create | T5 |
| Benign-vs-fatal split | Bad peer cert fails before relay | T8, T12, T13 |
| Constructor / factory precondition | Empty or unreadable `ca_file` | T5 |
| Constructor / factory failure | CA-store allocation failure and post-load constructor failure | T5 |
| Post-syscall sub-branch | CA-file open/load failure | T5 |
| Cleanup branch | Loaded CA-store freed after post-load constructor failure | T5 |
| Cleanup branch | Loaded CA-store freed after successful CLI force-destroy | T3 |
| Cleanup branch | Loaded CA-store freed after successful normal destroy | T4 |
| Cleanup branch | Verifier temporary objects freed on callback success and failure | T9, T10, T14 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Client parser accepts CA file and omission | Subcases: `odin-client --listen 8080 --server 127.0.0.1:4433`, `odin-client --listen 8080 --server 127.0.0.1:4433 --ca-file <repo>/thor/out/root-ca.pem`, `odin-client --listen 8080 --server 127.0.0.1:4433 --ca-file=<repo>/thor/out/root-ca.pem` | All return `ODIN_CLI_OK`; omitted case has `quic_ca_file == NULL`; separate-argument supplied case has `quic_ca_file` pointer equal to the CA argv slot; equals-form supplied case stores the exact path string from after `--ca-file=`; listen, server host, server port, and `client_transport` match the existing Client OK contract | G1 | unit |
| T2 | Bad CA-file option forms and precedence are pinned | Common valid Client prefix is `odin-client --listen 8080 --server 127.0.0.1:4433`. Subcase groups: empty CA with valid required options (`<prefix> --ca-file ""`, `<prefix> --ca-file=`); bad listen plus empty CA (`odin-client --listen abc --server 127.0.0.1:4433 --ca-file ""`); bad server plus empty CA (`odin-client --listen 8080 --server 127.0.0.1:bad --ca-file ""`); missing required plus empty CA (`odin-client --listen 8080 --ca-file ""`); invalid option forms (`<prefix> --ca-file` with no argument, `<prefix> --ca` abbreviation, `<prefix> --ca-file <repo>/thor/out/root-ca.pem extra`, and `odin-server --ca-file <repo>/thor/out/root-ca.pem --listen 4433 --quic-cert C --quic-key K`); help precedence (`odin-client --help --ca-file ""`) | Empty CA with valid required options returns `ODIN_CLI_ERR_BAD_QUIC_TLS`, Client mode, and `quic_ca_file == NULL`; `odin_cli_main` writes `odin: invalid QUIC TLS configuration\nusage: odin-client --listen ADDR --server ADDR [--ca-file FILE]\n` and returns 2 without invoking the runner. Bad listen plus empty CA returns `ODIN_CLI_ERR_BAD_LISTEN_PORT` and `odin_cli_main` writes `odin: invalid --listen port\nusage: odin-client --listen ADDR --server ADDR [--ca-file FILE]\n`. Bad server plus empty CA returns `ODIN_CLI_ERR_BAD_SERVER` and writes `odin: invalid --server\nusage: odin-client --listen ADDR --server ADDR [--ca-file FILE]\n`. Missing required plus empty CA returns `ODIN_CLI_ERR_MISSING_REQUIRED` and writes `odin: missing required flag\nusage: odin-client --listen ADDR --server ADDR [--ca-file FILE]\n`. All precedence cases leave `quic_ca_file == NULL`. Missing argument, abbreviation, stray positional, and Server-mode use return `ODIN_CLI_ERR_UNKNOWN_FLAG`; Server mode leaves `mode == ODIN_CLI_MODE_SERVER` and `quic_ca_file == NULL`; help returns `ODIN_CLI_HELP` and prints only Client usage to stdout | G1, S2 | unit |
| T3 | CLI runner forwards supplied CA file and preserves omitted startup default | Two `odin_cli_main` children with `--listen 0 --server 127.0.0.1:4433`: one adds `--ca-file <repo>/thor/out/root-ca.pem`, one omits `--ca-file`. Parent deadline-reads the ready line and runtime snapshot pipe within 2 s, sends `SIGTERM`, and waits with the existing 2 s child deadline | Both ready lines remain `odin: mode=client transport=quic listen=<port> server=127.0.0.1:4433\n`. Supplied child records the repo-root CA path in the CLI runtime-config and default-create records, `conn_ssl_config_value.cert_verify_flag == XQC_TLS_CERT_FLAG_NEED_VERIFY`, non-null `transport_callbacks_value.cert_verify_cb`, one CA-store load, and exactly one CA-store free during CLI `force_destroy` cleanup. Omitted child records `quic_ca_file == NULL`, default-create `ca_file == NULL`, `transport_callbacks == NULL`, `cert_verify_flag == 0`, no callback, and no CA-store free. Both leave listener, accept loop, runtime, signal timer, and event-loop liveness counters at zero | G2 | integration |
| T4 | Default runtime CA defaults, configured path, and normal cleanup | Direct `odin_xqc_client_runtime_create_default` with valid loopback local and peer addrs. Subcases: `ca_file == NULL`, `ca_file == "<repo>/thor/out/root-ca.pem"`. After each successful assertion, call `odin_xqc_client_runtime_destroy` and drain any required close callbacks using the existing runtime unit-test harness | Omitted case succeeds with `last_default_create.transport_callbacks == NULL`, `conn_ssl_config_value.cert_verify_flag == 0`, no CA-store load counter, and no CA-store free after destroy. Supplied case succeeds with one CA-store load, runtime-owned store non-null in the test state, `conn_ssl_config_value.cert_verify_flag == XQC_TLS_CERT_FLAG_NEED_VERIFY`, non-null cert callback, no `XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED` bit, a verifier-user-data value available through the test state, and exactly one CA-store free after normal destroy. A double-free or missing-free implementation fails the exact counter assertions before ASan | G2, G3 | unit |
| T5 | Default runtime rejects and cleans CA-store setup failures | Direct `odin_xqc_client_runtime_create_default` with valid addresses and a sentinel `*out`. Subcases: `ca_file == ""`, `ca_file` under a nonexistent temp path, `ca_file` pointing at a temp file containing `not a certificate\n`, `odin_xqc_client_runtime_test_fail_next_x509_store_new(0)` before a valid CA create, `odin_xqc_client_runtime_test_fail_next_x509_store_new(-1)` before a valid CA create, valid CA after `odin_xqc_client_runtime_test_fail_next_x509_store_new(ENOMEM)`, valid CA after `odin_xqc_client_runtime_test_fail_config_copy_alloc(ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST, ENOMEM)` | Empty, nonexistent, and invalid-file subcases return `-1/EINVAL`, leave sentinel `*out` unchanged, and record no UDP create, no ALPN register, and no `xqc_connect`. The 0 and -1 hook subcases return `-1/EINVAL` from the hook without arming it; the immediately following valid-CA create succeeds, records a normal CA load, and records no store-allocation failure. The allocation-hook subcase returns `-1/ENOMEM`, leaves sentinel `*out` unchanged, records one store-allocation failure, no CA load, and no store free. The post-load constructor-failure subcase returns `-1/ENOMEM`, leaves sentinel `*out` unchanged, records one successful CA load, one CA-store free, no UDP create, no ALPN register, and no `xqc_connect`. A leaked or double-freed `X509_STORE` fails the exact CA-store free/load counter assertions | G2, S2 | unit |
| T6 | Production xquic verifies server chain with CA file and IP identity | Install `signal(SIGPIPE, SIG_IGN)`. Using P1 Thor fixtures, start production `odin_xqc_server_runtime` on `127.0.0.1:0` with `thor/out/odin-server.pem` and key. Create production client default with peer set to the server UDP addr, `server_host == "127.0.0.1"`, and `ca_file == "thor/out/root-ca.pem"`. Every event-loop milestone and socket read/write uses a 2 s deadline: server bind, client handshake, local CONNECT write, HTTP 200 read, upstream read of `client-tail!`, upstream write of `server-tail!`, downstream tail read, and cleanup | Client records `XQC_CONNECT` with ALPN `odin/1`, `cert_verify_flag == XQC_TLS_CERT_FLAG_NEED_VERIFY`, non-null Odin cert callback, and `cert_verify_successes > 0`; xquic accepts the IP SAN path; downstream peer receives exact HTTP 200 then `server-tail!`; upstream origin receives exact `client-tail!`; no deadline expires; cleanup destroys both runtimes with no live sessions, registered CIDs, or open listener fds | G3, S1 | integration |
| T7 | Production xquic verifies intermediate server chain with CA file and DNS identity | Install `signal(SIGPIPE, SIG_IGN)`. Using P1 Thor fixtures, start production `odin_xqc_server_runtime` on `127.0.0.1:0` with `thor/out/intermediate-server-chain.pem` and `thor/out/intermediate-server-key.pem`; the chain PEM contains the intermediate-signed leaf followed by `thor/out/intermediate-ca.pem`. Create production client default with peer set to the server UDP addr, `server_host == "localhost"`, and `ca_file == "thor/out/root-ca.pem"`. Use the same payloads and 2 s deadlines as T6 | Client records `cert_verify_successes > 0` and the same exact relay bytes as T6; xquic accepts the leaf DNS SAN, proving the `X509_VERIFY_PARAM_set1_ip_asc(...) == 0` fallback to `X509_VERIFY_PARAM_set1_host(...)` succeeds on the production handshake path; Odin chain verification succeeds with a store containing only `root-ca.pem`, proving xquic delivered the peer intermediate as `certs[1]` to the callback; cleanup reaches zero live sessions, registered CIDs, and listener fds. An implementation that passes only the leaf DER from xquic to Odin fails chain building and fails this row | G3, S1 | integration |
| T8 | Production xquic rejects untrusted or wrong-identity server during handshake | Subcases A-C start the same production Thor server as T6 with all event-loop, socket, and cleanup waits capped at 2 s. Client A uses `server_host == "example.com"` with `ca_file == "thor/out/root-ca.pem"`. Client B uses `server_host == "127.0.0.2"` with `ca_file == "thor/out/root-ca.pem"`. Client C uses `server_host == "localhost"` with `ca_file == "boringssl/pki/testdata/cert_issuer_source_static_unittest/root.pem"`. Subcase D starts `odin_xqc_server_runtime` with `thor/out/cn-only-server.pem` and `thor/out/cn-only-server-key.pem`, where the root-signed leaf has subject `CN=localhost` and no SAN, then creates the client with `server_host == "localhost"` and `ca_file == "thor/out/root-ca.pem"`. A local CONNECT fd is added only if the test state reports handshake done | Each subcase fails the production handshake before HTTP 200 or relay bytes; `cert_verify_successes == 0`, `cert_verify_failures > 0`, and the failure reason is DNS identity mismatch for A, IP identity mismatch for B, chain verification failure for C, and Odin DNS identity mismatch for D. D proves the configured-CA verifier does not fall back from missing DNS SAN to subject CN; an implementation that omits `X509_CHECK_FLAG_NEVER_CHECK_SUBJECT` accepts D and fails this row. No local fd reaches an active relay, the origin accepts no client bytes by its read deadline, downstream receives EOF/reset or no bytes by its read deadline, and cleanup destroys both runtimes with no live sessions, registered CIDs, or open listener fds | G3, S1 | integration |
| T9 | CA verifier rejects leaf and argument failures | Create three row-local default runtimes with `ca_file == "thor/out/root-ca.pem"` and `server_host` values `localhost`, `example.com`, and `127.0.0.2`. For each runtime, capture `transport_callbacks_value.cert_verify_cb` and the matching verifier-user-data from that same live runtime, invoke its callback subcases before destroy, then destroy it in row cleanup. The `localhost` runtime covers null cert array, null `cert_len`, `certs_len == 0`, null `certs[0]`, malformed leaf bytes, and valid leaf DER with one trailing byte; the other runtimes cover Thor leaf DER under wrong DNS and IP identities | Every subcase returns non-`XQC_OK`; missing inputs record missing-argument failures, malformed/trailing leaf inputs record exact-DER parse failures, wrong identities record DNS or IP mismatch according to that runtime's `server_host`, callback failure count increments, and no callback or user-data is reused from another row. Exact counter tuples are `(0,0,0,0)` for missing inputs and malformed leaf before ownership, and `(1,0,0,0)` for parsed-leaf rejections: trailing leaf DER, wrong DNS, and wrong IP. An all-zero counter implementation fails the parsed-leaf subcases even if it returns the right verifier statuses | G3, S1, S3 | unit |
| T10 | CA verifier rejects bad callback context, malformed intermediates, unrelated CA, and setup failures | Create row-local live runtimes and invoke every callback before destroying its owning runtime. R0 uses `root-ca.pem` and `localhost`; capture its callback plus matching verifier-user-data for malformed-intermediate and setup-failpoint subcases, with setup failpoints using valid intermediate-server leaf plus `intermediate-ca` DER. Use the same callback for intentional bad-context inputs (`conn_user_data == NULL`, an `odin_xqc_udp_t` test object with null app user data, and a live omitted-CA runtime's row-local verifier-user-data). R1 uses the unrelated BoringSSL root to verify Thor leaf DER. R2 uses `root-ca.pem` for the untrusted-intermediate chain. The P1 fixture creates `thor/out/untrusted-intermediate-ca.pem` and `thor/out/untrusted-intermediate-server.pem` outside the `root-ca.pem` hierarchy | The invalid-enum hook call returns `-1/EINVAL` without arming, and the immediate valid R0 leaf-only callback returns `XQC_OK` without a setup-failure record. Every rejection subcase returns non-`XQC_OK`; bad context records missing runtime/store, malformed intermediate inputs record exact-DER parse failures before chain verification, wrong trust input records chain verification failure without swapping any runtime-owned store, the untrusted-intermediate chain fails against `thor/out/root-ca.pem` despite matching SAN `localhost`, and each valid setup failpoint records the named store-context setup failure before `X509_verify_cert`. Exact counter tuples are bad context `(0,0,0,0)`, invalid-enum immediate valid callback and unrelated-CA leaf-only failure `(1,0,0,1)`, malformed intermediate bytes `(1,0,1,0)`, trailing intermediate `(1,1,1,0)`, untrusted intermediate `(1,1,1,1)`, setup FAIL_NEW `(1,1,1,0)`, and setup FAIL_INIT/FAIL_SET_DEFAULT `(1,1,1,1)`. An all-zero counter implementation fails every parsed-object branch | G3, S1, S3 | unit |
| T11 | Production xquic verifies server chain with CA file and IPv6 identity | Preflight an `AF_INET6` UDP bind to `::1:0`; if it fails with `EAFNOSUPPORT` or `EADDRNOTAVAIL`, `GTEST_SKIP` records the errno before creating any xquic runtime. Otherwise install `signal(SIGPIPE, SIG_IGN)`, start production `odin_xqc_server_runtime` on `::1:0` with `thor/out/odin-server.pem` and key, create production client default with peer set to the server IPv6 UDP addr, `server_host == "::1"`, and `ca_file == "thor/out/root-ca.pem"`, and use the T6 2 s deadlines for bind, handshake, relay I/O, and cleanup | On IPv6-capable hosts, the client records `XQC_CONNECT` with `cert_verify_flag == XQC_TLS_CERT_FLAG_NEED_VERIFY`, non-null Odin cert callback, and `cert_verify_successes > 0`; xquic accepts the IPv6 IP SAN path before Odin's CA-file callback succeeds; downstream receives exact HTTP 200 then `server-tail!`; upstream receives exact `client-tail!`; no deadline expires; cleanup destroys both runtimes with no live sessions, registered CIDs, or open listener fds. An implementation that routes `::1` through `X509_VERIFY_PARAM_set1_host` fails the handshake and fails this row | G3, S1 | integration |
| T12 | Production xquic rejects default-trusted chains before and after Odin | Run in a serial test fixture because `SSL_CERT_FILE` is process-global. The P1 fixture action creates `thor/out/default-trust-only-root-ca.pem`, `thor/out/default-trust-only-server.pem`, and key with SAN `localhost,127.0.0.1`. Save the prior `SSL_CERT_FILE`, set it to that root, and preflight a fresh `SSL_CTX` with `SSL_CTX_set_default_verify_paths`, `X509_STORE_CTX_set_default(ctx, "ssl_server")`, `X509_VERIFY_PARAM_set1_host(..., "localhost", 9)`, and `X509_verify_cert` against the default-trust-only chain. Subcase A calls `xqc_tls_test_reset()`, starts that server, and creates a production client with `server_host == "localhost"` and `ca_file == "thor/out/root-ca.pem"`. Subcase B calls `xqc_tls_test_reset()`, starts that server, snapshots Odin verifier counters, and creates the client with `server_host == "example.com"` and the same CA file. All waits use the T6 2 s deadlines; restore the environment before row exit | A fails before HTTP 200 or relay bytes; `client_default_verify_paths_calls > 0`, `leaf_ok_app_verify_calls > 0`, and `issuer_local_app_verify_calls == 0`, proving BoringSSL accepted the chain through xquic's client default paths before Odin rejected it; `cert_verify_successes == 0`, `cert_verify_failures > 0`, and the recorded failure is Odin CA-file chain verification against `thor/out/root-ca.pem`, not identity mismatch. B fails before HTTP 200 or relay bytes on xquic's fatal default-verifier identity error; `client_default_verify_paths_calls > 0`, `leaf_ok_app_verify_calls == 0`, `issuer_local_app_verify_calls == 0`, and Odin verifier success/failure counters stay unchanged from the pre-handshake snapshot. A buggy xquic implementation that omits client default-path loading reaches the issuer-local branch in A; one that returns immediately on `ok == XQC_SSL_SUCCESS` completes A; one that invokes Odin after a fatal default-verifier error increments counters in B | G3, S1 | integration |
| T13 | Production xquic rejects configured-CA leaf lacking TLS server auth | The P1 fixture action creates `thor/out/odin-client-auth-only.pem` and key signed by `thor/out/root-ca.pem`, with SAN `localhost,127.0.0.1` and client-auth EKU only. Start production `odin_xqc_server_runtime` on `127.0.0.1:0` with that leaf/key. Create production client default with `server_host == "localhost"` and `ca_file == "thor/out/root-ca.pem"`. All event-loop, socket, and cleanup waits use the T6 2 s deadlines | The production handshake fails before HTTP 200 or relay bytes; identity matching succeeds far enough to record a TLS server-purpose verification failure, the verifier's last X509 error is `X509_V_ERR_INVALID_PURPOSE`, `cert_verify_successes == 0`, `cert_verify_failures > 0`, and cleanup reaches zero live sessions, registered CIDs, and listener fds. An implementation that calls `X509_verify_cert` without `X509_STORE_CTX_set_default(ctx, "ssl_server")` accepts this chain and fails the row | G3, S1 | integration |
| T14 | CA verifier accepts a required intermediate chain from the loaded store | The P1 fixture action creates intermediate CA certificate `thor/out/intermediate-ca.pem` signed by `thor/out/root-ca.pem` plus its key, and creates leaf certificate `thor/out/intermediate-server.pem` signed by that intermediate CA plus its key, with SAN `localhost,127.0.0.1,::1` and TLS server EKU. Create R0 with `ca_file == "thor/out/root-ca.pem"` and `server_host == "localhost"`, capture its callback and matching verifier-user-data, and before destroying R0 call leaf-only and leaf-plus-intermediate subcases. Then copy `root-ca.pem` to a temp CA path, create R1 from that temp path, capture R1's callback and matching verifier-user-data, unlink the temp path, call the leaf-plus-intermediate callback before destroying R1, and destroy both runtimes in row cleanup | The leaf-only call returns non-`XQC_OK`, records chain verification failure, and proves the root store did not accidentally trust or preload the intermediate. Both leaf-plus-intermediate calls return `XQC_OK`, record `cert_verify_successes > 0` with no identity mismatch, use only the owning live runtime's matching verifier-user-data, and verify `root-ca.pem -> intermediate-ca.pem -> intermediate-server.pem` while the runtime store contains only the loaded root. The post-unlink success proves the callback uses the runtime-owned store rather than reopening `ca_file`. Exact counter tuples are `(1,0,0,1)` for the leaf-only failure and `(1,1,1,1)` for each leaf-plus-intermediate success, for row-total deltas of leaf `3`, intermediate `2`, stack `2`, and context `3` on both alloc and free sides. An implementation that parses but ignores `certs[1]`, rereads the CA path during verification, reuses stale verifier user-data, leaves counters all zero, or leaks temporaries on the success path fails this row | G3, S1 | unit |
| T15 | XQUIC bridge covers ignored default-path failure and non-leaf success bypass | Install `signal(SIGPIPE, SIG_IGN)`. Serial fixture because it mutates `SSL_CERT_FILE` and `XQC_TLS_TEST_FORCE_DEFAULT_VERIFY_PATHS_RET`. Subcase A sets the force env var to `0`, starts the T6 Thor server, and creates an Odin client default with `server_host == "127.0.0.1"` and `ca_file == "thor/out/root-ca.pem"`. Subcase B clears the force env var, sets `SSL_CERT_FILE=thor/out/root-ca.pem`, starts `thor/out/intermediate-server-chain.pem`, and creates an Odin client default with `server_host == "localhost"` and the same CA file. Each subcase calls `xqc_tls_test_reset()` first and uses the T6 2 s deadlines | A succeeds through runtime creation and relay I/O even though `client_default_verify_paths_failures > 0`, proving the default-path return is ignored; B succeeds and records `success_non_leaf_bypass_calls > 0`, `leaf_ok_app_verify_calls == 1`, and no extra Odin verifier call before leaf depth. The environment is restored after each subcase. A context-create path that treats default-path return 0 as fatal, or a success branch that calls the application verifier at non-leaf depth, fails this row | G3, S1 | integration |

## 6. Implementation Plan

- **P1. Land CA-file contract surfaces with red-verifiable tests gated out of the default suite.**
  - **Scope:** add the public `quic_ca_file` and `ca_file` fields in `odin/cli.h`, `odin/cli_client.h`, and `odin/client_xqc_runtime.h`; add test-record fields for CLI runtime CA path, default-create CA path, CA-store allocation/load/free attempts, verifier-user-data capture, runtime-owned store presence, cert verifier success/failure counters, last verifier `X509_STORE_CTX` error, last verifier setup failure kind, and verifier temporary-object allocation/free counters under the existing `ODIN_CLI_CLIENT_TESTING` and target-wide `ODIN_XQC_CLIENT_RUNTIME_TESTING` configs used by `odin_unittests`; add `odin_xqc_client_runtime_test_fail_next_x509_store_new(int errnum)`, `odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t`, and `odin_xqc_client_runtime_test_fail_next_x509_store_ctx(odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t failpoint)` declarations and P1 stubs to `odin/testing/client_xqc_runtime_internal_test.h`, including the invalid-enum `-1/EINVAL` non-arming assertion in T10, with the `X509_STORE_new()` hook gated at `create_default` in P1 and the §3.2.2 store-context verifier setup sites reserved for P2 wiring under `ODIN_XQC_CLIENT_RUNTIME_TESTING`; add the §3.2.3 `build/secondary/xquic/BUILD.gn` test-only `config("xquic_tls_testing_config")` and `component("xquic_testing")`, compile xquic's TLS sources with `XQC_TLS_TESTING` through that target's own `configs`, expose the same define to consumers through its `public_configs`, add `xquic/src/tls/xqc_tls_internal_test.h` with `XQC_EXPORT_PUBLIC_API`-annotated `xqc_tls_test_reset()` and `xqc_tls_test_record()` declarations plus matching `XQC_EXPORT_PUBLIC_API` definitions in `xqc_tls.c`, add non-exported declarations and `xqc_tls.c` definitions for `xqc_tls_test_default_verify_paths_result(int result)` plus the non-leaf bypass and leaf/issuer increment helpers that mutate the file-static record from both TLS translation units, and change `//odin/testing:odin_unittests` to depend on `//xquic:xquic_testing` instead of direct `//xquic` so the test binary links one instrumented xquic object set from the debug shared component; add the test-only fixture script `thor/scripts/issue-ca-file-test-fixtures.sh --force`, which writes `thor/out/root-ca.pem`, `thor/out/odin-server.pem`, `thor/out/odin-server-key.pem`, `thor/out/default-trust-only-root-ca.pem`, `thor/out/default-trust-only-server.pem`, `thor/out/default-trust-only-server-key.pem`, `thor/out/odin-client-auth-only.pem`, `thor/out/odin-client-auth-only-key.pem`, `thor/out/intermediate-ca.pem`, `thor/out/intermediate-ca-key.pem`, `thor/out/intermediate-server.pem`, `thor/out/intermediate-server-chain.pem`, `thor/out/intermediate-server-key.pem`, `thor/out/untrusted-intermediate-ca.pem`, `thor/out/untrusted-intermediate-ca-key.pem`, `thor/out/untrusted-intermediate-server.pem`, and `thor/out/untrusted-intermediate-server-key.pem`; add T1-T15 to `odin/testing/cli_unittests.cpp`, `odin/testing/cli_client_unittests.cpp`, and `odin/testing/client_xqc_runtime_unittests.cpp` behind an `ODIN_CLIENT_CA_FILE_RED=1` red-verification gate or `GTEST_SKIP("pending RFC-029 P2")` in the default run. Do not implement parsing, CA loading, xquic client default-path loading, xquic IP identity selection, xquic success-branch callback dispatch, or certificate verification behavior in this phase.
    The same fixture script also writes `thor/out/cn-only-server.pem` and `thor/out/cn-only-server-key.pem` as a root-signed TLS-server leaf with subject `CN=localhost` and no SAN for T8.
  - **Depends on:** None.
  - **Done when:** the fixture gate `./thor/scripts/issue-ca-file-test-fixtures.sh --force`, `./thor/scripts/verify-server.sh --hosts localhost,127.0.0.1,::1 thor/out/odin-server.pem`, and `./thor/scripts/verify-server.sh --hosts localhost,127.0.0.1,::1 thor/out/intermediate-server-chain.pem` exit zero before any `*CaFile*` run, proving the untracked Thor outputs needed by T1-T15 exist with the deterministic output names, SANs, issuing hierarchy, chain PEM ordering, and EKUs the tests use. The generated key bytes remain untracked Thor outputs. `./tool/gn gen out/client_ca_mac --args='target_os="mac"'`, `./tool/gn gen out/client_ca_mac_arm64 --args='target_os="mac" target_cpu="arm64"'`, `./tool/gn gen out/client_ca_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/gn gen out/client_ca_ios_sim --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'`, and `./tool/gn gen out/client_ca_ios_device --args='target_os="ios" target_environment="device" target_cpu="arm64"'` resolve. `./tool/gn desc out/client_ca_mac //xquic:xquic_testing defines` lists `XQC_TLS_TESTING`, `./tool/gn desc out/client_ca_mac //xquic:xquic_testing sources` lists `xquic/src/tls/xqc_tls.c` and `xquic/src/tls/xqc_tls_ctx.c`, and `./tool/gn desc out/client_ca_mac //odin/testing:odin_unittests deps` shows the test executable link dependency on `//xquic:xquic_testing` rather than direct `//xquic:xquic`; any production `//xquic:xquic` build remains reachable only through `//odin:odin_cli_artifacts` data deps for `odin_main`. `./tool/ninja -C out/client_ca_mac odin_main odin_unittests tests` builds, and `nm -gU out/client_ca_mac/libxquic_testing.dylib | rg ' _?xqc_tls_test_(reset|record)$'` lists externally visible definitions for both test hooks while not listing the non-exported `xqc_tls_test_default_verify_paths_result` or `xqc_tls_test_inc_*` helpers; if either public symbol is hidden or missing, or if the hidden helpers are required by `odin_unittests`, this phase is not done. Matching `odin_main` and `odin_unittests` targets build for the four cross-compile output directories. The host red-verification command `ODIN_CLIENT_CA_FILE_RED=1 out/client_ca_mac/odin_unittests --gtest_filter='*CaFile*'` executes T1-T15 and fails them against the skeleton: T1-T3 because `--ca-file` separate and equals forms are not parsed, forwarded, preserved as omitted, or paired with a successful force-destroy CA-store free; T2 also proves bad-listen and bad-server precedence do not fall through to the CA-file error; T4-T5 because `create_default` ignores `ca_file` and performs no CA-store allocation hook, CA load, normal-destroy CA-store free, post-load constructor cleanup, invalid-hook boundary handling, or rejection; T6-T8 because no default CA verifier, peer-intermediate delivery, or xquic IP/DNS identity branch exists on the production handshake path; T9-T10 because no row-local Odin verifier callback exists to reject malformed inputs, missing callback context, DNS/IP mismatches, unrelated CA, untrusted peer-supplied intermediates, store-context setup failpoints, the invalid-enum non-arming hook contract, intermediate DER exactness failures, or exact positive verifier temporary allocation/free counter deltas before its owning runtime is destroyed; T11 because no xquic IPv6 IP-literal identity branch exists on the production handshake path when IPv6 loopback is available; T12 first links `xqc_tls_test_reset()` and `xqc_tls_test_record()` from `//xquic:xquic_testing`, then fails because xquic does not load client default verify paths from `xqc_tls_ctx.c`, its success branch does not invoke the Odin verifier after default trust accepts the peer, and its fatal default-verifier identity branch is not proven to bypass Odin; T13 because the Odin verifier does not apply `ssl_server` purpose checks; T14 because no row-local Odin verifier callback uses a valid peer-supplied intermediate for path building, proves the loaded store survives CA-path unlink, rejects stale verifier user-data reuse, or records exact positive verifier temporary allocation/free deltas; T15 because xquic lacks the forced default-path result record and non-leaf success bypass counter. T9/T10/T14 assertions compare the exact tuples in §5, so an implementation that wires the verifier statuses but leaves every verifier temporary counter at zero remains red. If the host IPv6 loopback preflight returns `EAFNOSUPPORT` or `EADDRNOTAVAIL`, T11 records a guarded skip and the red command still fails on the remaining red rows. The default host run `out/client_ca_mac/odin_unittests --gtest_brief=1` reports T1-T15 skipped or gated and exits zero with pre-existing tests green. Host-runnable enumeration: T1-T15 run only in `out/client_ca_mac/odin_unittests` on the host macOS architecture, with T11 skipped only when IPv6 loopback is unavailable. Cross-compile-only enumeration: `out/client_ca_mac_arm64/odin_unittests`, `out/client_ca_linux_x64/odin_unittests`, `out/client_ca_ios_sim/odin_unittests`, and `out/client_ca_ios_device/odin_unittests` are built but not executed in this RFC.
    Within that red command, T8's CN-only subcase fails until the Odin verifier rejects a root-signed leaf whose only `localhost` identity is subject CN and no DNS SAN.

- **P2. Implement CA-file parsing, handoff, runtime verification, and xquic verification bridge support.**
  - **Scope:** implement §3.2.1 parser, usage, `odin_cli_main`, and client-runner handoff for separate and equals-form CA-file values; implement §3.2.2 CA-file load, runtime-owned `X509_STORE`, `X509_STORE_new()` failure hook including its non-arming `errnum <= 0` boundary, default cert callback installation, DER chain parser, DNS/IP leaf identity check, `X509_STORE_CTX_new/init/set_default` setup-failure hooks including their invalid-enum non-arming boundary, `X509_STORE_CTX_set_default(ctx, "ssl_server")`, chain verification with peer-supplied intermediates as untrusted chain material, loaded-store verification after CA-path mutation, exact CA-store free counters for normal destroy and force-destroy, verifier temporary-object allocation/free counters at the `X509_free`, `sk_X509_pop_free`, and `X509_STORE_CTX_free` call sites, and test counters; implement §3.2.3 client `SSL_CTX_set_default_verify_paths`, pass its result through `xqc_tls_test_default_verify_paths_result(int result)` under `XQC_TLS_TESTING` and ignore the returned value, implement `xqc_tls_set_verify_identity` with `X509_VERIFY_PARAM_set1_ip_asc` fallback to `X509_VERIFY_PARAM_set1_host`, keep only `xqc_tls_test_reset()` and `xqc_tls_test_record()` exported from `//xquic:xquic_testing`, and update `xqc_ssl_cert_verify_cb` so the application `cert_verify_cb` runs only at leaf-depth success, calls the hidden non-leaf, leaf, and issuer helpers at the pinned `xqc_tls.c` call sites, and records `X509_V_ERR_APPLICATION_VERIFICATION` on application rejection; update existing RFC-028 expectations that currently assert default `cert_verify_flag == 0` for the no-CA path only; remove the `ODIN_CLIENT_CA_FILE_RED` gates and pending skips from T1-T15.
    The DNS verifier implementation keeps the §3.2.2 `X509_CHECK_FLAG_NEVER_CHECK_SUBJECT` check so a configured-CA leaf whose only `localhost` match is subject CN still rejects as an identity mismatch.
  - **Depends on:** P1.
  - **Done when:** the Thor fixture gate from P1 still exits zero. The P1 GN proof commands still show `//xquic:xquic_testing` compiling `xqc_tls.c` and `xqc_tls_ctx.c` with `XQC_TLS_TESTING` and supplying the only xquic link dependency for `odin_unittests`, and the P1 `nm -gU out/client_ca_mac/libxquic_testing.dylib | rg ' _?xqc_tls_test_(reset|record)$'` proof still shows both exported test hooks from the debug xquic testing component without exporting `xqc_tls_test_default_verify_paths_result` or `xqc_tls_test_inc_*` helpers. The `./tool/ninja -C out/client_ca_mac odin_unittests` build compiles the `xqc_tls_ctx.c` call to `xqc_tls_test_default_verify_paths_result()` and links it to the hidden definition in `xqc_tls.c` inside `//xquic:xquic_testing`; a same-file-only or const-accessor-only record contract fails this build before T12/T15 run. The host command `out/client_ca_mac/odin_unittests --gtest_filter='*CaFile*'` passes T1-T15 un-gated, except T11 may report the guarded IPv6-loopback skip only when the preflight returns `EAFNOSUPPORT` or `EADDRNOTAVAIL`; T3 and T4's passing assertions require exactly one CA-store free on successful CLI force-destroy and successful normal destroy; T7's passing assertions require the production server to present `thor/out/intermediate-server-chain.pem`, install SIGPIPE suppression, and validate against only `thor/out/root-ca.pem`; T9 and T14's passing assertions require every direct verifier callback to use matching verifier-user-data from the same live row-local runtime before destroy and to match the exact temporary allocation/free tuples named in §5, including positive leaf, intermediate, stack, and store-context deltas where those objects are owned; T10's passing assertions require row-local runtime ownership for each verifier callback, an invalid store-context failpoint enum to return `-1/EINVAL` without arming, the NEW, INIT, and SET_DEFAULT store-context failpoints to reject before `X509_verify_cert`, and the exact §5 temporary-object deltas after each failpoint and malformed-intermediate exit. An implementation that increments neither allocation nor free counters fails T9/T10/T14 even if all verifier return statuses are correct. T12's passing assertions require subcase A to record `client_default_verify_paths_calls > 0`, `leaf_ok_app_verify_calls > 0`, and `issuer_local_app_verify_calls == 0` from the xquic call sites, and subcase B to record `client_default_verify_paths_calls > 0`, `leaf_ok_app_verify_calls == 0`, `issuer_local_app_verify_calls == 0`, and unchanged Odin verifier counters after a fatal default-verifier identity error; T14's passing assertions require the temp-CA runtime to verify the intermediate chain after the temp CA path is unlinked; T15's passing assertions require SIGPIPE suppression, `client_default_verify_paths_failures > 0` while runtime creation still succeeds, and `success_non_leaf_bypass_calls > 0` with exactly one leaf app call in the intermediate-chain subcase. `out/client_ca_mac/odin_unittests --gtest_brief=1` passes the full local unit suite. The ASan gate `./tool/gn gen out/client_ca_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/client_ca_mac_asan odin_unittests`, and `out/client_ca_mac_asan/odin_unittests --gtest_filter='*CaFile*'` exits without AddressSanitizer reports, backing memory-safety coverage for T3-T15; verifier temporary cleanup is proved by the exact positive counters above rather than by ASan leak detection. The cross-compile targets named in P1 build successfully again but are not executed; their CA-file parser, runtime, xquic TLS, and BoringSSL include paths are compile-verified only.
    T8's passing assertions require `thor/out/cn-only-server.pem` with `server_host == "localhost"` and `ca_file == "thor/out/root-ca.pem"` to fail specifically as Odin DNS identity mismatch, rejecting an implementation that falls back from missing DNS SAN to subject CN.
