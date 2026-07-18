#!/usr/bin/env python3
"""Generate WebAuthn ceremony test vectors for the identity stack.

Writes two generated fixtures (do not edit them by hand; rerun this):

  examples/webauthn-app/sys/vectors.h        LPC defines (hex strings)
  scripts/verbsets/webauthn-ceremony.verbset console ceremony drive

The signatures come from an independent implementation -- OpenSSL via
the python 'cryptography' package -- so the platform's verify kfuns and
ceremony parsing are checked against foreign-generated payloads, not
against bytes the LPC stack produced itself. ECDSA signatures are
randomized, so regenerated files differ byte-for-byte but stay valid;
the structures (rpId, origin, challenges, credential ids, counters) are
fixed below and the LPC drivers assert against those.

Run from the repository root:

  python3 scripts/gen-webauthn-vectors.py

Needs python3 with the 'cryptography' package.
"""

import base64
import hashlib
import json
import pathlib
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, ed25519
from cryptography.hazmat.primitives import serialization

RP_ID = "localhost"
ORIGIN = "https://localhost:8443"
BAD_ORIGIN = "https://attacker.example"
BAD_RP_ID = "attacker.example"

ROOT = pathlib.Path(__file__).resolve().parent.parent
VECTORS_H = ROOT / "examples/webauthn-app/sys/vectors.h"
VERBSET = ROOT / "scripts/verbsets/webauthn-ceremony.verbset"
RECOVERY_VERBSET = ROOT / "scripts/verbsets/identity-recovery.verbset"


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


# --- minimal canonical CBOR encoder (the subset these structures use) ---

def cbor_head(major: int, arg: int) -> bytes:
    if arg < 24:
        return bytes([(major << 5) | arg])
    if arg < 0x100:
        return bytes([(major << 5) | 24, arg])
    if arg < 0x10000:
        return bytes([(major << 5) | 25]) + struct.pack(">H", arg)
    return bytes([(major << 5) | 26]) + struct.pack(">I", arg)


def cbor(value) -> bytes:
    if isinstance(value, int):
        if value >= 0:
            return cbor_head(0, value)
        return cbor_head(1, -1 - value)
    if isinstance(value, bytes):
        return cbor_head(2, len(value)) + value
    if isinstance(value, str):
        raw = value.encode()
        return cbor_head(3, len(raw)) + raw
    if isinstance(value, dict):
        def keybytes(k):
            return cbor(k)
        items = sorted(value.items(),
                       key=lambda kv: (len(keybytes(kv[0])), keybytes(kv[0])))
        out = cbor_head(5, len(value))
        for k, v in items:
            out += cbor(k) + cbor(v)
        return out
    raise TypeError(type(value))


# --- WebAuthn structures ---

def client_data(typ: str, challenge_b64u: str, origin: str) -> bytes:
    return json.dumps({"type": typ, "challenge": challenge_b64u,
                       "origin": origin, "crossOrigin": False},
                      separators=(",", ":")).encode()


def auth_data(rp_id: str, flags: int, count: int, att: bytes = b"") -> bytes:
    return (hashlib.sha256(rp_id.encode()).digest() + bytes([flags]) +
            struct.pack(">I", count) + att)


def attestation_object(ad: bytes) -> bytes:
    return cbor({"fmt": "none", "attStmt": {}, "authData": ad})


def attested_credential(cred_id: bytes, cose_key: dict) -> bytes:
    return (b"\x00" * 16 + struct.pack(">H", len(cred_id)) + cred_id +
            cbor(cose_key))


UP, UV, AT = 0x01, 0x04, 0x40

# --- ES256 credential ---

es_key = ec.generate_private_key(ec.SECP256R1())
es_nums = es_key.public_key().public_numbers()
es_x = es_nums.x.to_bytes(32, "big")
es_y = es_nums.y.to_bytes(32, "big")
es_cred_id = bytes(range(0x10, 0x20))
es_cose = {1: 2, 3: -7, -1: 1, -2: es_x, -3: es_y}

attacker_key = ec.generate_private_key(ec.SECP256R1())

CH_REG = b64u(bytes(range(32)))
CH_A1 = b64u(bytes(range(32, 64)))
CH_A2 = b64u(bytes(range(64, 96)))

reg_ad = auth_data(RP_ID, UP | AT, 5,
                   attested_credential(es_cred_id, es_cose))
reg_ao = attestation_object(reg_ad)
reg_cdj = client_data("webauthn.create", CH_REG, ORIGIN)

reg_cdj_bad_origin = client_data("webauthn.create", CH_REG, BAD_ORIGIN)
reg_cdj_bad_type = client_data("webauthn.get", CH_REG, ORIGIN)
reg_ao_bad_rp = attestation_object(
    auth_data(BAD_RP_ID, UP | AT, 5,
              attested_credential(es_cred_id, es_cose)))
reg_ao_bad_fmt = cbor({"fmt": "packed", "attStmt": {}, "authData": reg_ad})


def es_sign(ad: bytes, cdj: bytes) -> bytes:
    return es_key.sign(ad + hashlib.sha256(cdj).digest(),
                       ec.ECDSA(hashes.SHA256()))


a1_ad = auth_data(RP_ID, UP, 6)
a1_cdj = client_data("webauthn.get", CH_A1, ORIGIN)
a1_sig = es_sign(a1_ad, a1_cdj)

a_bad_origin_cdj = client_data("webauthn.get", CH_A2, BAD_ORIGIN)
a_bad_origin_sig = es_sign(a1_ad, a_bad_origin_cdj)

a_bad_rp_ad = auth_data(BAD_RP_ID, UP, 7)
a_bad_rp_cdj = client_data("webauthn.get", CH_A2, ORIGIN)
a_bad_rp_sig = es_sign(a_bad_rp_ad, a_bad_rp_cdj)

a_bad_sig = bytearray(a1_sig)
a_bad_sig[-1] ^= 0x01
a_bad_sig = bytes(a_bad_sig)

a_wrong_key_sig = attacker_key.sign(
    a1_ad + hashlib.sha256(a1_cdj).digest(), ec.ECDSA(hashes.SHA256()))

# --- Ed25519 credential ---

ed_key = ed25519.Ed25519PrivateKey.generate()
ed_pub = ed_key.public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw)
ed_cred_id = bytes(range(0x30, 0x40))
ed_cose = {1: 1, 3: -8, -1: 6, -2: ed_pub}

CH_REG2 = b64u(bytes(range(96, 128)))
CH_A3 = b64u(bytes(range(128, 160)))

ed_reg_ad = auth_data(RP_ID, UP | AT, 2,
                      attested_credential(ed_cred_id, ed_cose))
ed_reg_ao = attestation_object(ed_reg_ad)
ed_reg_cdj = client_data("webauthn.create", CH_REG2, ORIGIN)

ed_a_ad = auth_data(RP_ID, UP, 3)
ed_a_cdj = client_data("webauthn.get", CH_A3, ORIGIN)
ed_a_sig = ed_key.sign(ed_a_ad + hashlib.sha256(ed_a_cdj).digest())

# --- ES256 credential #2: the recovery ceremony's replacement passkey ---

es2_key = ec.generate_private_key(ec.SECP256R1())
es2_nums = es2_key.public_key().public_numbers()
es2_cose = {1: 2, 3: -7, -1: 1,
            -2: es2_nums.x.to_bytes(32, "big"),
            -3: es2_nums.y.to_bytes(32, "big")}
es2_cred_id = bytes(range(0x50, 0x60))

CH_REG3 = b64u(bytes(range(160, 192)))
CH_A4 = b64u(bytes(range(192, 224)))

reg3_ad = auth_data(RP_ID, UP | AT, 9,
                    attested_credential(es2_cred_id, es2_cose))
reg3_ao = attestation_object(reg3_ad)
reg3_cdj = client_data("webauthn.create", CH_REG3, ORIGIN)

a4_ad = auth_data(RP_ID, UP, 10)
a4_cdj = client_data("webauthn.get", CH_A4, ORIGIN)
a4_sig = es2_key.sign(a4_ad + hashlib.sha256(a4_cdj).digest(),
                      ec.ECDSA(hashes.SHA256()))

# --- ES256 credential #3: the operator console-bind vector ---

es3_key = ec.generate_private_key(ec.SECP256R1())
es3_nums = es3_key.public_key().public_numbers()
es3_cose = {1: 2, 3: -7, -1: 1,
            -2: es3_nums.x.to_bytes(32, "big"),
            -3: es3_nums.y.to_bytes(32, "big")}
es3_cred_id = bytes(range(0x60, 0x70))

CH_REG4 = b64u(bytes(range(224, 256)))

reg4_ad = auth_data(RP_ID, UP | AT, 11,
                    attested_credential(es3_cred_id, es3_cose))
reg4_ao = attestation_object(reg4_ad)
reg4_cdj = client_data("webauthn.create", CH_REG4, ORIGIN)


# --- emit the LPC fixture ---

defines = [
    ("WA_RP_ID", RP_ID, "s"), ("WA_ORIGIN", ORIGIN, "s"),
    ("WA_BAD_ORIGIN", BAD_ORIGIN, "s"),
    ("WA_CH_REG", CH_REG, "s"), ("WA_CH_A1", CH_A1, "s"),
    ("WA_CH_A2", CH_A2, "s"), ("WA_CH_REG2", CH_REG2, "s"),
    ("WA_CH_A3", CH_A3, "s"),
    ("WA_REG_CDJ", reg_cdj, "x"), ("WA_REG_AO", reg_ao, "x"),
    ("WA_REG_CDJ_BAD_ORIGIN", reg_cdj_bad_origin, "x"),
    ("WA_REG_CDJ_BAD_TYPE", reg_cdj_bad_type, "x"),
    ("WA_REG_AO_BAD_RP", reg_ao_bad_rp, "x"),
    ("WA_REG_AO_BAD_FMT", reg_ao_bad_fmt, "x"),
    ("WA_ES_CRED_ID", es_cred_id, "x"),
    ("WA_A1_AD", a1_ad, "x"), ("WA_A1_CDJ", a1_cdj, "x"),
    ("WA_A1_SIG", a1_sig, "x"),
    ("WA_A_BAD_ORIGIN_CDJ", a_bad_origin_cdj, "x"),
    ("WA_A_BAD_ORIGIN_SIG", a_bad_origin_sig, "x"),
    ("WA_A_BAD_RP_AD", a_bad_rp_ad, "x"),
    ("WA_A_BAD_RP_CDJ", a_bad_rp_cdj, "x"),
    ("WA_A_BAD_RP_SIG", a_bad_rp_sig, "x"),
    ("WA_A_BAD_SIG", a_bad_sig, "x"),
    ("WA_A_WRONG_KEY_SIG", a_wrong_key_sig, "x"),
    ("WA_ED_REG_CDJ", ed_reg_cdj, "x"), ("WA_ED_REG_AO", ed_reg_ao, "x"),
    ("WA_ED_CRED_ID", ed_cred_id, "x"),
    ("WA_ED_A_AD", ed_a_ad, "x"), ("WA_ED_A_CDJ", ed_a_cdj, "x"),
    ("WA_ED_A_SIG", ed_a_sig, "x"),
    ("WA_CH_REG3", CH_REG3, "s"), ("WA_CH_A4", CH_A4, "s"),
    ("WA_CH_REG4", CH_REG4, "s"),
    ("WA_REG3_CDJ", reg3_cdj, "x"), ("WA_REG3_AO", reg3_ao, "x"),
    ("WA_ES2_CRED_ID", es2_cred_id, "x"),
    ("WA_A4_AD", a4_ad, "x"), ("WA_A4_CDJ", a4_cdj, "x"),
    ("WA_A4_SIG", a4_sig, "x"),
    ("WA_REG4_CDJ", reg4_cdj, "x"), ("WA_REG4_AO", reg4_ao, "x"),
    ("WA_ES3_CRED_ID", es3_cred_id, "x"),
]

lines = ["/*",
         " * Generated by scripts/gen-webauthn-vectors.py -- do not edit.",
         " * WebAuthn ceremony vectors; signatures produced by OpenSSL via",
         " * the python cryptography package (an independent implementation).",
         " * _HEX-suffixed values decode with /lib/util/hex decodeString.",
         " */", ""]
for name, value, kind in defines:
    if kind == "s":
        lines.append(f'# define {name}\t"{value}"')
    else:
        lines.append(f'# define {name}_HEX\t"{value.hex()}"')
lines.append("")
VECTORS_H.write_text("\n".join(lines))

# --- emit the console ceremony verbset ---

vs = f"""# webauthn operator verb -- the ceremony daemon driven end-to-end on a
# module-bearing boot with foreign-generated vectors (regenerate with
# scripts/gen-webauthn-vectors.py; do not edit by hand):
#   LPC_EXT_CRYPTO=<crypto> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh \\
#       scripts/verbsets/webauthn-ceremony.verbset
# Covers: rp configuration, TOFU registration minting an identity,
# assertion verification updating signCount, the signCount replay
# refusal, re-registration of a bound credential refused, an unknown
# credential refused, and the System-tier gate refusing a console
# caller.

# point the ceremony daemon at the vectors' rp
cmd: webauthn rpid {RP_ID}
expect: webauthn: rpId {RP_ID}

cmd: webauthn origin {ORIGIN}
expect: webauthn: origin {ORIGIN}

cmd: webauthn
expect: webauthn: rpId {RP_ID}
expect: webauthn: origin {ORIGIN}
expect: crypto module: present

# TOFU registration mints an identity bound to the credential
cmd: webauthn register {CH_REG} {b64u(reg_cdj)} {b64u(reg_ao)}
expect: webauthn: registered identity:[0-9a-f-]+
capture: uuid registered identity:([0-9a-f-]+)

# the same credential cannot register twice (never bare TOFU re-bind)
cmd: webauthn register {CH_REG} {b64u(reg_cdj)} {b64u(reg_ao)}
expect: identity: credential already bound

# assertion verifies and advances signCount (5 at registration -> 6)
cmd: webauthn authenticate {CH_A1} {b64u(es_cred_id)} {b64u(a1_cdj)} {b64u(a1_ad)} {b64u(a1_sig)}
expect: webauthn: authenticated identity:%{{uuid}} signCount 6

# replaying the same assertion is refused (6 is not greater than 6)
cmd: webauthn authenticate {CH_A1} {b64u(es_cred_id)} {b64u(a1_cdj)} {b64u(a1_ad)} {b64u(a1_sig)}
expect: webauthn: signCount replay

# an unknown credential is refused
cmd: webauthn authenticate {CH_A1} {b64u(bytes(16))} {b64u(a1_cdj)} {b64u(a1_ad)} {b64u(a1_sig)}
expect: webauthn: unknown credential

# the System-tier API refuses a console caller
cmd: code "/usr/System/sys/webauthnd"->issue_challenge()
expect: Error: Access denied

# the record carries the passkey row with the advanced counter
cmd: identity show %{{uuid}}
expect: credentials: 1
expect: passkey signCount 6
"""
VERBSET.write_text(vs)

# --- emit the identity-recovery console verbset ---

rvs = f"""# identity recovery -- the operator half of the recovery doctrine on a
# module-bearing boot (regenerate with scripts/gen-webauthn-vectors.py;
# do not edit by hand):
#   LPC_EXT_CRYPTO=<crypto> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh \\
#       scripts/verbsets/identity-recovery.verbset
# Covers: mint-with-codes, the console bind verb (operator-mediated
# re-bind through the same ceremony verifier the wire uses), the
# never-bare-re-bind refusal, single-use redemption, and the
# last-credential redemption refusal.

# point the ceremony daemon at the vectors' rp
cmd: webauthn rpid {RP_ID}
expect: webauthn: rpId {RP_ID}

cmd: webauthn origin {ORIGIN}
expect: webauthn: origin {ORIGIN}

# a record minted with recovery codes only
cmd: identity mint 2
expect: identity: minted identity:[0-9a-f-]+ with 2 recovery codes
capture: uuid minted identity:([0-9a-f-]+)
capture: code1 identity: code 1: ([A-Za-z0-9_-]+)

# operator-mediated re-bind: the console verifies the attestation
# through the same ceremony entry the wire uses, then binds
cmd: identity bind %{{uuid}} {CH_REG4} {b64u(reg4_cdj)} {b64u(reg4_ao)}
expect: identity: bound passkey {b64u(es3_cred_id)} to identity:%{{uuid}}

cmd: identity show %{{uuid}}
expect: identity: credentials: 3

# never bare re-bind: an already-bound credential refuses
cmd: identity bind %{{uuid}} {CH_REG4} {b64u(reg4_cdj)} {b64u(reg4_ao)}
expect: identity: credential already bound

# redemption is single-use
cmd: identity redeem %{{uuid}} %{{code1}}
expect: identity: redeemed

cmd: identity redeem %{{uuid}} %{{code1}}
expect: identity: unknown recovery code

# the last credential cannot be redeemed away
cmd: identity mint 1
expect: identity: minted identity:[0-9a-f-]+ with 1 recovery codes
capture: uuid2 minted identity:([0-9a-f-]+)
capture: lastcode identity: code 1: ([A-Za-z0-9_-]+)

cmd: identity redeem %{{uuid2}} %{{lastcode}}
expect: cannot redeem the last credential
"""
RECOVERY_VERBSET.write_text(rvs)

print(f"wrote {VECTORS_H}")
print(f"wrote {VERBSET}")
print(f"wrote {RECOVERY_VERBSET}")
