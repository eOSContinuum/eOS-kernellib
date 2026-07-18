/*
 * WebAuthn ceremony daemon: the System-tier owner of the platform's
 * passkey ceremonies. Composes the pure verification library
 * (/lib/util/webauthn) with the identity registry (identityd): TOFU
 * registration verifies a foreign attestation payload and mints an
 * identity bound to the new credential in one atomic step; assertion
 * verification checks the signature against the stored credential and
 * enforces the signature-counter replay policy before advancing the
 * stored counter.
 *
 * The daemon holds no challenge state: issue_challenge() returns fresh
 * secure randomness, and the verifying entry points take the expected
 * challenge from the caller -- the session layer that issued it owns
 * it, which is also what lets test vectors pin their challenges.
 *
 * Counter policy (spec section 6.1.1): when either the stored or the
 * asserted counter is nonzero, the asserted counter must be strictly
 * greater than the stored one; a pair of zeros means the authenticator
 * does not implement the counter and is accepted.
 *
 * rpId and origin are operator-configured (the "webauthn" console
 * verb); defaults match the platform's native-TLS shape. The surface
 * is System/kernel-tier like identityd's.
 */

# include <kernel/kernel.h>
# include <kfun.h>
# include <type.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */
private inherit base64 "/lib/util/base64";
private inherit webauthn "/lib/util/webauthn";

private string rpId;	/* relying-party id the ceremonies verify against */
private string origin;	/* web origin the client data must carry */

static void create()
{
    ::create();
    rpId = "localhost";
    origin = "https://localhost:8443";
    sysLog("webauthn: ceremony daemon up (rpId " + rpId + ")");
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
 * a fresh single-use challenge, base64url; the caller stores it and
 * passes it back to the verifying entry points
 */
string issue_challenge()
{
    check_system(previous_program());
# ifdef KF_SECURE_RANDOM
    return base64::urlEncode(secure_random(32));
# else
    error("webauthn: crypto module not loaded");
# endif
}

string query_rp_id()
{
    check_system(previous_program());
    return rpId;
}

string query_origin()
{
    check_system(previous_program());
    return origin;
}

void configure(string id, string org)
{
    check_system(previous_program());
    if (id) {
	rpId = id;
    }
    if (org) {
	origin = org;
    }
}

/*
 * registration-ceremony verification without a mint: verify the
 * payloads against the challenge the caller issued and return the
 * credential row, keyed under "credentialId" (base64url, the form the
 * store binds). What happens to the verified row is the System-tier
 * caller's composition: register_credential below mints a fresh
 * identity (TOFU), the recovery ceremony pairs it with a code
 * redemption onto an existing record, and the operator bind verb
 * attaches it directly -- never-bare-re-bind holds because no caller
 * composes a bare re-bind out of the registration route.
 */
mapping verify_registration_payload(string challenge, string clientDataJSON,
				    string attestationObject)
{
    mapping parsed, row;

    check_system(previous_program());
    parsed = webauthn::verifyRegistration(rpId, origin, challenge,
					  clientDataJSON, attestationObject);
    row = parsed + ([ ]);
    row["credentialId"] = base64::urlEncode(parsed["credentialId"]);
    row[CRED_CREATED] = time();
    return row;
}

/*
 * TOFU registration: verify the payloads, then mint an identity whose
 * first credential is the attested passkey (identityd refuses a
 * credential id bound anywhere else -- never bare TOFU re-bind).
 * Returns the new principal string.
 */
string register_credential(string challenge, string clientDataJSON,
			   string attestationObject)
{
    mapping row;
    string credentialId, uuid;

    check_system(previous_program());
    row = verify_registration_payload(challenge, clientDataJSON,
				      attestationObject);
    credentialId = row["credentialId"];
    row["credentialId"] = nil;
    uuid = IDENTITYD->create_identity(credentialId, row);
    return "identity:" + uuid;
}

/*
 * assertion: look the credential up, verify the signature, enforce the
 * counter policy, advance the stored counter. Returns the principal.
 */
string verify_assertion(string challenge, string credentialId,
			string clientDataJSON, string authenticatorData,
			string signature)
{
    object identity;
    mapping row;
    string uuid;
    int stored, asserted;

    check_system(previous_program());
    uuid = IDENTITYD->find_by_credential(credentialId);
    if (!uuid) {
	error("webauthn: unknown credential");
    }
    identity = IDENTITYD->find_identity(uuid);
    row = identity->query_credential(credentialId);
    if (!row || row[CRED_TYPE] != CRED_TYPE_PASSKEY) {
	error("webauthn: unknown credential");
    }
    asserted = webauthn::verifyAssertion(rpId, origin, challenge,
					 row[CRED_SCHEME], row[CRED_KEY],
					 clientDataJSON, authenticatorData,
					 signature);
    stored = row[CRED_SIGNCOUNT];
    if ((stored != 0 || asserted != 0) && asserted <= stored) {
	error("webauthn: signCount replay");
    }
    IDENTITYD->update_sign_count(uuid, credentialId, asserted);
    return "identity:" + uuid;
}


/*
 * NAME:	_emit()
 * DESCRIPTION:	route operator-verb output through the console user
 */
private void _emit(object user, string msg)
{
    if (user) {
	user->message(msg);
    }
}

/*
 * NAME:	cmd_webauthn()
 * DESCRIPTION:	the webauthn operator verb, dispatched by the kernel
 *		admin-console registry:
 *		  webauthn                 -- status
 *		  webauthn rpid <id>       -- set the relying-party id
 *		  webauthn origin <url>    -- set the expected origin
 *		  webauthn register <challenge> <clientDataJSON-b64u>
 *		           <attestationObject-b64u>
 *		  webauthn authenticate <challenge> <credentialId-b64u>
 *		           <clientDataJSON-b64u> <authenticatorData-b64u>
 *		           <signature-b64u>
 */
void cmd_webauthn(object user, string cmd, string str)
{
    string *parts, err, principal, uuid;
    int count;

    if (!KERNEL()) {
	error("Access denied");
    }

    parts = str ? explode(str, " ") - ({ "" }) : ({ });
    if (sizeof(parts) == 0) {
	_emit(user, "webauthn: rpId " + rpId + "\n" +
		    "webauthn: origin " + origin + "\n" +
		    "webauthn: crypto module: " +
# ifdef KF_SECURE_RANDOM
		    "present"
# else
		    "absent (ceremonies unavailable)"
# endif
		    + "\n");
	return;
    }

    switch (parts[0]) {
    case "rpid":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " rpid <relying-party-id>\n");
	    return;
	}
	configure(parts[1], nil);
	_emit(user, "webauthn: rpId " + rpId + "\n");
	return;

    case "origin":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " origin <url>\n");
	    return;
	}
	configure(nil, parts[1]);
	_emit(user, "webauthn: origin " + origin + "\n");
	return;

    case "register":
	if (sizeof(parts) != 4) {
	    _emit(user, "usage: " + cmd + " register <challenge> " +
			"<clientDataJSON-b64u> <attestationObject-b64u>\n");
	    return;
	}
	err = catch(principal = register_credential(parts[1],
			base64::urlDecode(parts[2]),
			base64::urlDecode(parts[3])));
	_emit(user, err ? err + "\n"
			: "webauthn: registered " + principal + "\n");
	return;

    case "authenticate":
	if (sizeof(parts) != 6) {
	    _emit(user, "usage: " + cmd + " authenticate <challenge> " +
			"<credentialId-b64u> <clientDataJSON-b64u> " +
			"<authenticatorData-b64u> <signature-b64u>\n");
	    return;
	}
	err = catch(principal = verify_assertion(parts[1], parts[2],
			base64::urlDecode(parts[3]),
			base64::urlDecode(parts[4]),
			base64::urlDecode(parts[5])),
		    uuid = IDENTITYD->find_by_credential(parts[2]),
		    count = IDENTITYD->find_identity(uuid)->
			    query_credential(parts[2])[CRED_SIGNCOUNT]);
	_emit(user, err ? err + "\n"
			: "webauthn: authenticated " + principal +
			  " signCount " + (string) count + "\n");
	return;

    default:
	_emit(user, "usage: " + cmd + " [rpid <id> | origin <url> | " +
		    "register ... | authenticate ...]\n");
	return;
    }
}
