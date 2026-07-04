#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CFSSL="$REPO_ROOT/tool/cfssl"
CA_CERT="$REPO_ROOT/thor/out/root-ca.pem"
SERVER_CERT=""
EXPECT_HOSTS=""

usage() {
    cat <<USAGE
usage: $0 [--hosts SAN[,SAN...]] server-cert

options:
  --hosts VALUE   Require these DNS/IP SAN values in the certificate
USAGE
}

json_object_string_field() {
    local object="$1"
    local field="$2"

    printf '%s\n' "$CERT_INFO" |
        sed -n "/\"$object\": {/,/^[[:space:]]*}/s/.*\"$field\": \"\\([^\"]*\\)\".*/\\1/p" |
        sed -n '1p'
}

json_string_field() {
    local field="$1"

    printf '%s\n' "$CERT_INFO" |
        sed -n "s/^[[:space:]]*\"$field\": \"\\([^\"]*\\)\".*/\\1/p" |
        sed -n '1p'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --hosts)
            if [ "$#" -lt 2 ] || [ -z "$2" ]; then
                echo "error: --hosts requires a non-empty value" >&2
                usage >&2
                exit 1
            fi
            EXPECT_HOSTS="$2"
            shift 2
            ;;
        --help)
            usage
            exit 0
            ;;
        -*)
            echo "error: unknown argument '$1'" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [ -n "$SERVER_CERT" ]; then
                echo "error: only one server certificate may be provided" >&2
                usage >&2
                exit 1
            fi
            SERVER_CERT="$1"
            shift
            ;;
    esac
done

if [ -z "$SERVER_CERT" ]; then
    echo "error: server certificate is required" >&2
    usage >&2
    exit 1
fi

if [ ! -f "$CA_CERT" ]; then
    echo "error: $CA_CERT not found" >&2
    echo "       run ./thor/scripts/init-ca.sh first" >&2
    exit 1
fi

if [ ! -f "$SERVER_CERT" ]; then
    echo "error: $SERVER_CERT not found" >&2
    exit 1
fi

if [ ! -x "$CFSSL" ]; then
    echo "error: $CFSSL not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

if ! BUNDLE_INFO="$("$CFSSL" bundle -loglevel=5 -ca-bundle "$CA_CERT" -cert "$SERVER_CERT" 2>&1)"; then
    printf '%s\n' "$BUNDLE_INFO" >&2
    echo "error: server certificate failed CA verification" >&2
    exit 1
fi

if ! CERT_INFO="$("$CFSSL" certinfo -loglevel=5 -cert "$SERVER_CERT" 2>&1)"; then
    printf '%s\n' "$CERT_INFO" >&2
    echo "error: failed to inspect server certificate" >&2
    exit 1
fi

SAN_INFO="$(printf '%s\n' "$CERT_INFO" | sed -n '/"sans": \[/,/\]/p')"

echo "$SERVER_CERT: OK"
echo "subject=CN=$(json_object_string_field subject common_name)"
echo "issuer=CN=$(json_object_string_field issuer common_name)"
echo "notBefore=$(json_string_field not_before)"
echo "notAfter=$(json_string_field not_after)"

if [ -n "$EXPECT_HOSTS" ]; then
    old_ifs="$IFS"
    IFS=","
    set -- $EXPECT_HOSTS
    IFS="$old_ifs"
    for host in "$@"; do
        if ! printf '%s\n' "$SAN_INFO" | grep -Fq "\"$host\""; then
            echo "error: server certificate is missing SAN $host" >&2
            exit 1
        fi
    done
fi

echo "server certificate verified"
