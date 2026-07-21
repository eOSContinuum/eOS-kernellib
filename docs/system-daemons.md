# System-daemon application surface

The signature reference for the daemons an author is told to consume elsewhere in this doc set: the compile-graph recorder (objectd), the upgrade coordinator (upgraded), the error manager (errord), the logger (logd), the capability store (capabilityd), the identity registry (identityd), the WebAuthn ceremony daemon (webauthnd), the session daemon (sessiond), the agent ceremony daemon (agentauthd), the transport authentication facade (authd), and the logical-name registry (the Index daemon). Format follows `docs/dispatcher.md`'s Application surface: per-function signature, gating, semantics. The source files are authoritative.

**Audience**: a System-tier or application author about to call one of these daemons directly and needing the exact contract, after the owning concept doc (`docs/code-lifecycle.md`, `docs/operations.md`, `docs/capability.md`, `docs/schema.md`) has explained when to.

Gating vocabulary: SYSTEM() admits callers under `/usr/System`; KERNEL() admits kernel-tier callers; a `previous_program()` gate admits exactly the named program. What a gated function does for an outside caller varies by function -- silent nil, an empty return, or an error -- and is noted where it matters; do not probe a gate by calling it.

## objectd -- `src/usr/System/sys/objectd.c`

The compile-time program graph: which programs exist, what each includes and inherits. The driver-hook event surface (compile, destruct, touch, and the rest) is covered in `docs/code-lifecycle.md`; the query surface below is SYSTEM()-gated.

### `string query_path(int index)`

The object path registered at program index `index`, or nil.

### `int *query_issues(string path)`

The active program issue indices for `path`. A library recompiled while inherited can carry more than one live issue; a path with none returns an empty array.

### `string *query_includes(int index)`

The files the program at `index` includes.

### `int **query_included(string path)`

The indices of programs that include `path`. The outer array's grouping is a storage artifact (buckets bounded by the host's array-size limit), not a per-program structure; flatten before use.

### `int *query_inherits(int index)`

The indices of programs the program at `index` inherits.

### `int **query_inherited(int index)`

The indices of programs that inherit the program at `index`, with the same bucket-shaped outer array as `query_included`. This is the record the upgrade daemon walks when a library recompile must cascade. The array-returning queries in this group return an empty array both for a caller outside the SYSTEM() gate and for a path or index with no entries.

## upgraded -- `src/usr/System/sys/upgraded.c`

The live-upgrade coordinator behind the console's `upgrade` verb (`docs/changing-a-running-system.md` describes the ladder; `docs/admin-console.md` the verb).

### `mixed upgrade(string owner, string *sources, int atom, object patchtool)`

SYSTEM()-gated. Drive a recompile cascade for the named source files under `owner`. Returns an array of failed paths -- empty on success -- or an error string when the upgrade cannot start or a precondition fails (one already running, patching still in progress, access denied, a missing source file, no existing issues); nil only signals a caller outside the gate. With `atom` nonzero the whole cascade runs atomically (all-or-nothing). A non-nil `patchtool` supplies `do_patch(obj)`, which the daemon applies to every live clone in an eager sweep of zero-delay callouts after the recompile, one object per tick (`docs/changing-a-running-system.md` describes the sweep).

The daemon's other entry points are internal (leaf generation is gated to the System auto).

## errord -- `src/usr/System/sys/errord.c`

The error manager has no application-facing query surface: its three functions (`runtime_error`, `atomic_error`, `compile_error`) are driver hooks, callable only by the kernel driver. Their dispatch, formatting, and logd persistence are documented in `docs/operations.md` (Logging and diagnostics) and `docs/debugging-applications.md` (Reading an error trace); the hook signatures live in `docs/kernel-reference.md` (the driver hooks section).

## logd -- `src/usr/System/sys/logd.c`

Persistent leveled logging into `/usr/System/log/system.log` (semantics and the operator surface: `docs/operations.md` Logging and diagnostics).

### `void log(int level, string msg)`

Public. Append a diagnostic line at `level` (the `LOG_DEBUG` .. `LOG_ERROR` constants). Lines below the threshold are dropped; admitted lines buffer and flush on a deferred call_out, which is what lets logging survive atomic execution. Never throws.

### `void log_report(string report)`

Gated to SYSTEM() or KERNEL() callers; errord's persistence path. Appends a pre-formatted report at ERROR level, bypassing the threshold.

### `void set_threshold(int level)` / `int query_threshold()`

The write is KERNEL()-gated (an ungated caller gets an error, not a silent nil) and errors on a level outside the valid range; the read is public. Default threshold is INFO. Operators reach these through the `log-level` verb.

## capabilityd -- `src/kernel/sys/capabilityd.c`

The capability store behind the platform's gating surfaces. `docs/capability.md` (The mechanism) is the owning document; the surface, for completeness:

- `int is_allowed(string capability, string principal)` -- public, silent membership read
- `void require_member(string capability, string principal)` -- throwing check, uniform denial message
- `void grant(string capability, string principal)` / `void revoke(string capability, string principal)` -- KERNEL()-gated mutation (an ungated caller gets an error)
- `string *query_principals(string capability)` / `string *query_capabilities()` -- public reads
- `void set_delegable(string capability, int flag)` / `int query_delegable(string capability)` -- the per-capability delegable flag gating controller-to-agent delegation (`docs/capability.md` Identity principals); mutation KERNEL()-gated, read public. The operator face is the `capability` console verb (list the store; `capability delegable <cap> on|off`).

## identityd -- `src/usr/System/sys/identityd.c`

The platform identity substrate: mints identity records (clones of `src/usr/System/obj/identity.c`, one per identity), owns the global credential-id index, and enforces the record invariants -- a credential id binds to at most one identity ever, and a record never reaches zero credentials (single-step removals are guarded up front; compound operations run as `atomic` functions validated at the end, so a violating compound rolls back whole). A record's principal string (`identity:<uuid>`) is the form capabilityd principals take for authenticated identities. Records carry a kind (human or agent; agent records add an immutable controller edge and a suspended flag, `docs/identity.md` Agent identities), and credential rows bind by kind: passkeys and recovery codes to human records, agent keys and agent tokens to agent records, checked at the single bind choke point. Recovery codes and agent tokens are generated here (never imported), stored hashed; plaintext exists only in the minting response, and agent tokens carry a required expiry (30-day default, 90-day cap). Randomness and hashing need the host crypto module -- without it the daemon boots, reports the stand-down, and refuses minting cleanly. Every committed mutation notifies subscribed observers (`subscribe_events` below); per-login bookkeeping (`update_sign_count`, `touch_credential`) deliberately does not. The mutating and reading surface is System/kernel-tier; the operator face is the `identity` console verb (`docs/admin-console.md`). Credential-row keys and types, and the mutation-event vocabulary, are defined in `src/include/identityd.h`.

### `atomic string create_identity(string credentialId, mapping row)` / `atomic mixed *mint_with_codes(int n)`

Mint a record with its first credential (a record never exists empty): from a validated credential row, or from `n` freshly generated recovery codes (returns `({ uuid, code... })`).

### `atomic void bind_credential(string uuid, string credentialId, mapping row)` / `atomic void unbind_credential(string uuid, string credentialId)`

Add a credential (globally unique id) / remove one (refuses to empty the record).

### `atomic void rotate_credential(string uuid, string newId, mapping row, string oldId)`

Add-then-revoke as one atomic step -- the record never passes through a zero-credential or old-only state observable from outside.

### `atomic string *rotate_recovery_codes(string uuid, int n)`

Replace the record's recovery-code set with `n` fresh codes; returns the plaintext codes. A rotation that would empty a codes-only record aborts and rolls back whole.

### `atomic int redeem_recovery_code(string uuid, string code)` / `atomic void redeem_and_replace(string uuid, string code, string newId, mapping row)`

Single-use redemption (refuses to consume the last credential) / the recovery ceremony's substrate step: redeem plus bind the replacement in one atomic operation, valid even on the last credential.

### `void update_sign_count(string uuid, string credentialId, int count)` / `void touch_credential(string uuid, string credentialId)`

WebAuthn bookkeeping on a bound passkey (sets the authenticator counter and last-used time) / ceremony bookkeeping on any bound credential (stamps last use; agentauthd's path).

### `atomic string mint_agent(string controllerUuid, string credentialId, mapping row)` / `atomic string *mint_agent_with_token(string controllerUuid, varargs int ttl)` / `atomic string *bind_agent_token(string uuid, varargs int ttl)`

Agent minting: with a caller-supplied agent-key row (public key material only), or with a fresh platform-generated token (returns `({ uuid, token })`, the only time the plaintext exists). The controller must be an existing human record. `bind_agent_token` adds a fresh token to an existing agent (returns `({ credentialId, token })`); a non-positive `ttl` takes the 30-day default, capped at 90 days.

### `atomic int suspend_agent(string uuid)` / `atomic void resume_agent(string uuid)`

The kill switch: suspend blocks authentication at ceremony time, revokes the agent's live sessions (returns the count), and revokes its delegated grants in the same atomic operation. Resume restores the ability to authenticate only.

### `void subscribe_events(object daemon)`

The mutation-notification seam. Gated to sys-tier daemons (`/usr/<domain>/sys/...` -- the WWW servers' `push_event` trust boundary, deliberately wider than the System-tier mutating surface: observers receive uuid-tier bookkeeping, never credential material). Each committed mutation delivers `daemon->identity_event(event, data)` in its own zero-delay call_out per observer, armed inside the atomic mutator: an aborted mutation rolls its notifications back with everything else, and an observer failure (catch-wrapped, own execution round) never touches identity state. Event names and the data keys each carries are the `IDEV_*` vocabulary in `src/include/identityd.h`; data values are uuids, capability names, credential ids, and counts -- never key material, hashes, or plaintext. Compound operations emit each substrate operation they perform (a recovery emits the bind, the removal, and `recovered`; suspension and revocation cascades emit `undelegated` per dropped delegation; `reconcile_delegations` emits what it revokes). A destructed observer drops from the object-keyed registry on its own; there is no explicit unsubscribe. The `identity` status verb reports the live observer count.

### `atomic void delegate_capability(string controllerUuid, string agentUuid, string capability)` / `atomic void undelegate_capability(string controllerUuid, string agentUuid, string capability)`

Controller-to-agent delegation, checked against live state in one atomic operation (delegator holds it, flagged delegable, target is the delegator's own unsuspended agent) and routed through the identity-constrained elevation helper. Undelegate removes the delegation; the store entry dies only with its last source (`docs/capability.md` Identity principals).

### `mapping query_delegations(string uuid)` / `atomic string *reconcile_delegations(string uuid)`

An agent's delegations (capability : grantor uuid, a copy) / the bypass heal: revoke any delegation whose grantor no longer holds the capability; returns the capabilities revoked. `identity show` reconciles at read.

### `string *query_agents(string controllerUuid)` / `string *query_grant_sources(string uuid, string capability)`

The agents a controller controls (the controller index) / the tracked reasons a grant exists (`operator`, `delegated`).

### `object find_identity(string uuid)` / `string find_by_credential(string credentialId)` / `int query_identity_count()`

Lookups (System/kernel-tier like the rest of the surface).

### `void grant_capability(string uuid, string capability)` / `void revoke_capability(string uuid, string capability)` / `string *query_grants(string uuid)`

The operator path binding a platform capability to an identity's principal (`identity:<uuid>`). `capabilityd`'s own grant/revoke stay `KERNEL()`-gated; these derive the principal from the uuid and route through the console registry's identity-constrained elevation helper, then the grant is checked back through the ordinary `is_allowed` choke-point (`docs/capability.md` Identity principals). `query_grants` reads back the platform capabilities the identity holds.

## webauthnd -- `src/usr/System/sys/webauthnd.c`

The WebAuthn ceremony daemon: composes the pure verification library (`/lib/util/webauthn`, `docs/kernel-libraries.md`) with identityd. TOFU registration verifies a foreign attestation payload and mints an identity bound to the new credential (identityd's global credential-id uniqueness makes a re-registration of a bound credential fail -- never bare TOFU re-bind); assertion verification checks the signature against the stored credential and enforces the signature-counter policy (when either counter is nonzero, the asserted counter must be strictly greater than the stored one) before advancing it. The daemon holds no challenge state: `issue_challenge()` returns fresh secure randomness and the verifying entry points take the expected challenge from the caller -- the session layer that issued it owns it. rpId and origin are operator-configured via the `webauthn` console verb (`docs/admin-console.md`); the surface is System/kernel-tier.

### `string issue_challenge()`

A fresh single-use challenge, base64url (32 bytes of `secure_random`); errors without the crypto module.

### `mapping verify_registration_payload(string challenge, string clientDataJSON, string attestationObject)`

Registration-ceremony verification without a mint: returns the verified credential row, keyed under `"credentialId"` (base64url, the form the store binds). What happens to the row is the System-tier caller's composition -- `register_credential` mints a fresh identity, authd's recovery ceremony pairs it with a code redemption onto an existing record, and the operator `identity bind` verb attaches it directly. Never-bare-re-bind holds because no caller composes a bare re-bind out of the registration route.

### `string register_credential(string challenge, string clientDataJSON, string attestationObject)`

The TOFU registration ceremony; returns the new principal string (`identity:<uuid>`).

### `string verify_assertion(string challenge, string credentialId, string clientDataJSON, string authenticatorData, string signature)`

The assertion ceremony against the stored credential (`credentialId` in base64url, as bound at registration); enforces and advances the signature counter; returns the principal.

### `void configure(string rpId, string origin)` / `string query_rp_id()` / `string query_origin()`

Relying-party configuration; nil leaves a value unchanged.

## sessiond -- `src/usr/System/sys/sessiond.c`

Mints and validates bearer session tokens for authenticated principals -- the primitive only; no cookie handling or HTTP bearer parsing ships here, a transport surface that wants sessions calls `mint`/`validate` directly. Secret discipline: a token's plaintext exists only in the mint response; what persists is `SHA-256(token) -> a session record` (principal, created, expires), and validation hashes the presented token to look it up. Because the plaintext is never stored it cannot reach the statedump -- `scripts/session-smoke.sh` proves this against a live image (a scan for the plaintext token bytes, with the stored hash and principal as the positive control). Hashing and randomness need the host crypto module; without it the daemon boots, reports the stand-down, and refuses minting. The surface is System/kernel-tier; the operator face is the `session` console verb (`docs/admin-console.md`).

### `string mint(string principal, varargs int ttl)`

Mint a session; returns the plaintext token (the only time it exists). `ttl` caps at one day; a non-positive `ttl` uses the one-hour default.

### `string validate(string token)`

The principal a live token authenticates, or nil for an unknown or expired token.

### `int revoke(string token)` / `int revoke_principal(string principal)` / `int query_session_count()`

Drop one session (TRUE iff a live one was removed); drop every session for a principal (a logout-everywhere primitive; returns the count); the live-session count (expired records reaped first).

## agentauthd -- `src/usr/System/sys/agentauthd.c`

The agent ceremony daemon: verifies the two agent authentication ceremonies against live substrate state (the record's kind and suspended flag are checked at ceremony time) and returns the proven principal. It never mints sessions -- authd composes ceremony plus mint -- and never mutates records beyond last-use bookkeeping. The key assertion verifies a signature over a domain-separated message (a fixed tag plus the challenge, so an agent-auth signature cannot be replayed into another protocol) with the bound row's verify scheme and raw public key; replay protection is the caller-owned single-use challenge, with no signature counter. The token ceremony hashes the presented token, requires a matching stored hash and an unexpired row, and stamps last use. Challenge ownership follows the webauthnd contract: the daemon holds no challenge state. Ceremonies need the crypto module; the verifying surface is System/kernel-tier (`docs/identity.md` Agent identities; `scripts/agent-smoke.sh` is the live proof).

### `string issue_challenge()`

A fresh single-use challenge, base64url (32 bytes of `secure_random`); errors without the crypto module. Public, like webauthnd's.

### `string verify_key_assertion(string challenge, string credentialId, string signature)`

The key ceremony: signature over the domain-separated message against the bound agent-key row; returns the proven principal. Refuses a non-agent or suspended record, an unknown credential, and an invalid signature.

### `string verify_token(string token)`

The token ceremony: hash lookup, full-hash match, expiry and record checks; returns the proven principal.

## authd -- `src/usr/System/sys/authd.c`

The transport authentication facade. The identity substrate's daemons gate every entry to System/kernel callers, so a tier-E transport surface -- an HTTP application binding a login flow, a non-HTTP protocol doing the same -- consumes ceremonies and sessions through this facade instead. It composes webauthnd and sessiond and exposes exactly the ceremony-plus-session flow; it deliberately does NOT expose `sessiond->mint` (minting a session for an arbitrary principal string would forge authority -- here a session is minted only for the principal a ceremony just proved), identityd mutation, or the capability grant path. The facade holds no state: challenge ownership stays with the caller per the webauthnd contract, and the reference single-use challenge store is `examples/composite-app`'s handler (`docs/composite-applications.md`). Unlike the rest of this page, the surface is deliberately callable from every tier.

### `string issue_challenge()`

A fresh single-use challenge via webauthnd; the caller stores it and accepts it back exactly once.

### `mixed *register_identity(string challenge, string clientDataJSON, string attestationObject, varargs int ttl)`

The TOFU registration ceremony plus session mint in one step; returns `({ principal, token })`. Ceremony errors propagate.

### `mixed *authenticate(string challenge, string credentialId, string clientDataJSON, string authenticatorData, string signature, varargs int ttl)`

The assertion ceremony plus session mint in one step; returns `({ principal, token })`.

### `string validate(string token)` / `int logout(string token)`

sessiond's validate and revoke, passed through: the principal a live token authenticates (or nil), and single-session revocation (TRUE iff a live one was removed).

### `mixed *authenticate_agent_key(string challenge, string credentialId, string signature, varargs int ttl)` / `mixed *authenticate_agent_token(string agentToken, varargs int ttl)`

The agent ceremonies (verified by agentauthd) plus session mint in one step; returns `({ principal, token })` exactly like the human flow -- a session is minted only for a ceremony-proven principal.

### `string mint_agent(string sessionToken, string credentialId, mapping row)` / `string *mint_agent_with_token(string sessionToken, varargs int ttl)` / `int suspend_agent(string sessionToken, string agentUuid)` / `void resume_agent(string sessionToken, string agentUuid)` / `void delegate_capability(string sessionToken, string agentUuid, string capability)` / `void undelegate_capability(string sessionToken, string agentUuid, string capability)`

Controller self-service: a live session proves the controlling identity, and every operation derives the controller from that proven principal, never from a caller-supplied argument -- the new agent's controller edge on the mint paths, and the own-agents constraint on suspend/resume/delegate/undelegate. The substrate enforces that only a human record controls agents, so an agent session cannot mint or manage agents (`docs/identity.md` Agent identities).

### `mixed *query_agents(string sessionToken)`

The session identity's own agents, read-only: one row per agent, `({ uuid, suspended, delegated capabilities })`. The controller derives from the live session, so a caller can only ever see its own; rows carry record state, never credential material.

### `mixed *recover_identity(string uuid, string code, string challenge, string clientDataJSON, string attestationObject, varargs int ttl)`

The recovery ceremony plus session mint: verifies the NEW passkey's registration payload without a mint, then redeems the recovery code and binds the verified credential in identityd's one atomic step (valid even on the record's last credential), then mints the session for the recovered principal. Both proofs travel in one call -- a wrong code binds nothing, a bad attestation redeems nothing, and there is no intermediate recovery state to hijack (`docs/identity.md` Rotation and recovery).

### `string *rotate_recovery_codes(string sessionToken, int n)`

Self-service recovery-code provisioning: a live identity session replaces its record's code set with `n` fresh codes and receives the plaintext -- the only time it exists. Without this entry a transport-registered identity would have no codes and no self-service recovery path.

### `string enroll_passkey(string sessionToken, string challenge, string clientDataJSON, string attestationObject)`

Add-passkey enrollment: a live session binds an ADDITIONAL passkey to its own record -- the second-device path, so routine device addition never rides the recovery ceremony. The registration payload is verified exactly as recovery's is (verify without a mint), then bound with the substrate's own rules doing the refusing (a globally-used credential id; a human row on an agent record). Returns the new credentialId; no session is minted -- enrollment adds a credential, not authentication.

### `mixed *query_passkeys(string sessionToken)` / `void revoke_passkey(string sessionToken, string credentialId)`

Passkey self-service on the session's own record. The read returns one row per passkey credential, `({ credentialId, created, lastUsed })` -- bookkeeping, never key material. The revocation removes one of the record's own passkeys, refusing non-passkey rows (recovery codes rotate as a set above) and the record's last passkey, so a principal never revokes itself out of login; the substrate's never-zero guard backs it at the record level. Sessions are separate state and are untouched. The intended sequence for a lost device is recovery first (the replacement passkey binds via `recover_identity`), then revocation of the lost credential here.

## Index daemon -- `src/usr/Index/sys/index_daemon.c`

The logical-name registry behind `/lib/util/named.c` (`set_object_name` / `find_named`, catalogued in `docs/kernel-libraries.md`). Names are colon-delimited paths (`Domain:path:name`); registration is reserved to the named library, lookup is public.

### `atomic void set_name(object ob, string name)` / `atomic void clear_name(string name)`

Gated to `/lib/util/named` by `previous_program()`. Register or unregister a logical name. `set_name` errors on a malformed name (empty, leading `/`, empty segment) and on every collision class: the name taken by another object, an intermediate path segment already occupied by an object, or the name referring to a folder. Replacing an object's prior name is the caller's job -- the named library's `set_object_name` clears the old name before re-registering.

### `atomic void clear_name_for_object(object ob)`

KERNEL()-gated destruct-time cleanup: looks up and clears whatever name `ob` holds.

### `object query_object(string name)` / `string query_name(object ob)`

Public reads: name to object (nil if unregistered or the name is a folder), and object to name (nil if none).

### `mapping query_tree()` / `string *query_subdirs(varargs string path)` / `string *query_objects(varargs string path)`

Public introspection over the name tree: the full nested mapping, the folder names under `path`, and the object names under `path` (nil or absent `path` means the root).

## Where to next

- [`docs/code-lifecycle.md`](code-lifecycle.md): the object-manager event surface objectd records and the upgrade model upgraded drives.
- [`docs/operations.md`](operations.md): the logging pipeline and error dispatch these daemons implement.
- [`docs/capability.md`](capability.md): the capability model behind capabilityd's store.
- [`docs/kernel-reference.md`](kernel-reference.md): the "Where signatures live" router for every other kind of callable.
