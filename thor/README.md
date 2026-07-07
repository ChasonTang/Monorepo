# Thor

Thor is the repository-local private CA and PKI workspace. It is intended to
own private trust roots and issue leaf certificates for any local component
that needs repository-managed TLS, not only Odin.

Thor uses the checked-in `tool/cfssl` and `tool/cfssljson` binaries to create a
private root CA and sign leaf certificates. Odin QUIC is the first consumer and
currently has a ready-made server certificate preset.

Generated certificates and keys live under `thor/out/`. That directory is
ignored except for its `.gitignore`; do not commit private keys.

## Layout

```text
thor/
  cfssl/
    ca-config.json
    root-ca-csr.json
  presets/
    odin-server.env
  scripts/
    init-ca.sh
    issue-server.sh
    issue-ca-file-test-fixtures.py
    verify-server.sh
  out/
    .gitignore
```

## Generate A Private CA

From the repository root:

```bash
./thor/scripts/init-ca.sh
```

This writes the local root certificate and key under `thor/out/`:

- `thor/out/root-ca.pem`
- `thor/out/root-ca-key.pem`

## Issue An Odin Server Certificate

Odin is a current Thor consumer represented as a server certificate preset.
From the repository root:

```bash
./thor/scripts/issue-server.sh --preset odin-server
./thor/scripts/verify-server.sh \
  --hosts localhost,127.0.0.1,::1 \
  thor/out/odin-server.pem
```

The default Odin server certificate is issued for:

- `localhost`
- `127.0.0.1`
- `::1`

Use a custom SAN list when issuing:

```bash
./thor/scripts/issue-server.sh \
  --preset odin-server \
  --hosts localhost,127.0.0.1,odin.local
```

Use the generated Odin leaf certificate with Odin server:

```bash
odin-server --listen 9443 \
  --quic-cert thor/out/odin-server.pem \
  --quic-key thor/out/odin-server-key.pem
```

## Odin Test Fixtures

Odin unit and integration tests use a compact fixture set generated under the
GN build directory:

```bash
./tool/ninja -C out tests
```

That build invokes `//thor:odin_test_certs`, which runs the Python generator
`thor/scripts/issue-ca-file-test-fixtures.py` and refreshes only the files the
tests need.

For the default `out` build, the files are written under
`out/gen/thor/odin_test_certs/`:

- `root-ca.pem`
- `root-ca-key.pem`
- `odin-test-leaf-key.pem`
- `odin-server.pem`
- `intermediate-server-chain.pem`
- `untrusted-intermediate-server-chain.pem`
- `cn-only-server.pem`
- `odin-client-auth-only.pem`

The test server leaves intentionally share `odin-test-leaf-key.pem`; their
separate certificate files carry the SAN, EKU, and chain differences under
test. For manual generation, pass `--out-dir <build-dir>/gen/thor/odin_test_certs`
to the Python script.

## Issue A Generic Server Certificate

Use the generic server signing profile for non-Odin consumers:

```bash
./thor/scripts/issue-server.sh \
  --name example-server \
  --cn example.local \
  --hosts example.local,127.0.0.1 \
  --org Example \
  --ou ExampleService

./thor/scripts/verify-server.sh \
  --hosts example.local,127.0.0.1 \
  thor/out/example-server.pem
```

## Consumer Trust

Thor creates `thor/out/root-ca.pem`, which is the CA certificate consumers
should trust when they accept certificates issued by this private CA.

For Odin specifically, the current QUIC client default path does not yet expose
a CA file or certificate-chain verification option. Wiring that in should set
`XQC_TLS_CERT_FLAG_NEED_VERIFY` and provide an xquic `cert_verify_cb` that
validates the server chain against `thor/out/root-ca.pem`.
