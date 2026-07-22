# Identity and authentication

The platform's shared identity substrate: one identity record per human or agent, the credentials bound to it, the ceremonies that authenticate it, and how an authenticated identity reaches authorization. This is the doctrine the earlier doc set deferred; it now ships.

**Audience**: an application or System-tier author building authenticated users on the platform, and an architect auditing how identity, credentials, and authorization compose. Assumes `docs/capability.md` for the authority model and `docs/system-daemons.md` for the daemon surfaces this page ties together.

## The shared substrate

Identity is a platform service, not a per-application concern. One identity record (`src/usr/System/obj/identity.c`, minted by `identityd`, `docs/system-daemons.md`) represents a human or agent, and every application consumes the same record rather than keeping its own user table. The record carries a fixed UUID, a principal string derived from it, and a set of typed credential rows.

The **principal string** is `identity:<uuid>`. It is the form an authenticated identity takes in the capability store, the third principal kind the store was built to hold alongside domain names and program paths (`docs/capability.md` Identity principals). This is the single seam between "who is authenticated" and "what they may do": the identity substrate produces the principal, and the capability library checks it. Application-facing surfaces (the `authd` facade, the composite example's wire) return the same string as the **subject** -- the acting identity -- and the store records it as a principal at check time.

**Credential rows** are typed. A passkey row holds the credential id, the extracted public key and its verify scheme, the WebAuthn signature counter, and timestamps. A recovery-code row holds a single-use hash. The record is the durable substrate both credential kinds attach to; new credential types extend it additively without a new record shape.

## Credentials: passkeys via WebAuthn

The first credential type is a **passkey** -- a WebAuthn (W3C Web Authentication) public-key credential. The platform is a relying party: it issues a challenge, and the authenticator (a security key, a phone, a laptop's secure enclave) signs it. No shared secret is stored; what persists is a public key. User verification -- the fingerprint, face, or PIN that unlocks the authenticator -- is the authenticator's own local ceremony: no biometric ever reaches the platform, which only ever verifies the signed assertion.

Registration is **trust-on-first-use** (TOFU) with attestation format `"none"`: the platform trusts the first authenticator that presents a credential for an identity, rather than verifying an attestation chain back to a manufacturer. This is the right posture for a platform that is not certifying hardware provenance -- it wants a key it can verify future assertions against, and TOFU gives it that without an attestation-root trust store. The verification is strict about everything else: the client-data type, the challenge it issued, the exact origin, the relying-party-id hash, and the user-present flag are all checked before the credential is bound (`src/lib/util/webauthn.c`, `docs/kernel-libraries.md`).

Authentication (an *assertion*) verifies the authenticator's signature over the authenticator data and a hash of the client data, against the stored public key, using the host crypto module's verify kfun (ES256 or Ed25519). The signature counter is checked for replay: when either the stored or the asserted counter is nonzero, the asserted counter must be strictly greater, then the stored counter advances. A clone of an authenticator that replays an old counter is refused.

The ceremonies are the `webauthnd` daemon's (`docs/system-daemons.md`); the pure verification is `/lib/util/webauthn`, exercised on a live boot against foreign-generated vectors (`examples/webauthn-app/`).

## Rotation and recovery

A credential set must survive the loss of one credential without locking the identity out, and without a bare re-enrollment that would let an attacker bind a fresh key to someone else's identity.

**Rotation** is atomic add-then-revoke: the replacement credential is bound before the old one is removed, in one atomic operation, so the record never passes through a zero-credential or old-only state observable from outside. The record never reaches zero credentials -- single-credential removals are refused up front, and compound operations validate the invariant at the end and roll back whole if it would be violated (`identityd`, `docs/system-daemons.md`).

**Recovery** has an order, strongest first:

1. **A spare passkey.** Encourage registering more than one authenticator, so losing one leaves another. Binding the spare is **enrollment**, the second-device path: a live session adds an additional passkey to its own record through authd's `enroll_passkey`. The registration payload is verified exactly as recovery's is (verified without a mint), then bound with the substrate's own rules doing the refusing (a globally-used credential id; a human row on an agent record), and no session is minted -- enrollment adds a credential, not authentication. The composite example binds it as `GET /auth/enroll-challenge` plus `POST /auth/enroll` (`docs/system-daemons.md` authd; `examples/composite-app`), so routine device addition never rides the recovery ceremony. Using the spare later is ordinary authentication, not recovery.
2. **Single-use recovery codes.** Generated at mint time (or provisioned later over a live session: authd's `rotate_recovery_codes`), stored hashed, shown once. Redeeming a code is single-use and refuses to consume the record's last credential; the recovery ceremony's substrate step redeems a code and binds a replacement credential in one atomic operation, valid even when the code is the last credential. The self-service wire form ships through the authd facade: `recover_identity` takes the code plus the NEW passkey's registration payload in one call, verified without a mint and composed atomically (`docs/system-daemons.md` authd); `examples/composite-app` binds it, with the handler's purpose-tagged challenge store as the reference discipline for running more than one ceremony kind over one store.
3. **Operator-mediated re-bind.** When an identity has lost access to every credential, an operator binds a new one through the console: `identity bind` verifies the new passkey's registration ceremony through webauthnd, then binds it to the existing record (`docs/admin-console.md`). This is a deliberate human step, not an automatic path.

Two rules bound recovery. **Never bare TOFU re-bind to an existing identity**: a fresh WebAuthn registration always mints a *new* identity (the credential id is globally unique in the store, so re-registering a bound credential fails); binding a new credential to an *existing* identity goes through session-proven enrollment, rotation, recovery, or the operator's deliberate re-bind -- never through the registration ceremony. **Never recover by minting a fresh identity**: recovery restores access to the same record, preserving its principal and everything granted to it; minting a new identity would silently orphan the old principal's authorizations.

**Credential bookkeeping is self-service.** A live session lists its own record's passkeys (credential id, created, last used -- bookkeeping, never key material) and revokes one of them, with the record's last passkey refused so a principal never revokes itself out of login; recovery codes rotate as a set (`rotate_recovery_codes` above), and sessions are separate state, untouched by either. The intended lost-device sequence is recovery first -- the replacement passkey binds atomically via `recover_identity` -- then revocation of the lost credential (`docs/system-daemons.md` authd; the composite example binds the pair as `GET /auth/passkeys` and `POST /auth/passkeys/<id>/revoke`).

## The three-layer authorization split

Authorization on the platform is three distinct layers, and keeping them distinct is the doctrine:

1. **Application-tier authorization lives in application objects.** Whether an authenticated identity may perform an application action -- post to this room, edit this document -- is the application's decision, gated the way the chat example gates rooms (`docs/chat-applications.md`). The platform supplies the authenticated principal; the application supplies the policy. This is the great majority of authorization, and it does not touch the capability store.
2. **Platform capabilities are granted to identity principals by an operator.** A capability the *platform* gates (not an application) reaches an identity through the operator grant path: `identity grant <uuid> <capability>` binds the capability to the `identity:<uuid>` principal, checked through the same `is_allowed` choke-point as any other principal. The store's mutation stays `KERNEL()`-gated; the operator path routes through a constrained elevation helper (`docs/capability.md` Identity principals). This is rare and deliberate -- a platform capability is not an application permission.
3. **Delegated grants ship in one constrained form; held-handle mediation is the future seam.** A controller may re-grant a subset of its own platform capabilities to its own agent (Agent identities below; the doctrine is `docs/capability.md` Identity principals). The general form -- peer-to-peer delegation, per-reference attenuation, authority carried by an unforgeable, revocable held reference rather than by the ambient principal string -- stays deferred: it needs host-driver primitives LPC does not have, and it attaches at the capability library's `principal`-argument seam (`docs/capability.md` Toward stricter object-capability).

## Sessions

An authentication ceremony is a point event; a session lets it persist across requests. `sessiond` mints a bearer token for an authenticated principal and validates it later (`docs/system-daemons.md`). The token's plaintext exists only in the mint response; what persists is its hash, so a statedump cannot leak a live token (a tested property, `scripts/session-smoke.sh`). This is the primitive only: no cookie handling and no HTTP bearer parsing ship. The daemon surface is System-tier, so a transport reaches it through the `authd` facade (`docs/system-daemons.md`), binding the token to its own request flow; `examples/composite-app` is the worked binding (`docs/composite-applications.md`).

## Agent identities

An agent -- an automated process acting on a human's behalf -- is an ordinary identity record, not a parallel account system. Two additive fields separate it from a human record: a **kind** (`human` or `agent`; records minted before the field existed read as human, so nothing migrates), and an immutable **controller** edge that must name a human identity. Control cannot chain -- an agent never controls an agent, so a "sub-agent" is minted as another agent of the same human -- and every agent action is therefore attributable to exactly one accountable **natural person** through one enumerable edge. The anchor is deliberately natural-person-shaped: an organizational (legal-person) controller is not modeled, so an organization acts here through the natural persons who control its agents. The agent's principal keeps the uniform `identity:<uuid>` form: the capability store, the elevation helpers, and every `is_allowed` call site are untouched, and agent-ness is a substrate query, not principal parsing.

**Two agent credential types.** The primary is an **agent key**: a raw public key and verify scheme bound to the record; the private key never reaches the platform. The ceremony (verified by `agentauthd`, `docs/system-daemons.md`) is challenge-sign over a domain-separated message -- a fixed tag plus the caller-owned single-use challenge -- so an agent-auth signature can never be replayed into another protocol that has the agent sign something. There is no signature counter: WebAuthn's counter exists because authenticator hardware is cloneable, and an agent key is just a key, so replay protection is the single-use challenge. The fallback is an **agent token**, for agents that cannot hold a signing key: platform-generated (never imported), stored as a hash under the recovery-code discipline, with a **required expiry** (30-day default, 90-day cap, settable at bind) and last use stamped at each ceremony for stale-credential review. The token is the exception path, expected to narrow as agent stacks learn to hold keys.

**The two-stage credential doctrine.** An agent credential is a session-minting root, never ambient per-request authority: a ceremony yields an ordinary short-lived session (sessiond's one-hour default, one-day cap), and authority is checked at use through `is_allowed`. A long-lived agent key is defensible *only* because every check hits live platform state -- the record's kind and suspended flag are read at ceremony time, sessions die at suspension, and no authorization result is ever cached. That conditional is doctrine, not an implementation detail: a deployment that caches authorization decisions has silently traded away the revocation guarantees this substrate claims.

**Minting is two paths onto one substrate operation.** An operator mints an agent through the console (`identity mint-agent`, `docs/admin-console.md`); a controller mints its own through the `authd` facade, where a live session proves the controller and the new agent's controller edge derives from that proven principal, never from a caller-supplied argument. The same session-proven, own-agents-only shape covers controller suspend/resume and delegation.

**Suspension is the kill switch.** A suspended agent cannot authenticate (checked at ceremony time for both credential types), its live sessions are revoked in the same operation, and its delegated grants die with them. The record and its audit trail persist: suspension is reversible where deletion would break the grant graph and orphan the controller edge. Resume restores the ability to authenticate only -- grants revoked at suspension are re-delegated deliberately, never automatically. Humans keep the no-suspend, no-delete posture.

**Delegation** -- a controller granting its agent a subset of its own platform capabilities -- is the first consumer of the delegated-grant seam, with its own doctrine in `docs/capability.md` (Identity principals): per-capability operator-flagged delegability, delegation only from a controller to its own agent, a source-tracked eager revocation cascade, and an accountability claim rather than a confinement claim.

`scripts/agent-smoke.sh` proves the ceremonies (against a foreign Ed25519 signer), the required token expiry, the suspension semantics, and the statedump discipline for agent tokens on a live boot. `examples/agent-app/` is the worked binding of this doctrine as an application: the registration, minting, ceremony, and refusal battery run as a boot-time driver, with the operator half driven by `scripts/verbsets/agent-app.verbset`.

## Observing the substrate: the mutation-notification seam

Every committed mutation of the substrate notifies subscribed observers, from inside the atomic mutator: identityd's `subscribe_events` delivers `identity_event(event, data)` to each observer in its own zero-delay call_out, so an aborted mutation rolls its notifications back with everything else, and a consumer never sees state that did not commit. The event vocabulary (`IDEV_*`, `src/include/identityd.h`) covers the record lifecycle -- minting, credential bind and removal, recovery, suspension and resume, grants, delegation and its revocation cascades -- and the data values are uuid-tier bookkeeping (uuids, capability names, credential ids, counts), never key material, hashes, or plaintext. Per-login bookkeeping (signature counters, last-use stamps) deliberately does not notify.

The seam is what event-driven identity consumers build on instead of polling substrate state:

- **A live view.** Recompute a derived snapshot when the substrate changes, not on a timer. The composite example's agent-state stream is the worked consumer: on any event that can change a controller's own-agents view, it recomputes each subscriber's snapshot and pushes the ones that changed (`docs/composite-applications.md` The event streams).
- **Notification policy around recovery.** The substrate deliberately ships no notify/delay/cancel window on recovery; the events (a recovery emits its bind, its removal, and `recovered`) are the hook an application-tier policy builds on -- mail the identity's registered address, hold a cancellation window, or both.
- **Audit hooks.** Record grant and delegation churn as it commits -- each suspension or revocation cascade emits `undelegated` per dropped delegation, so an audit trail can attribute every authority change to the operation that caused it.

The contract and its trust boundary (sys-tier observers -- deliberately wider than the System-tier mutating surface) are `docs/system-daemons.md` subscribe_events.

## The boundary: operator authentication is a separate circuit

Operator (DGD-admin) authentication is **not** part of this substrate, and the separation is load-bearing. An operator is a kernel access-list entry plus a password hash in the operator's user object, authenticated by password over the tunneled telnet console (`docs/admin-console.md` Connecting, `docs/security-posture.md` Credential lifecycle). That circuit is unchanged and independent:

- A platform identity is **never** minted as a kernel user or owner. Identities live in the System-tier substrate under `identity:<uuid>` principals; they are not access-list entries and hold no owner tier.
- A platform identity **never** attaches to the operator circuit. Authenticating as an identity does not grant console access; console access is the operator password path, gated separately.
- The operator is the one who runs the identity substrate's privileged operations (minting, operator-mediated re-bind, platform-capability grants) *through* the console -- but the operator's own authentication is the password circuit, not a passkey.

Keeping these apart means an application-user compromise cannot escalate to console (host-shell-equivalent) authority, and a console compromise is bounded by the operator credential lifecycle, not by any identity's credentials.

## Where to next

- [system-daemons.md](system-daemons.md) -- the `identityd`, `webauthnd`, and `sessiond` surfaces this page composes.
- [capability.md](capability.md) -- the authority model, the identity-principal grant path, and the delegation doctrine (shipped for controller-to-agent; held-reference general form deferred).
- [kernel-libraries.md](kernel-libraries.md) -- the `/lib/util/webauthn`, `cbor`, and `cose` verification and codec libraries.
- [security-posture.md](security-posture.md) -- the operator credential circuit this substrate is deliberately separate from.
- [application-authoring.md](application-authoring.md) Identity and request authentication -- how an application consumes an authenticated identity.
