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
 *     principal string would let any caller forge authority. Here a
 *     session is minted only for the principal a ceremony just proved.
 *   - identityd credential mutation (bind/rotate/redeem) and the
 *     capability grant path: those remain operator- and System-tier
 *     concerns.
 *
 * The agent entry points extend the same rule rather than weaken it:
 * an agent ceremony (verified by agentauthd) mints a session only for
 * the proven agent principal, and the controller self-service entries
 * derive the controlling identity from a live session's proven
 * principal -- the new agent's controller edge, and the own-agents
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
# include <identityd.h>

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */

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
 * ({ principal, token }): the new identity's principal string and a
 * live session token (plaintext, the only time it exists). Errors out
 * of webauthnd/identityd propagate to the caller.
 */
mixed *register_identity(string challenge, string clientDataJSON,
			 string attestationObject, varargs int ttl)
{
    string principal, token;

    principal = WEBAUTHND->register_credential(challenge, clientDataJSON,
					       attestationObject);
    token = SESSIOND->mint(principal, ttl);
    return ({ principal, token });
}

/*
 * assertion verification plus session mint in one step. On success
 * returns ({ principal, token }) for the asserted identity.
 */
mixed *authenticate(string challenge, string credentialId,
		    string clientDataJSON, string authenticatorData,
		    string signature, varargs int ttl)
{
    string principal, token;

    principal = WEBAUTHND->verify_assertion(challenge, credentialId,
					    clientDataJSON,
					    authenticatorData, signature);
    token = SESSIOND->mint(principal, ttl);
    return ({ principal, token });
}

/*
 * agent ceremonies: the same ceremony-plus-mint composition for agent
 * principals. agentauthd checks the record's kind and suspended state
 * at ceremony time; a session is minted only for the proven principal.
 */
mixed *authenticate_agent_key(string challenge, string credentialId,
			      string signature, varargs int ttl)
{
    string principal, token;

    principal = AGENTAUTHD->verify_key_assertion(challenge, credentialId,
						 signature);
    token = SESSIOND->mint(principal, ttl);
    return ({ principal, token });
}

mixed *authenticate_agent_token(string agentToken, varargs int ttl)
{
    string principal, token;

    principal = AGENTAUTHD->verify_token(agentToken);
    token = SESSIOND->mint(principal, ttl);
    return ({ principal, token });
}

/*
 * controller self-service: a live session proves the controlling
 * identity. The new agent's controller edge -- and the own-agents
 * constraint on suspend/resume -- is derived from that proven
 * principal, never caller-supplied. identityd enforces that only a
 * human record controls agents, so an agent session cannot pass these
 * entries.
 */
private string session_identity(string sessionToken)
{
    string principal, uuid;

    principal = SESSIOND->validate(sessionToken);
    if (!principal) {
	error("auth: no live session");
    }
    if (sscanf(principal, "identity:%s", uuid) == 0) {
	error("auth: not an identity session");
    }
    return uuid;
}

private void check_own_agent(string controllerUuid, string agentUuid)
{
    object agent;

    agent = IDENTITYD->find_identity(agentUuid);
    if (!agent || agent->query_controller() != controllerUuid) {
	error("auth: no such agent of this controller");
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
 * the principal a live session token authenticates, or nil
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
