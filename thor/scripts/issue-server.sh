#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CFSSL="$REPO_ROOT/tool/cfssl"
CFSSLJSON="$REPO_ROOT/tool/cfssljson"
CONFIG="$REPO_ROOT/thor/cfssl/ca-config.json"
PRESET_DIR="$REPO_ROOT/thor/presets"
OUT_DIR="$REPO_ROOT/thor/out"
CA_CERT="$OUT_DIR/root-ca.pem"
CA_KEY="$OUT_DIR/root-ca-key.pem"

NAME=""
CN=""
HOSTS=""
ORG="Thor"
ORG_UNIT="Server"
COUNTRY="US"
STATE="Local"
LOCALITY="Local"
FORCE=0

usage() {
    cat <<USAGE
usage: $0 --name NAME --cn COMMON-NAME --hosts SAN[,SAN...] [options]

options:
  --preset NAME       Load defaults from thor/presets/NAME.env
  --org VALUE         Subject organization. Default: Thor
  --ou VALUE          Subject organizational unit. Default: Server
  --country VALUE     Subject country. Default: US
  --state VALUE       Subject state. Default: Local
  --locality VALUE    Subject locality. Default: Local
  --force             Replace existing thor/out/NAME.pem and key
USAGE
}

load_preset() {
    local preset="$1"
    local preset_file="$PRESET_DIR/$preset.env"
    if [ ! -f "$preset_file" ]; then
        echo "error: preset '$preset' not found at $preset_file" >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$preset_file"
    NAME="${THOR_SERVER_NAME:-$NAME}"
    CN="${THOR_SERVER_CN:-$CN}"
    HOSTS="${THOR_SERVER_HOSTS:-$HOSTS}"
    ORG="${THOR_SERVER_ORG:-$ORG}"
    ORG_UNIT="${THOR_SERVER_OU:-$ORG_UNIT}"
    COUNTRY="${THOR_SERVER_COUNTRY:-$COUNTRY}"
    STATE="${THOR_SERVER_STATE:-$STATE}"
    LOCALITY="${THOR_SERVER_LOCALITY:-$LOCALITY}"
}

require_option_value() {
    local option="$1"
    local value="${2:-}"
    if [ -z "$value" ]; then
        echo "error: $option requires a non-empty value" >&2
        usage >&2
        exit 1
    fi
}

require_safe_value() {
    local label="$1"
    local value="$2"
    case "$value" in
        *[!A-Za-z0-9.,:_@%+=\/\ -]*)
            echo "error: $label contains unsupported characters" >&2
            exit 1
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --preset)
            require_option_value "--preset" "${2:-}"
            load_preset "$2"
            shift 2
            ;;
        --name)
            require_option_value "--name" "${2:-}"
            NAME="$2"
            shift 2
            ;;
        --cn)
            require_option_value "--cn" "${2:-}"
            CN="$2"
            shift 2
            ;;
        --hosts)
            require_option_value "--hosts" "${2:-}"
            HOSTS="$2"
            shift 2
            ;;
        --org)
            require_option_value "--org" "${2:-}"
            ORG="$2"
            shift 2
            ;;
        --ou)
            require_option_value "--ou" "${2:-}"
            ORG_UNIT="$2"
            shift 2
            ;;
        --country)
            require_option_value "--country" "${2:-}"
            COUNTRY="$2"
            shift 2
            ;;
        --state)
            require_option_value "--state" "${2:-}"
            STATE="$2"
            shift 2
            ;;
        --locality)
            require_option_value "--locality" "${2:-}"
            LOCALITY="$2"
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [ -z "$NAME" ] || [ -z "$CN" ] || [ -z "$HOSTS" ]; then
    echo "error: --name, --cn, and --hosts are required" >&2
    usage >&2
    exit 1
fi

require_safe_value "--name" "$NAME"
require_safe_value "--cn" "$CN"
require_safe_value "--hosts" "$HOSTS"
require_safe_value "--org" "$ORG"
require_safe_value "--ou" "$ORG_UNIT"
require_safe_value "--country" "$COUNTRY"
require_safe_value "--state" "$STATE"
require_safe_value "--locality" "$LOCALITY"

case "$NAME" in
    */*|.*|*..*)
        echo "error: --name must be a simple output basename" >&2
        exit 1
        ;;
esac

if [ ! -x "$CFSSL" ]; then
    echo "error: $CFSSL not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

if [ ! -x "$CFSSLJSON" ]; then
    echo "error: $CFSSLJSON not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "error: thor cfssl configuration is incomplete" >&2
    exit 1
fi

if [ ! -f "$CA_CERT" ] || [ ! -f "$CA_KEY" ]; then
    echo "error: root CA not found under thor/out" >&2
    echo "       run ./thor/scripts/init-ca.sh first" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

OUT_PREFIX="$OUT_DIR/$NAME"
if [ "$FORCE" -eq 0 ] && { [ -e "$OUT_PREFIX.pem" ] || [ -e "$OUT_PREFIX-key.pem" ]; }; then
    echo "server certificate already exists under thor/out: $NAME"
    echo "use $0 --force to replace it"
    exit 0
fi

if [ "$FORCE" -eq 1 ]; then
    rm -f "$OUT_PREFIX.pem" "$OUT_PREFIX-key.pem" "$OUT_PREFIX.csr"
fi

TMP_CSR="$(mktemp "${TMPDIR:-/tmp}/thor-server-csr.XXXXXX")"
trap 'rm -f "$TMP_CSR"' EXIT

cat >"$TMP_CSR" <<JSON
{
  "CN": "$CN",
  "key": {
    "algo": "rsa",
    "size": 2048
  },
  "names": [
    {
      "C": "$COUNTRY",
      "L": "$LOCALITY",
      "O": "$ORG",
      "OU": "$ORG_UNIT",
      "ST": "$STATE"
    }
  ]
}
JSON

"$CFSSL" gencert \
    -ca "$CA_CERT" \
    -ca-key "$CA_KEY" \
    -config "$CONFIG" \
    -profile server \
    -hostname "$HOSTS" \
    "$TMP_CSR" | "$CFSSLJSON" -bare "$OUT_PREFIX"

chmod 600 "$OUT_PREFIX-key.pem"

echo "wrote thor/out/$NAME.pem"
echo "wrote thor/out/$NAME-key.pem"
