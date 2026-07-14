# Identity and authentication

The platform's shared identity substrate: one identity record per human or agent, the credentials bound to it, the ceremonies that authenticate it, and how an authenticated identity reaches authorization. This is the doctrine the earlier doc set deferred; it now ships.

**Audience**: an application or System-tier author building authenticated users on the platform, and an architect auditing how identity, credentials, and authorization compose. Assumes `docs/capability.md` for the authority model and `docs/system-daemons.md` for the daemon surfaces this page ties together.

## The shared substrate

Identity is a platform service, not a per-application concern. One identity record (`src/usr/System/obj/identity.c`, minted by `identityd`, `docs/system-daemons.md`) represents a human or agent, and every application consumes the same record rather than keeping its own user table. The record carries a fixed UUID, a principal string derived from it, and a set of typed credential rows.

The **principal string** is `identity:<uuid>`. It is the form an authenticated identity takes in the capability store, the third principal kind the store was built to hold alongside domain names and program paths (`docs/capability.md` Identity principals). This is the single seam between "who is authenticated" and "what they may do": the identity substrate produces the principal, and the capability library checks it.

**Credential rows** are typed. A passkey row holds the credential id, the extracted public key and its verify scheme, the WebAuthn signature counter, and timestamps. A recovery-code row holds a single-use hash. The record is the durable substrate both credential kinds attach to; new credential types extend it additively without a new record shape.

## Credentials: passkeys via WebAuthn

The first credential type is a **passkey** -- a WebAuthn (W3C Web Authentication) public-key credential. The platform is a relying party: it issues a challenge, and the authenticator (a security key, a phone, a laptop's secure enclave) signs it. No shared secret is stored; what persists is a public key.

Registration is **trust-on-first-use** (TOFU) with attestation format `"none"`: the platform trusts the first authenticator that presents a credential for an identity, rather than verifying an attestation chain back to a manufacturer. This is the right posture for a platform that is not certifying hardware provenance -- it wants a key it can verify future assertions against, and TOFU gives it that without an attestation-root trust store. The verification is strict about everything else: the client-data type, the challenge it issued, the exact origin, the relying-party-id hash, and the user-present flag are all checked before the credential is bound (`src/lib/util/webauthn.c`, `docs/kernel-libraries.md`).

Authentication (an *assertion*) verifies the authenticator's signature over the authenticator data and a hash of the client data, against the stored public key, using the host crypto module's verify kfun (ES256 or Ed25519). The signature counter is checked for replay: when either the stored or the asserted counter is nonzero, the asserted counter must be strictly greater, then the stored counter advances. A clone of an authenticator that replays an old counter is refused.

The ceremonies are the `webauthnd` daemon's (`docs/system-daemons.md`); the pure verification is `/lib/util/webauthn`, exercised on a live boot against foreign-generated vectors (`examples/webauthn-app/`).

## Rotation and recovery

A credential set must survive the loss of one credential without locking the identity out, and without a bare re-enrollment that would let an attacker bind a fresh key to someone else's identity.

**Rotation** is atomic add-then-revoke: the replacement credential is bound before the old one is removed, in one atomic operation, so the record never passes through a zero-credential or old-only state observable from outside. The record never reaches zero credentials -- single-credential removals are refused up front, and compound operations validate the invariant at the end and roll back whole if it would be violated (`identityd`, `docs/system-daemons.md`).

**Recovery** has an order, strongest first:

1. **A spare passkey.** Encourage registering more than one authenticator at signup, so losing one leaves another. This is ordinary authentication, not recovery.
2. **Single-use recovery codes.** Generated at mint time, stored hashed, shown once. Redeeming a code is single-use and refuses to consume the record's last credential; the recovery ceremony's substrate step redeems a code and binds a replacement credential in one atomic operation, valid even when the code is the last credential.
3. **Operator-mediated re-bind.** When an identity has lost every credential, an operator binds a new one through the console. This is a deliberate human step, not an automatic path.

Two rules bound recovery. **Never bare TOFU re-bind to an existing identity**: a fresh WebAuthn registration always mints a *new* identity (the credential id is globally unique in the store, so re-registering a bound credential fails); binding a new credential to an *existing* identity goes through rotation or recovery, never through the registration ceremony. **Never recover by minting a fresh identity**: recovery restores access to the same record, preserving its principal and everything granted to it; minting a new identity would silently orphan the old principal's authorizations.

## The three-layer authorization split

Authorization on the platform is three distinct layers, and keeping them distinct is the doctrine:

1. **Application-tier authorization lives in application objects.** Whether an authenticated identity may perform an application action -- post to this room, edit this document -- is the application's decision, gated the way the chat example gates rooms (`docs/chat-applications.md`). The platform supplies the authenticated principal; the application supplies the policy. This is the great majority of authorization, and it does not touch the capability store.
2. **Platform capabilities are granted to identity principals by an operator.** A capability the *platform* gates (not an application) reaches an identity through the operator grant path: `identity grant <uuid> <capability>` binds the capability to the `identity:<uuid>` principal, checked through the same `is_allowed` choke-point as any other principal. The store's mutation stays `KERNEL()`-gated; the operator path routes through a constrained elevation helper (`docs/capability.md` Identity principals). This is rare and deliberate -- a platform capability is not an application permission.
3. **Held-handle mediation is the future seam.** A no-ambient-authority mode where authority is carried by an unforgeable, attenuable, revocable held reference rather than by the ambient principal string -- and, built on it, delegated grants where one identity re-grants a subset of its own authority. Both are deferred: they need host-driver primitives LPC does not have, and they attach at the capability library's `principal`-argument seam (`docs/capability.md` Identity principals and Toward stricter object-capability).

## Sessions

An authentication ceremony is a point event; a session lets it persist across requests. `sessiond` mints a bearer token for an authenticated principal and validates it later (`docs/system-daemons.md`). The token's plaintext exists only in the mint response; what persists is its hash, so a statedump cannot leak a live token (a tested property, `scripts/session-smoke.sh`). This is the primitive only: no cookie handling and no HTTP bearer parsing ship. The daemon surface is System-tier, so a transport reaches it through the `authd` facade (`docs/system-daemons.md`), binding the token to its own request flow; `examples/composite-app` is the worked binding (`docs/composite-applications.md`).

## The boundary: operator authentication is a separate circuit

Operator (DGD-admin) authentication is **not** part of this substrate, and the separation is load-bearing. An operator is a kernel access-list entry plus a password hash in the operator's user object, authenticated by password over the tunneled telnet console (`docs/admin-console.md` Connecting, `docs/security-posture.md` Credential lifecycle). That circuit is unchanged and independent:

- A passkey identity is **never** minted as a kernel user or owner. Identities live in the System-tier substrate under `identity:<uuid>` principals; they are not access-list entries and hold no owner tier.
- A passkey identity **never** attaches to the operator circuit. Authenticating as an identity does not grant console access; console access is the operator password path, gated separately.
- The operator is the one who runs the identity substrate's privileged operations (minting, operator-mediated re-bind, platform-capability grants) *through* the console -- but the operator's own authentication is the password circuit, not a passkey.

Keeping these apart means an application-user compromise cannot escalate to console (host-shell-equivalent) authority, and a console compromise is bounded by the operator credential lifecycle, not by any identity's credentials.

## Where to next

- [system-daemons.md](system-daemons.md) -- the `identityd`, `webauthnd`, and `sessiond` surfaces this page composes.
- [capability.md](capability.md) -- the authority model, the identity-principal grant path, and the deferred delegated-grant seam.
- [kernel-libraries.md](kernel-libraries.md) -- the `/lib/util/webauthn`, `cbor`, and `cose` verification and codec libraries.
- [security-posture.md](security-posture.md) -- the operator credential circuit this substrate is deliberately separate from.
- [application-authoring.md](application-authoring.md) Identity and request authentication -- how an application consumes an authenticated identity.
