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
 *   - identityd mutation (bind/rotate/redeem) and the capability grant
 *     path: those remain operator- and System-tier concerns.
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

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */

# define WEBAUTHND	"/usr/System/sys/webauthnd"
# define SESSIOND	"/usr/System/sys/sessiond"

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
