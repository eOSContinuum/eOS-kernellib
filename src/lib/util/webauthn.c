# include <type.h>
# include <identityd.h>

/*
 * WebAuthn ceremony verification (W3C Web Authentication Level 2), as
 * pure functions over caller-supplied payloads: no challenge state, no
 * credential store, no side effects. The caller (the platform ceremony
 * daemon, or a test driver) supplies the relying-party id, the origin,
 * and the challenge it issued, plus the client's raw payloads; these
 * functions either return the parsed result or raise a "webauthn: ..."
 * error naming the first check that failed.
 *
 * Scope matches the platform's TOFU posture: attestation format "none"
 * only (registration trusts the first key it sees; there is no
 * attestation chain to verify), ES256 and Ed25519 credentials (the
 * algorithms the host crypto module verifies natively). Signature and
 * hashing kfuns need that module; without it verification errors.
 */

private inherit cbor "/lib/util/cbor";
private inherit cose "/lib/util/cose";
private inherit json "/lib/util/json";

# define WA_FLAG_UP	0x01	/* user present */
# define WA_FLAG_UV	0x04	/* user verified */
# define WA_FLAG_AT	0x40	/* attested credential data present */
# define WA_FLAG_ED	0x80	/* extension data present */

/*
 * collectedClientData checks shared by both ceremonies (section 5.8.1):
 * JSON with the expected type, the issued challenge (base64url string
 * compare), and the exact origin
 */
private void checkClientData(string clientDataJSON, string expectedType,
			     string challenge, string origin)
{
    mixed data;

    data = json::decode(clientDataJSON);
    if (typeof(data) != T_MAPPING) {
	error("webauthn: clientDataJSON is not a JSON object");
    }
    if (data["type"] != expectedType) {
	error("webauthn: clientData type mismatch");
    }
    if (data["challenge"] != challenge) {
	error("webauthn: challenge mismatch");
    }
    if (data["origin"] != origin) {
	error("webauthn: origin mismatch");
    }
}

/*
 * the authenticator-data prefix shared by both ceremonies (section
 * 6.1): 32-byte rpIdHash checked against SHA-256(rpId), a flags byte
 * that must carry user-present, and the big-endian signature counter
 */
private void checkAuthDataPrefix(string authData, string rpId)
{
    if (strlen(authData) < 37) {
	error("webauthn: authenticator data truncated");
    }
    if (authData[.. 31] != hash_string("SHA256", rpId)) {
	error("webauthn: rpIdHash mismatch");
    }
    if (!(authData[32] & WA_FLAG_UP)) {
	error("webauthn: user-present flag missing");
    }
}

private int signCount(string authData)
{
    return (authData[33] << 24) | (authData[34] << 16) |
	   (authData[35] << 8) | authData[36];
}

/*
 * registration (section 7.1, the TOFU subset): verify the client data
 * and the attestation object, and return the attested credential as a
 * mapping over the identityd row keys plus "credentialId" -- the raw
 * credential id the caller binds under. fmt must be "none" with an
 * empty attStmt; the authenticator data must carry attested credential
 * data for a key type /lib/util/cose supports.
 */
static mapping verifyRegistration(string rpId, string origin,
				  string challenge, string clientDataJSON,
				  string attestationObject)
{
    mixed attestation, attStmt, coseKey;
    string authData, credentialId, *verifier;
    int flags, idLength, offset;
    mixed *sub;

    checkClientData(clientDataJSON, "webauthn.create", challenge, origin);

    attestation = cbor::decode(attestationObject);
    if (typeof(attestation) != T_MAPPING) {
	error("webauthn: attestation object is not a CBOR map");
    }
    if (attestation["fmt"] != "none") {
	error("webauthn: unsupported attestation format");
    }
    attStmt = attestation["attStmt"];
    if (typeof(attStmt) != T_MAPPING || map_sizeof(attStmt) != 0) {
	error("webauthn: attestation statement not empty");
    }
    authData = attestation["authData"];
    if (typeof(authData) != T_STRING) {
	error("webauthn: authenticator data missing");
    }

    checkAuthDataPrefix(authData, rpId);
    flags = authData[32];
    if (!(flags & WA_FLAG_AT)) {
	error("webauthn: no attested credential data");
    }
    if (strlen(authData) < 55) {
	error("webauthn: attested credential data truncated");
    }
    idLength = (authData[53] << 8) | authData[54];
    if (idLength == 0 || strlen(authData) < 55 + idLength) {
	error("webauthn: credential id truncated");
    }
    credentialId = authData[55 .. 55 + idLength - 1];

    sub = cbor::decodePrefix(authData, 55 + idLength);
    coseKey = sub[0];
    offset = sub[1];
    if (typeof(coseKey) != T_MAPPING) {
	error("webauthn: credential public key is not a COSE map");
    }
    if (offset != strlen(authData) && !(flags & WA_FLAG_ED)) {
	error("webauthn: trailing bytes after credential data");
    }
    verifier = cose::verifyKey(coseKey);

    return ([ "credentialId" : credentialId,
	      CRED_TYPE : CRED_TYPE_PASSKEY,
	      CRED_SCHEME : verifier[0],
	      CRED_KEY : verifier[1],
	      CRED_SIGNCOUNT : signCount(authData),
	      CRED_FLAGS : flags ]);
}

/*
 * assertion (section 7.2): verify the client data, the authenticator
 * data, and the signature over authenticatorData || SHA-256(clientData)
 * with the stored credential's verify scheme and raw public key.
 * Returns the assertion's signature counter; counter policy (replay
 * comparison against the stored value) is the caller's.
 */
static int verifyAssertion(string rpId, string origin, string challenge,
			   string scheme, string key, string clientDataJSON,
			   string authenticatorData, string signature)
{
    string message;
    int valid;

    checkClientData(clientDataJSON, "webauthn.get", challenge, origin);
    checkAuthDataPrefix(authenticatorData, rpId);

    message = authenticatorData + hash_string("SHA256", clientDataJSON);
    catch {
	valid = decrypt(scheme + " verify", key, signature, message);
    } : {
	valid = FALSE;
    }
    if (!valid) {
	error("webauthn: signature invalid");
    }
    return signCount(authenticatorData);
}
