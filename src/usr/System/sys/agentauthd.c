/*
 * agentauthd: the agent ceremony daemon. Verifies the two agent
 * authentication ceremonies against the identity substrate and returns
 * the proven principal; it never mints sessions (authd composes
 * ceremony + mint) and never mutates records beyond last-use
 * bookkeeping.
 *
 *   - key assertion: look the credential up via identityd's global
 *     index, check the record is an unsuspended agent, and verify the
 *     signature over the domain-separated message (AGENT_AUTH_DOMAIN +
 *     challenge) with the row's verify scheme and raw public key. The
 *     domain tag means a signature produced for agent authentication
 *     can never be replayed into another protocol, and vice versa.
 *     There is no signature counter: replay protection is the
 *     caller-owned single-use challenge (WebAuthn's counter exists
 *     because authenticator hardware is cloneable; an agent key is
 *     just a key).
 *
 *   - token presentation: hash the presented token, look the row up by
 *     the derived credential id, and require an unsuspended agent
 *     record, a matching stored hash, and an unexpired row. Expiry is
 *     required on every agent-token row, so there is no unexpiring
 *     bearer path.
 *
 * Both ceremonies check kind and suspended state at ceremony time:
 * every authentication decision is made against live platform state,
 * never against anything cached in a credential.
 *
 * Challenge ownership follows the webauthnd contract: the daemon holds
 * no challenge state; the layer that issues a challenge stores it
 * single-use and passes it back with the assertion.
 *
 * Hashing and signature verification need the host crypto module;
 * without it the daemon boots, reports the stand-down, and refuses
 * ceremonies cleanly. The verifying surface is System/kernel-tier;
 * transports reach it through the authd facade.
 */

# include <kernel/kernel.h>
# include <kfun.h>
# include <type.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */
private inherit base64 "/lib/util/base64";
private inherit hex "/lib/util/hex";

static void create()
{
    ::create();
    sysLog("agentauth: ceremony daemon up; crypto module " +
# ifdef KF_SECURE_RANDOM
	   "present"
# else
	   "absent (ceremonies unavailable)"
# endif
	   );
}

/*
 * gate: the caller's program, captured at the public entry
 */
private void check_system(string caller)
{
    if (sscanf(caller, "/usr/System/%*s") == 0 &&
	sscanf(caller, "/kernel/%*s") == 0) {
	error("Access denied");
    }
}

/*
 * a fresh single-use challenge (base64url). The caller owns it: store
 * it, hand it to the agent, accept it back exactly once.
 */
string issue_challenge()
{
# ifdef KF_SECURE_RANDOM
    return base64::urlEncode(secure_random(32));
# else
    error("agentauth: crypto module not loaded");
# endif
}

/*
 * the ceremony-time record checks shared by both ceremonies: the
 * credential must belong to an agent record that is not suspended
 */
private object need_live_agent(string uuid)
{
    object identity;

    identity = IDENTITYD->find_identity(uuid);
    if (!identity || identity->query_kind() != ID_KIND_AGENT) {
	error("agentauth: not an agent credential");
    }
    if (identity->query_suspended()) {
	error("agentauth: agent suspended");
    }
    return identity;
}

/*
 * key assertion: verify a signature over AGENT_AUTH_DOMAIN + challenge
 * with the bound agent-key row; returns the proven principal
 */
string verify_key_assertion(string challenge, string credentialId,
			    string signature)
{
    string uuid;
    object identity;
    mapping row;
    int valid;

    check_system(previous_program());
    if (!challenge || strlen(challenge) == 0 || !credentialId ||
	!signature) {
	error("agentauth: bad assertion");
    }
    uuid = IDENTITYD->find_by_credential(credentialId);
    if (!uuid) {
	error("agentauth: unknown credential");
    }
    identity = need_live_agent(uuid);
    row = identity->query_credential(credentialId);
    if (row[CRED_TYPE] != CRED_TYPE_AGENT_KEY) {
	error("agentauth: not an agent key");
    }
    catch {
	valid = decrypt(row[CRED_SCHEME] + " verify", row[CRED_KEY],
			signature, AGENT_AUTH_DOMAIN + challenge);
    } : {
	valid = FALSE;
    }
    if (!valid) {
	error("agentauth: signature invalid");
    }
    IDENTITYD->touch_credential(uuid, credentialId);
    return identity->query_principal();
}

/*
 * token presentation: hash lookup plus expiry and record checks;
 * returns the proven principal
 */
string verify_token(string token)
{
    string uuid, hash, id;
    object identity;
    mapping row;

    check_system(previous_program());
    if (!token || strlen(token) == 0) {
	error("agentauth: bad token");
    }
# ifdef KF_SECURE_RANDOM
    hash = hex::format(hash_string("SHA256", token));
# else
    error("agentauth: crypto module not loaded");
# endif
    id = AGENT_TOKEN_ID(hash);
    uuid = IDENTITYD->find_by_credential(id);
    if (!uuid) {
	error("agentauth: unknown token");
    }
    identity = need_live_agent(uuid);
    row = identity->query_credential(id);
    if (row[CRED_TYPE] != CRED_TYPE_AGENT_TOKEN || row[CRED_HASH] != hash) {
	error("agentauth: unknown token");
    }
    if (row[CRED_EXPIRES] <= time()) {
	error("agentauth: token expired");
    }
    IDENTITYD->touch_credential(uuid, id);
    return identity->query_principal();
}
