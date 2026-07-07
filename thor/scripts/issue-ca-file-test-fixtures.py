#!/usr/bin/env python3
"""Generate the compact Odin TLS test fixture set under a GN build directory."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT_DIR = REPO_ROOT / "out" / "gen" / "thor" / "odin_test_certs"


def run(cmd):
    subprocess.run(
        cmd,
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def unlink_if_exists(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def write_req_cfg(path, cn, san, eku):
    path.write_text(
        f"""[req]
distinguished_name = dn
prompt = no

[dn]
CN = {cn}

[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer

[v3_leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = {eku}
subjectAltName = {san}
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer

[v3_leaf_no_san]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = {eku}
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
""",
        encoding="utf-8",
    )


def make_key(key):
    run(["openssl", "genrsa", "-out", os.fspath(key), "2048"])
    key.chmod(0o600)


def make_ca(tmp_dir, cert, cn, key, issuer_cert=None, issuer_key=None):
    base = cert.stem
    cfg = tmp_dir / f"{base}.cnf"
    csr = tmp_dir / f"{base}.csr"
    write_req_cfg(cfg, cn, "DNS:unused", "serverAuth")
    if issuer_cert is None:
        run(
            [
                "openssl",
                "req",
                "-x509",
                "-new",
                "-key",
                os.fspath(key),
                "-sha256",
                "-days",
                "365",
                "-subj",
                f"/CN={cn}",
                "-out",
                os.fspath(cert),
                "-config",
                os.fspath(cfg),
                "-extensions",
                "v3_ca",
            ]
        )
        return

    run(
        [
            "openssl",
            "req",
            "-new",
            "-key",
            os.fspath(key),
            "-subj",
            f"/CN={cn}",
            "-out",
            os.fspath(csr),
            "-config",
            os.fspath(cfg),
        ]
    )
    run(
        [
            "openssl",
            "x509",
            "-req",
            "-in",
            os.fspath(csr),
            "-CA",
            os.fspath(issuer_cert),
            "-CAkey",
            os.fspath(issuer_key),
            "-CAcreateserial",
            "-CAserial",
            os.fspath(tmp_dir / f"{base}.srl"),
            "-out",
            os.fspath(cert),
            "-days",
            "365",
            "-sha256",
            "-extfile",
            os.fspath(cfg),
            "-extensions",
            "v3_ca",
        ]
    )


def make_leaf(
    tmp_dir,
    cert,
    cn,
    san,
    eku,
    issuer_cert,
    issuer_key,
    key,
    ext="v3_leaf",
):
    base = cert.stem
    cfg = tmp_dir / f"{base}.cnf"
    csr = tmp_dir / f"{base}.csr"
    write_req_cfg(cfg, cn, san, eku)
    run(
        [
            "openssl",
            "req",
            "-new",
            "-key",
            os.fspath(key),
            "-subj",
            f"/CN={cn}",
            "-out",
            os.fspath(csr),
            "-config",
            os.fspath(cfg),
        ]
    )
    run(
        [
            "openssl",
            "x509",
            "-req",
            "-in",
            os.fspath(csr),
            "-CA",
            os.fspath(issuer_cert),
            "-CAkey",
            os.fspath(issuer_key),
            "-CAcreateserial",
            "-CAserial",
            os.fspath(tmp_dir / f"{base}.srl"),
            "-out",
            os.fspath(cert),
            "-days",
            "365",
            "-sha256",
            "-extfile",
            os.fspath(cfg),
            "-extensions",
            ext,
        ]
    )


def main():
    parser = argparse.ArgumentParser(
        description="Generate the compact Odin TLS test fixture set.",
    )
    parser.add_argument(
        "--out-dir",
        default=os.fspath(DEFAULT_OUT_DIR),
        help="Directory for generated PEM fixtures.",
    )
    parser.add_argument("--stamp", help="Optional GN stamp file to write.")
    args = parser.parse_args()

    if shutil.which("openssl") is None:
        sys.stderr.write("error: openssl not found\n")
        return 1

    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = Path.cwd() / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    root_ca = out_dir / "root-ca.pem"
    root_key = out_dir / "root-ca-key.pem"
    leaf_key = out_dir / "odin-test-leaf-key.pem"

    generated_files = [
        root_ca,
        root_key,
        out_dir / "root-ca.csr",
        leaf_key,
        out_dir / "odin-server.pem",
        out_dir / "odin-server-key.pem",
        out_dir / "odin-server.csr",
        out_dir / "default-trust-only-root-ca.pem",
        out_dir / "default-trust-only-root-ca-key.pem",
        out_dir / "default-trust-only-server.pem",
        out_dir / "default-trust-only-server-key.pem",
        out_dir / "intermediate-ca.pem",
        out_dir / "intermediate-ca-key.pem",
        out_dir / "intermediate-server.pem",
        out_dir / "intermediate-server-key.pem",
        out_dir / "intermediate-server-chain.pem",
        out_dir / "untrusted-intermediate-ca.pem",
        out_dir / "untrusted-intermediate-ca-key.pem",
        out_dir / "untrusted-intermediate-server.pem",
        out_dir / "untrusted-intermediate-server-key.pem",
        out_dir / "untrusted-intermediate-server-chain.pem",
        out_dir / "cn-only-server.pem",
        out_dir / "cn-only-server-key.pem",
        out_dir / "odin-client-auth-only.pem",
        out_dir / "odin-client-auth-only-key.pem",
    ]
    for path in generated_files:
        unlink_if_exists(path)

    with tempfile.TemporaryDirectory(prefix="odin-ca-file-fixtures.") as tmp:
        tmp_dir = Path(tmp)
        make_key(root_key)
        make_key(leaf_key)

        make_ca(tmp_dir, root_ca, "Odin Test Root CA", root_key)

        make_leaf(
            tmp_dir,
            out_dir / "odin-server.pem",
            "odin-server",
            "DNS:localhost,IP:127.0.0.1,IP:::1",
            "serverAuth",
            root_ca,
            root_key,
            leaf_key,
        )

        intermediate_ca = tmp_dir / "intermediate-ca.pem"
        intermediate_leaf = tmp_dir / "intermediate-server.pem"
        make_ca(
            tmp_dir,
            intermediate_ca,
            "Odin Test Intermediate CA",
            root_key,
            root_ca,
            root_key,
        )
        make_leaf(
            tmp_dir,
            intermediate_leaf,
            "intermediate-server",
            "DNS:localhost,IP:127.0.0.1,IP:::1",
            "serverAuth",
            intermediate_ca,
            root_key,
            leaf_key,
        )
        (out_dir / "intermediate-server-chain.pem").write_bytes(
            intermediate_leaf.read_bytes() + intermediate_ca.read_bytes()
        )

        untrusted_ca = tmp_dir / "untrusted-intermediate-ca.pem"
        untrusted_leaf = tmp_dir / "untrusted-intermediate-server.pem"
        make_ca(
            tmp_dir,
            untrusted_ca,
            "Odin Test Untrusted Intermediate CA",
            leaf_key,
        )
        make_leaf(
            tmp_dir,
            untrusted_leaf,
            "untrusted-intermediate-server",
            "DNS:localhost,IP:127.0.0.1",
            "serverAuth",
            untrusted_ca,
            leaf_key,
            leaf_key,
        )
        (out_dir / "untrusted-intermediate-server-chain.pem").write_bytes(
            untrusted_leaf.read_bytes() + untrusted_ca.read_bytes()
        )

        make_leaf(
            tmp_dir,
            out_dir / "cn-only-server.pem",
            "localhost",
            "DNS:unused",
            "serverAuth",
            root_ca,
            root_key,
            leaf_key,
            "v3_leaf_no_san",
        )

        make_leaf(
            tmp_dir,
            out_dir / "odin-client-auth-only.pem",
            "odin-client-auth-only",
            "DNS:localhost,IP:127.0.0.1",
            "clientAuth",
            root_ca,
            root_key,
            leaf_key,
        )

    if args.stamp:
        stamp = Path(args.stamp)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text("ok\n", encoding="utf-8")

    print(f"wrote compact Odin TLS test fixtures under {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
