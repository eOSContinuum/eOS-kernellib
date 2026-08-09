/*
 * authd: the transport authentication facade.
 *
 * The identity substrate's daemons (identityd, webauthnd, sessiond) are
 * System/kernel-tier surfaces: every public entry checks the caller's
 * program against /usr/System and /kernel. A tier-E transport surface --
 * an HTTP application server binding a login flow, a non-HTTP protocol
 * doing the same -- therefore cannot consume the substrate directly.
 * This facade is the seam: it exposes exactly the ceremony-plus-session
 * flow a transport needs, and nothing else.
 *
 * What it deliberately does NOT expose:
 *
 *   - sessiond->mint(principal): minting a session for an arbitrary
 *     subject string would let any caller forge authority. Here a
 *     session is minted only for the subject a ceremony just proved.
 *   - identityd credential mutation (bind/rotate/redeem) and the
 *     capability grant path: those remain operator- and System-tier
 *     concerns.
 *
 * The agent entry points extend the same rule rather than weaken it:
 * an agent ceremony (verified by agentauthd) mints a session only for
 * the agent's proven subject, and the controller self-service entries
 * derive the controlling identity from a live session's proven
 * subject -- the new agent's controller edge, and the own-agents
 * constraint on suspend/resume, are never caller-supplied.
 *
 * Challenge ownership follows the webauthnd contract: the daemon holds
 * no challenge state, so the layer that issues a challenge owns it. A
 * transport application calls issue_challenge(), stores the value
 * single-use, and passes it back with the ceremony payload; replay
 * discipline on the challenge is the application's. The composite
 * example (examples/composite-app) carries the reference form of that
 * store.
 *
 * All inputs are raw strings, exactly as the underlying daemons take
 * them: clientDataJSON is the JSON text, attestationObject and
 * authenticatorData and signature are raw bytes, credentialId is
 * base64url (the form it is bound under). A wire format that transports
 * binary fields base64url-encoded decodes them before calling in.
 */

# include <kernel/kernel.h>
# include <kernel/capability.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";
inherit "/kernel/lib/capability";
private inherit "/lib/util/lpc";	/* sysLog */

/* the capability the session-administration entries require of the
 * caller's own proven subject, granted through the operator path
 * (identity grant <uuid> session.admin) */
# define SESSION_ADMIN_CAP	"session.admin"

static void create()
{
    ::create();
    sysLog("auth: transport authentication facade up");
}

/*
 * a fresh single-use challenge (base64url). The caller owns it: store
 * it, hand it to the client, accept it back exactly once.
 */
string issue_challenge()
{
    return WEBAUTHND->issue_challenge();
}

/*
 * TOFU registration plus session mint in one step. On success returns
 * ({ subject, token }): the new identity's subject string
 * ("identity:<uuid>", the form the capability store records as a
 * principal) and a live session token (plaintext, the only time it
 * exists). Errors out of webauthnd/identityd propagate to the caller.
 */
mixed *register_identity(string challenge, string clientDataJSON,
			 string attestationObject, varargs int ttl)
{
    string subject, token;

    subject = WEBAUTHND->register_credential(challenge, clientDataJSON,
					     attestationObject);
    token = SESSIOND->mint(subject, ttl);
    return ({ subject, token });
}

/*
 * assertion verification plus session mint in one step. On success
 * returns ({ subject, token }) for the asserted identity.
 */
mixed *authenticate(string challenge, string credentialId,
		    string clientDataJSON, string authenticatorData,
		    string signature, varargs int ttl)
{
    string subject, token;

    subject = WEBAUTHND->verify_assertion(challenge, credentialId,
					  clientDataJSON,
					  authenticatorData, signature);
    token = SESSIOND->mint(subject, ttl);
    return ({ subject, token });
}

/*
 * agent ceremonies: the same ceremony-plus-mint composition for agent
 * identities. agentauthd checks the record's kind and suspended state
 * at ceremony time; a session is minted only for the proven subject.
 */
mixed *authenticate_agent_key(string challenge, string credentialId,
			      string signature, varargs int ttl)
{
    string subject, token;

    subject = AGENTAUTHD->verify_key_assertion(challenge, credentialId,
					       signature);
    token = SESSIOND->mint(subject, ttl);
    return ({ subject, token });
}

mixed *authenticate_agent_token(string agentToken, varargs int ttl)
{
    string subject, token;

    subject = AGENTAUTHD->verify_token(agentToken);
    token = SESSIOND->mint(subject, ttl);
    return ({ subject, token });
}

/*
 * the recovery ceremony: both proofs in one shot. The registration
 * payload for the NEW passkey is verified without a mint, then the
 * substrate redeems the recovery code and binds the verified
 * credential as one atomic operation (valid even on the record's last
 * credential), then a session is minted for the recovered identity.
 * A wrong code binds nothing, a bad attestation redeems nothing, and
 * there is no intermediate recovery state to hijack. Never-bare-
 * re-bind holds: the only path onto an existing record still requires
 * the account proof.
 */
mixed *recover_identity(string uuid, string code, string challenge,
			string clientDataJSON, string attestationObject,
			varargs int ttl)
{
    mapping row;
    string credentialId, subject, token;

    row = WEBAUTHND->verify_registration_payload(challenge, clientDataJSON,
						 attestationObject);
    credentialId = row["credentialId"];
    row["credentialId"] = nil;
    IDENTITYD->redeem_and_replace(uuid, code, credentialId, row);
    subject = "identity:" + uuid;
    token = SESSIOND->mint(subject, ttl);
    return ({ subject, token });
}


/*
 * controller self-service: a live session proves the controlling
 * identity. The new agent's controller edge -- and the own-agents
 * constraint on suspend/resume -- is derived from that proven
 * subject, never caller-supplied. identityd enforces that only a
 * human record controls agents, so an agent session cannot pass these
 * entries.
 */
private string session_identity(string sessionToken)
{
    string subject, uuid;

    subject = SESSIOND->validate(sessionToken);
    if (!subject) {
	error("auth: no live session");
    }
    if (sscanf(subject, "identity:%s", uuid) == 0) {
	error("auth: not an identity session");
    }
    return uuid;
}

private void check_own_agent(string controllerUuid, string agentUuid)
{
    object agent;

    agent = IDENTITYD->find_identity(agentUuid);
    if (!agent || agent->query_controller() != controllerUuid) {
	error("auth: no such agent of this principal");
    }
}

/*
 * mint an agent controlled by the session's identity, with a
 * caller-supplied agent-key credential row; returns the agent's uuid
 */
string mint_agent(string sessionToken, string credentialId, mapping row)
{
    return IDENTITYD->mint_agent(session_identity(sessionToken),
				 credentialId, row);
}

/*
 * mint an agent controlled by the session's identity, with a fresh
 * platform-generated agent token; returns ({ uuid, token }) -- the
 * only time the plaintext exists
 */
string *mint_agent_with_token(string sessionToken, varargs int ttl)
{
    return IDENTITYD->mint_agent_with_token(session_identity(sessionToken),
					    ttl);
}

/*
 * suspend one of the session identity's own agents; live sessions of
 * the agent die now. Returns the count of sessions revoked.
 */
int suspend_agent(string sessionToken, string agentUuid)
{
    string controllerUuid;

    controllerUuid = session_identity(sessionToken);
    check_own_agent(controllerUuid, agentUuid);
    return IDENTITYD->suspend_agent(agentUuid);
}

/*
 * resume one of the session identity's own agents: restores the
 * ability to authenticate only
 */
void resume_agent(string sessionToken, string agentUuid)
{
    string controllerUuid;

    controllerUuid = session_identity(sessionToken);
    check_own_agent(controllerUuid, agentUuid);
    IDENTITYD->resume_agent(agentUuid);
}

/*
 * delegate one of the session identity's own capabilities to one of
 * its own agents, or withdraw the delegation. The substrate checks --
 * against live state, atomically -- that the delegator holds the
 * capability, the capability is operator-flagged delegable, the
 * target is the delegator's own unsuspended agent.
 */
void delegate_capability(string sessionToken, string agentUuid,
			 string capability)
{
    IDENTITYD->delegate_capability(session_identity(sessionToken),
				   agentUuid, capability);
}

void undelegate_capability(string sessionToken, string agentUuid,
			   string capability)
{
    IDENTITYD->undelegate_capability(session_identity(sessionToken),
				     agentUuid, capability);
}

/*
 * the session identity's own agents, read-only: one row per agent,
 * ({ uuid, suspended, delegated capabilities }). The controller is
 * derived from the live session, so a caller can only ever see its
 * own; the row carries record state, never credential material.
 */
mixed *query_agents(string sessionToken)
{
    string controllerUuid, *uuids;
    mixed *rows;
    object agent;
    int i, sz;

    controllerUuid = session_identity(sessionToken);
    uuids = IDENTITYD->query_agents(controllerUuid);
    sz = sizeof(uuids);
    rows = allocate(sz);
    for (i = 0; i < sz; i++) {
	agent = IDENTITYD->find_identity(uuids[i]);
	rows[i] = ({ uuids[i], agent->query_suspended(),
		     map_indices(IDENTITYD->query_delegations(uuids[i])) });
    }
    return rows;
}

/*
 * self-service recovery-code provisioning: a live identity session
 * replaces the record's recovery-code set with n fresh codes and
 * returns the plaintext -- the only time it exists. Without this
 * entry a transport-registered identity would have no codes and so
 * no self-service recovery path.
 */
string *rotate_recovery_codes(string sessionToken, int n)
{
    return IDENTITYD->rotate_recovery_codes(session_identity(sessionToken),
					    n);
}

/*
 * the session identity's own passkeys, read-only: one row per passkey
 * credential, ({ credentialId, created, lastUsed }). Derived from the
 * live session, so a caller only ever sees its own; the rows carry
 * bookkeeping, never key material.
 */
mixed *query_passkeys(string sessionToken)
{
    string *ids;
    object identity;
    mapping row;
    mixed *rows;
    int i;

    identity = IDENTITYD->find_identity(session_identity(sessionToken));
    ids = identity->query_credential_ids();
    rows = ({ });
    for (i = 0; i < sizeof(ids); i++) {
	row = identity->query_credential(ids[i]);
	if (row[CRED_TYPE] == CRED_TYPE_PASSKEY) {
	    rows += ({ ({ ids[i], row[CRED_CREATED],
			  row[CRED_LASTUSED] }) });
	}
    }
    return rows;
}

/*
 * add-passkey enrollment: a live session binds an ADDITIONAL passkey
 * to its own record -- the second-device path, so routine device
 * addition never rides the recovery ceremony. The registration
 * payload is verified exactly as recovery's is (verify without a
 * mint); the substrate's bind rules do the refusing (a
 * globally-used credential id, or a human row on an agent record).
 * No session is minted: enrollment adds a credential, not
 * authentication.
 */
string enroll_passkey(string sessionToken, string challenge,
		      string clientDataJSON, string attestationObject)
{
    string uuid, credentialId;
    mapping row;

    uuid = session_identity(sessionToken);
    row = WEBAUTHND->verify_registration_payload(challenge, clientDataJSON,
						 attestationObject);
    credentialId = row["credentialId"];
    row["credentialId"] = nil;
    IDENTITYD->bind_credential(uuid, credentialId, row);
    return credentialId;
}

/*
 * self-service passkey revocation: a live session removes ONE of its
 * own passkey credentials -- the lost or superseded device. Refuses
 * non-passkey rows (recovery codes rotate as a set through
 * rotate_recovery_codes) and the record's last passkey, so a
 * principal never revokes itself out of login; the substrate's own
 * never-zero guard backs this at the record level. Revocation removes
 * the credential binding only -- live sessions are separate state and
 * die by logout or expiry.
 */
void revoke_passkey(string sessionToken, string credentialId)
{
    string uuid, *ids;
    object identity;
    mapping row;
    int i, passkeys;

    uuid = session_identity(sessionToken);
    identity = IDENTITYD->find_identity(uuid);
    row = identity->query_credential(credentialId);
    if (!row || row[CRED_TYPE] != CRED_TYPE_PASSKEY) {
	error("auth: no such passkey on this identity");
    }
    ids = identity->query_credential_ids();
    for (i = 0; i < sizeof(ids); i++) {
	if (identity->query_credential(ids[i])[CRED_TYPE] ==
						CRED_TYPE_PASSKEY) {
	    passkeys++;
	}
    }
    if (passkeys <= 1) {
	error("auth: cannot revoke the last passkey");
    }
    IDENTITYD->unbind_credential(uuid, credentialId);
}

/*
 * Session administration: bookkeeping and revocation for subjects
 * OTHER than the caller -- the moderation shape (boot a member,
 * logout-everywhere) an application cannot otherwise build without
 * holding another identity's plaintext token against the platform's
 * no-plaintext-at-rest posture. Every entry derives the acting
 * subject from its own live session, then requires the session.admin
 * capability for that subject AT CALL TIME -- the check hits the live
 * capability store on every call, nothing cached, the same
 * revocation-honest posture the two-stage credential doctrine
 * demands. This deliberately generalizes the suspend_agent shape (an
 * entry that ends another record's sessions) past the controller
 * edge: where suspend_agent's authority is the immutable controller
 * relationship, this authority is an explicit, operator-granted,
 * operator-revocable capability.
 *
 * Rows carry the bookkeeping id (the stored token hash: it can
 * revoke, never authenticate -- validation hashes a presented
 * plaintext), mint time, and expiry. A plaintext token never crosses
 * this surface in either direction.
 */
private string admin_subject(string sessionToken)
{
    string subject;

    subject = SESSIOND->validate(sessionToken);
    if (!subject) {
	error("auth: no live session");
    }
    require_member(SESSION_ADMIN_CAP, subject);
    return subject;
}

/*
 * a subject's live sessions, read-only: one row per session,
 * ({ sessionId, created, expires })
 */
mixed *query_subject_sessions(string sessionToken, string subject)
{
    admin_subject(sessionToken);
    return SESSIOND->query_principal_sessions(subject);
}

/*
 * revoke ONE of a subject's live sessions by its bookkeeping id;
 * TRUE iff a live session with that id belonged to the subject and
 * was removed (the id-to-subject binding is checked in sessiond, so
 * an id learned from one listing cannot revoke another subject's
 * session)
 */
int revoke_subject_session(string sessionToken, string subject,
			   string sessionId)
{
    admin_subject(sessionToken);
    return SESSIOND->revoke_session_id(subject, sessionId);
}

/*
 * revoke ALL of a subject's live sessions (the boot / logout-
 * everywhere primitive); returns the count removed
 */
int revoke_subject_sessions(string sessionToken, string subject)
{
    admin_subject(sessionToken);
    return SESSIOND->revoke_principal(subject);
}

/*
 * the subject a live session token authenticates, or nil
 */
string validate(string token)
{
    return SESSIOND->validate(token);
}

/*
 * drop the token's session; TRUE iff a live one was removed
 */
int logout(string token)
{
    return SESSIOND->revoke(token);
}

/*
 * self-service logout-everywhere: revoke every OTHER live session for
 * the subject the presented token proves, sparing the session that
 * presents it. Possession of a live token is the whole authority --
 * the sweep never leaves the caller's own subject, so no capability
 * applies; acting on ANOTHER subject's sessions is the administration
 * surface above, behind session.admin. Returns the count removed, or
 * -1 when the token proves nothing.
 */
int revoke_other_sessions(string token)
{
    return SESSIOND->revoke_other_sessions(token);
}
