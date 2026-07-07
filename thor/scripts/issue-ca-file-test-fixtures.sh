#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$REPO_ROOT/thor/out"

FORCE=0

usage() {
    cat <<USAGE
usage: $0 [--force]
USAGE
}

if [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

if [ "${1:-}" = "--force" ]; then
    FORCE=1
    shift
fi

if [ "$#" -gt 0 ]; then
    echo "error: unknown argument '$1'" >&2
    usage >&2
    exit 1
fi

if ! command -v openssl >/dev/null 2>&1; then
    echo "error: openssl not found" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

if [ "$FORCE" -eq 1 ]; then
    "$REPO_ROOT/thor/scripts/init-ca.sh" --force
else
    "$REPO_ROOT/thor/scripts/init-ca.sh"
fi

"$REPO_ROOT/thor/scripts/issue-server.sh" \
    --name odin-server \
    --cn odin-server \
    --hosts localhost,127.0.0.1,::1 \
    --force

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/odin-ca-file-fixtures.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

ROOT_CA="$OUT_DIR/root-ca.pem"
ROOT_KEY="$OUT_DIR/root-ca-key.pem"

write_req_cfg() {
    local path="$1"
    local cn="$2"
    local san="$3"
    local eku="$4"
    local ca="$5"

    cat >"$path" <<CFG
[req]
distinguished_name = dn
prompt = no

[dn]
CN = $cn

[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer

[v3_leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = $eku
subjectAltName = $san
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer

[v3_leaf_no_san]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = $eku
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
CFG
    if [ "$ca" = "1" ]; then
        :
    fi
}

make_ca() {
    local name="$1"
    local cn="$2"
    local issuer_cert="${3:-}"
    local issuer_key="${4:-}"
    local key="$OUT_DIR/$name-key.pem"
    local cert="$OUT_DIR/$name.pem"
    local cfg="$TMP_DIR/$name.cnf"
    local csr="$TMP_DIR/$name.csr"

    write_req_cfg "$cfg" "$cn" "DNS:unused" "serverAuth" "1"
    openssl genrsa -out "$key" 2048 >/dev/null 2>&1
    chmod 600 "$key"
    if [ -z "$issuer_cert" ]; then
        openssl req -x509 -new -key "$key" -sha256 -days 365 \
            -subj "/CN=$cn" -out "$cert" -config "$cfg" -extensions v3_ca
    else
        openssl req -new -key "$key" -subj "/CN=$cn" -out "$csr" \
            -config "$cfg"
        openssl x509 -req -in "$csr" -CA "$issuer_cert" -CAkey "$issuer_key" \
            -CAcreateserial -CAserial "$TMP_DIR/$name.srl" -out "$cert" \
            -days 365 -sha256 -extfile "$cfg" -extensions v3_ca \
            >/dev/null 2>&1
    fi
}

make_leaf() {
    local name="$1"
    local cn="$2"
    local san="$3"
    local eku="$4"
    local issuer_cert="$5"
    local issuer_key="$6"
    local ext="${7:-v3_leaf}"
    local key="$OUT_DIR/$name-key.pem"
    local cert="$OUT_DIR/$name.pem"
    local cfg="$TMP_DIR/$name.cnf"
    local csr="$TMP_DIR/$name.csr"

    write_req_cfg "$cfg" "$cn" "$san" "$eku" "0"
    openssl genrsa -out "$key" 2048 >/dev/null 2>&1
    chmod 600 "$key"
    openssl req -new -key "$key" -subj "/CN=$cn" -out "$csr" \
        -config "$cfg"
    openssl x509 -req -in "$csr" -CA "$issuer_cert" -CAkey "$issuer_key" \
        -CAcreateserial -CAserial "$TMP_DIR/$name.srl" -out "$cert" \
        -days 365 -sha256 -extfile "$cfg" -extensions "$ext" \
        >/dev/null 2>&1
}

make_ca "default-trust-only-root-ca" "Odin RFC029 Default Trust Only Root"
make_leaf "default-trust-only-server" "default-trust-only-server" \
    "DNS:localhost,IP:127.0.0.1" "serverAuth" \
    "$OUT_DIR/default-trust-only-root-ca.pem" \
    "$OUT_DIR/default-trust-only-root-ca-key.pem"

make_leaf "odin-client-auth-only" "odin-client-auth-only" \
    "DNS:localhost,IP:127.0.0.1" "clientAuth" "$ROOT_CA" "$ROOT_KEY"

make_ca "intermediate-ca" "Odin RFC029 Intermediate CA" "$ROOT_CA" "$ROOT_KEY"
make_leaf "intermediate-server" "intermediate-server" \
    "DNS:localhost,IP:127.0.0.1,IP:::1" "serverAuth" \
    "$OUT_DIR/intermediate-ca.pem" "$OUT_DIR/intermediate-ca-key.pem"
cat "$OUT_DIR/intermediate-server.pem" "$OUT_DIR/intermediate-ca.pem" \
    >"$OUT_DIR/intermediate-server-chain.pem"

make_ca "untrusted-intermediate-ca" "Odin RFC029 Untrusted Intermediate CA"
make_leaf "untrusted-intermediate-server" "untrusted-intermediate-server" \
    "DNS:localhost,IP:127.0.0.1" "serverAuth" \
    "$OUT_DIR/untrusted-intermediate-ca.pem" \
    "$OUT_DIR/untrusted-intermediate-ca-key.pem"

make_leaf "cn-only-server" "localhost" "DNS:unused" "serverAuth" \
    "$ROOT_CA" "$ROOT_KEY" "v3_leaf_no_san"

echo "wrote RFC-029 CA-file fixtures under thor/out"
