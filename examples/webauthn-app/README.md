# WebAuthn codec substrate

The pure-function decoding a WebAuthn relying party performs on
credential payloads before any ceremony logic exists: base64url for
the JSON-carried fields, strict CBOR for the attestation object and
authenticator data, COSE_Key extraction to the raw public-key shapes
the crypto kfuns verify against. The library signatures live in
`docs/kernel-libraries.md` Utilities; this example is their live-boot
regression.

## Operations

- `/lib/util/base64` `urlEncode`/`urlDecode` round-trip the RFC 4648
  section 10 vectors and the URL-safe alphabet bytes (`-`/`_`,
  unpadded input).
- `/lib/util/cbor` decodes the RFC 8949 Appendix A vectors for the
  supported deterministic subset (integers, byte and text strings,
  arrays, maps, false/true/null) and rejects everything outside it:
  truncation, indefinite lengths, tags, floats, undefined, malformed
  initial bytes, duplicate map keys, null map values, trailing bytes.
- `/lib/util/cose` maps ES256 (EC2/P-256) and EdDSA (OKP/Ed25519)
  COSE_Key mappings to `({ scheme, key })` in the
  `decrypt("<scheme> verify", ...)` shapes, and rejects wrong key
  types, curve and algorithm mismatches, and malformed coordinates.
- A pipeline test decodes a CBOR-encoded COSE key with trailing data
  via `decodePrefix`, as a key appears embedded mid-stream in
  authenticator data.

## Deployment

```sh
cp -R examples/webauthn-app src/usr/WebAuthn
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new
domain automatically.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh webauthn-app
```

The runner does the clean-slate deploy, waits for the driver's
self-exit, and asserts 13 " OK" sentinels; boot output lands under
`state/`. The manual sequence it automates:

```sh
# Clean slate: remove any prior deploy and state, then redeploy.
rm -rf src/usr/WebAuthn
rm -f state/snapshot state/snapshot.old
cp -R examples/webauthn-app src/usr/WebAuthn

/path/to/dgd/bin/dgd example.dgd    # the driver dumps and exits itself
cat src/usr/WebAuthn/data/test-result.log
```

Expected result-log contents: `WebAuthn:test: starting`, then 13
sentinel lines ending ` OK` (`B64URL-VECTORS`, `B64URL-ALPHABET`,
`CBOR-INTS`, `CBOR-STRINGS`, `CBOR-ARRAYS`, `CBOR-MAPS`,
`CBOR-SIMPLE`, `CBOR-PREFIX`, `CBOR-REJECTS`, `COSE-ES256`,
`COSE-ED25519`, `COSE-REJECTS`, `COSE-PIPELINE`), no `FAIL` lines.

## Files

- `initd.c` -- domain initd; compiles `sys/test` at boot.
- `lib/app.c` -- thin marker lib mirroring the other bundled examples.
- `sys/test.c` -- boot-time test driver; runs the vector and reject
  batteries against the three codec libraries and self-exits via the
  System persist helper.

## Notes

- Everything here is decoding; no signature is verified and no
  network ceremony runs. The ceremony surface (challenge issuance,
  registration, assertion verification) builds on these codecs and
  ships with the platform identity service.
- The CBOR decoder is deliberately strict: WebAuthn's CTAP2 canonical
  encoding forbids indefinite lengths and duplicate keys, so the
  decoder treats them as errors rather than tolerating them.
