/*
 * HTTP handler for the composite example's Inventory domain.
 *
 * Registered with the WWW route registry under BOTH the /inventory and
 * /auth prefixes (one domain may own several route prefixes). Receives
 * the narrow handler contract from the WWW servers:
 *
 *   handle(method, path, body, authorization)
 *     -> ({ code, phrase, contentType, body })
 *
 * and binds the platform's authentication substrate to the wire:
 *
 *   - /auth/challenge issues a WebAuthn challenge through the authd
 *     facade and records it in a single-use store. Challenge ownership
 *     is the application's (the webauthnd contract): a ceremony
 *     payload must present a challenge this handler issued and not yet
 *     consumed, so a replayed ceremony dies here before any
 *     cryptography runs.
 *   - /auth/register and /auth/login run the ceremonies through authd
 *     and hand the client a bearer session token, minted by the
 *     platform only for a ceremony-proven principal.
 *   - Authenticated routes parse "Authorization: Bearer <token>" and
 *     validate through authd; the validated principal is what the
 *     domain's authorization (inventoryd) decides against.
 *   - /auth/agents binds authd's controller self-service: a live
 *     identity session mints agent principals (the agent token is in
 *     the response once, at mint, the only time the plaintext exists),
 *     lists its own agents, suspends and resumes them, and delegates
 *     or withdraws its own capabilities. Management refusals surface
 *     the substrate's reason -- the caller is authenticated and asking
 *     about its own records; ceremony refusals below stay uniform.
 *   - /auth/agent-login runs the agent-token ceremony: the token is
 *     the proof, so there is no challenge round-trip.
 *   - /auth/recover runs the recovery ceremony: a recovery code plus
 *     a NEW passkey's registration payload in one request, composed
 *     atomically behind authd (redeem + bind + session mint). Its
 *     challenge must come from /auth/recover-challenge -- the store
 *     tags every challenge with the purpose it was issued for.
 *     /auth/recovery-codes (bearer) provisions the codes a
 *     self-service recovery later depends on; the response is the
 *     only time the plaintext exists.
 *   - The event streams: GET /inventory/events (public, like the audit
 *     read) and GET /auth/agents/stream?token=<session> subscribe the
 *     connection with the SSE broker (sys/streamd) and return the
 *     streaming sentinel the WWW servers act on. ?heartbeat=1 on the
 *     audit stream opts the subscriber into the broker's server-pushed
 *     tick events. The agent stream authenticates by token in the
 *     query string because EventSource cannot set an Authorization
 *     header; the token is validated here at subscribe time and
 *     re-validated by the broker's poll, and a production surface
 *     would prefer a cookie-bound session to keep tokens out of
 *     request logs.
 *
 * Wire format: JSON bodies; WebAuthn binary fields (clientDataJSON,
 * attestationObject, authenticatorData, signature) travel
 * base64url-encoded and are decoded here to the raw forms the platform
 * daemons take. credentialId stays base64url -- that is the form the
 * substrate binds it under.
 *
 * arm_challenge() is the driver's test seam: it lets the same-domain
 * test driver plant the fixed challenge a foreign-generated ceremony
 * vector embeds. It is gated to callers from this domain and would not
 * exist in a production handler.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/System/lib/auto";
private inherit json "/lib/util/json";
private inherit base64 "/lib/util/base64";

# define AUTHD		"/usr/System/sys/authd"
# define INVENTORYD	"/usr/Inventory/sys/inventoryd"
# define STREAMD	"/usr/Inventory/sys/streamd"
# define DEMO_PROVISIOND	"/usr/System/sys/demo_provisiond"

# define STREAM_SENTINEL	({ 200, "OK", "text/event-stream", nil })

private mapping pendingChallenges;	/* challenge : purpose */

static void create()
{
    ::create();
    pendingChallenges = ([ ]);
}

/*
 * single-use challenge store: issue records, consume removes. Each
 * challenge carries the purpose it was issued for ("webauthn" for the
 * register/login ceremonies, which WebAuthn's clientDataJSON type
 * field already separates cryptographically; "recover" for the
 * recovery ceremony), and consumption checks it -- a challenge is
 * only ever spendable on the route family that issued it. The
 * reference discipline for an application running more than one
 * ceremony kind over one store.
 */
private string issue_challenge(string purpose)
{
    string challenge;

    challenge = AUTHD->issue_challenge();
    pendingChallenges[challenge] = purpose;
    return challenge;
}

private int consume_challenge(string challenge, string purpose)
{
    if (challenge && pendingChallenges[challenge] == purpose) {
	pendingChallenges[challenge] = nil;
	return TRUE;
    }
    return FALSE;
}

/*
 * test seam: plant a known challenge (foreign ceremony vectors embed a
 * fixed one), with an optional purpose (default "webauthn"). Same-
 * domain callers only; not a production surface.
 */
void arm_challenge(string challenge, varargs string purpose)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    pendingChallenges[challenge] = purpose ? purpose : "webauthn";
}

/*
 * the bearer token in an Authorization header value, or nil
 */
private string bearer_token(string authorization)
{
    string token;

    if (authorization && sscanf(authorization, "Bearer %s", token) != 0 &&
	token != "") {
	return token;
    }
    return nil;
}

/*
 * the validated principal for the request, or nil
 */
private string bearer_principal(string authorization)
{
    string token;

    token = bearer_token(authorization);
    return token ? AUTHD->validate(token) : nil;
}

/*
 * response helpers: everything this handler says is JSON
 */
private mixed *respond(int code, string phrase, mapping body)
{
    return ({ code, phrase, "application/json",
	      json::encode(body) + "\n" });
}

private mixed *fail(int code, string phrase, string why)
{
    return respond(code, phrase, ([ "error" : why ]));
}

/*
 * a JSON request body as a mapping, or nil
 */
private mapping parse_body(string body)
{
    mixed parsed;

    if (!body || body == "") {
	return nil;
    }
    if (catch(parsed = json::decode(body)) != nil ||
	typeof(parsed) != T_MAPPING) {
	return nil;
    }
    return parsed;
}

/*
 * Demo seam: when the browser-demo bring-up has compiled the demo
 * provisioner (examples/composite-app/System/demo_provisiond.c, a
 * System-tier stand-in for the operator's grant verb), announce a
 * fresh registration so the demo capability is pre-provisioned. In
 * every headless profile the provisioner is absent and this is a
 * no-op; a provisioner failure never breaks registration itself.
 */
private void announce_registration(string principal)
{
    object provisiond;
    string uuid;

    provisiond = find_object(DEMO_PROVISIOND);
    if (provisiond && principal &&
	sscanf(principal, "identity:%s", uuid) != 0) {
	catch(provisiond->welcome(uuid));
    }
}

/*
 * a base64url field decoded to raw bytes, or nil
 */
private string raw_field(mapping body, string field)
{
    mixed value;
    string raw;

    value = body[field];
    if (typeof(value) != T_STRING || value == "") {
	return nil;
    }
    if (catch(raw = base64::urlDecode(value)) != nil) {
	return nil;
    }
    return raw;
}


private mixed *do_challenge(string purpose)
{
    string challenge;

    if (catch(challenge = issue_challenge(purpose)) != nil) {
	return fail(503, "Service Unavailable", "ceremonies unavailable");
    }
    return respond(200, "OK", ([ "challenge" : challenge ]));
}

private mixed *do_register(string body)
{
    mapping parsed;
    mixed challenge, *result;
    string clientDataJSON, attestationObject;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    challenge = parsed["challenge"];
    clientDataJSON = raw_field(parsed, "clientDataJSON");
    attestationObject = raw_field(parsed, "attestationObject");
    if (typeof(challenge) != T_STRING || !clientDataJSON ||
	!attestationObject) {
	return fail(400, "Bad Request", "missing ceremony fields");
    }
    if (!consume_challenge(challenge, "webauthn")) {
	return fail(400, "Bad Request", "unknown or consumed challenge");
    }
    if (catch(result = AUTHD->register_identity(challenge, clientDataJSON,
						attestationObject)) != nil) {
	return fail(400, "Bad Request", "registration refused");
    }
    announce_registration(result[0]);
    return respond(201, "Created",
		   ([ "principal" : result[0], "token" : result[1] ]));
}

private mixed *do_login(string body)
{
    mapping parsed;
    mixed challenge, credentialId, *result;
    string clientDataJSON, authenticatorData, signature;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    challenge = parsed["challenge"];
    credentialId = parsed["credentialId"];
    clientDataJSON = raw_field(parsed, "clientDataJSON");
    authenticatorData = raw_field(parsed, "authenticatorData");
    signature = raw_field(parsed, "signature");
    if (typeof(challenge) != T_STRING ||
	typeof(credentialId) != T_STRING || !clientDataJSON ||
	!authenticatorData || !signature) {
	return fail(400, "Bad Request", "missing ceremony fields");
    }
    if (!consume_challenge(challenge, "webauthn")) {
	return fail(400, "Bad Request", "unknown or consumed challenge");
    }
    if (catch(result = AUTHD->authenticate(challenge, credentialId,
					   clientDataJSON,
					   authenticatorData,
					   signature)) != nil) {
	return fail(401, "Unauthorized", "assertion refused");
    }
    return respond(200, "OK",
		   ([ "principal" : result[0], "token" : result[1] ]));
}

/*
 * the recovery ceremony: both proofs in one request. The challenge
 * must have been issued for recovery; the code redemption and the
 * new-credential bind are one atomic substrate step behind authd.
 * Refusals are uniform like the login routes -- an unauthenticated
 * caller learns nothing about which proof failed.
 */
private mixed *do_recover(string body)
{
    mapping parsed;
    mixed uuid, code, challenge;
    string clientDataJSON, attestationObject;
    mixed *result;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    uuid = parsed["uuid"];
    code = parsed["code"];
    challenge = parsed["challenge"];
    clientDataJSON = raw_field(parsed, "clientDataJSON");
    attestationObject = raw_field(parsed, "attestationObject");
    if (typeof(uuid) != T_STRING || uuid == "" ||
	typeof(code) != T_STRING || code == "" ||
	typeof(challenge) != T_STRING || !clientDataJSON ||
	!attestationObject) {
	return fail(400, "Bad Request", "missing recovery fields");
    }
    if (!consume_challenge(challenge, "recover")) {
	return fail(400, "Bad Request", "unknown or consumed challenge");
    }
    if (catch(result = AUTHD->recover_identity(uuid, code, challenge,
					       clientDataJSON,
					       attestationObject)) != nil) {
	return fail(401, "Unauthorized", "recovery refused");
    }
    return respond(200, "OK",
		   ([ "principal" : result[0], "token" : result[1] ]));
}

/*
 * self-service recovery-code provisioning: a live session replaces
 * the record's code set; the response is the only time the plaintext
 * exists
 */
private mixed *do_recovery_codes(string body, string authorization)
{
    mapping parsed;
    mixed n;
    string err, *codes;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    n = parsed["n"];
    if (typeof(n) != T_INT || n < 1) {
	return fail(400, "Bad Request", "n (codes to mint) required");
    }
    err = catch(codes = AUTHD->rotate_recovery_codes(
					bearer_token(authorization), n));
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    return respond(201, "Created", ([ "codes" : codes ]));
}

/*
 * the event streams: subscribe the per-connection server clone (the
 * caller of handle()) with the broker, then hand the server the
 * streaming sentinel
 */
private mixed *do_audit_stream(int heartbeat)
{
    STREAMD->subscribe_audit(previous_object(), heartbeat);
    return STREAM_SENTINEL;
}

private mixed *do_agent_stream(string token)
{
    if (!token || token == "" || !AUTHD->validate(token)) {
	return fail(401, "Unauthorized", "live session token required");
    }
    STREAMD->subscribe_agents(previous_object(), token);
    return STREAM_SENTINEL;
}

private mixed *do_agent_login(string body)
{
    mapping parsed;
    mixed agentToken;
    mixed *result;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    agentToken = parsed["token"];
    if (typeof(agentToken) != T_STRING || agentToken == "") {
	return fail(400, "Bad Request", "agent token required");
    }
    if (catch(result = AUTHD->authenticate_agent_token(agentToken)) != nil) {
	return fail(401, "Unauthorized", "agent ceremony refused");
    }
    return respond(200, "OK",
		   ([ "principal" : result[0], "token" : result[1] ]));
}

private mixed *do_mint_agent(string authorization)
{
    string err, *result;

    err = catch(result = AUTHD->mint_agent_with_token(
					bearer_token(authorization)));
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    return respond(201, "Created",
		   ([ "uuid" : result[0], "token" : result[1] ]));
}

private mixed *do_list_agents(string authorization)
{
    mixed *rows, *out;
    string err;
    int i;

    err = catch(rows = AUTHD->query_agents(bearer_token(authorization)));
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    out = allocate(sizeof(rows));
    for (i = 0; i < sizeof(rows); i++) {
	out[i] = ([ "uuid" : rows[i][0], "suspended" : rows[i][1],
		    "capabilities" : rows[i][2] ]);
    }
    return respond(200, "OK", ([ "agents" : out ]));
}

private mixed *do_agent_suspend(string uuid, string authorization)
{
    string err;
    int revoked;

    err = catch(revoked = AUTHD->suspend_agent(bearer_token(authorization),
					       uuid));
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    return respond(200, "OK", ([ "suspended" : uuid,
				 "revoked" : revoked ]));
}

private mixed *do_agent_resume(string uuid, string authorization)
{
    string err;

    err = catch(AUTHD->resume_agent(bearer_token(authorization), uuid));
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    return respond(200, "OK", ([ "resumed" : uuid ]));
}

private mixed *do_agent_delegation(string uuid, string body,
				   string authorization, int grant)
{
    mapping parsed;
    mixed capability;
    string err;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    capability = parsed["capability"];
    if (typeof(capability) != T_STRING || capability == "") {
	return fail(400, "Bad Request", "capability required");
    }
    if (grant) {
	err = catch(AUTHD->delegate_capability(bearer_token(authorization),
					       uuid, capability));
    } else {
	err = catch(AUTHD->undelegate_capability(bearer_token(authorization),
						 uuid, capability));
    }
    if (err != nil) {
	return fail(403, "Forbidden", err);
    }
    return respond(200, "OK",
		   ([ (grant ? "delegated" : "undelegated") : capability,
		      "agent" : uuid ]));
}

private mixed *do_logout(string authorization)
{
    string token;

    token = bearer_token(authorization);
    if (!token) {
	return fail(401, "Unauthorized", "bearer token required");
    }
    return respond(200, "OK", ([ "revoked" : AUTHD->logout(token) ]));
}

private mixed *do_list_items()
{
    mixed *items, *out;
    int i;

    items = INVENTORYD->query_items();
    out = allocate(sizeof(items));
    for (i = 0; i < sizeof(items); i++) {
	out[i] = ([ "id" : items[i][0], "name" : items[i][1],
		    "qty" : items[i][2], "creator" : items[i][3] ]);
    }
    return respond(200, "OK", ([ "items" : out ]));
}

private mixed *do_create_item(string body, string principal)
{
    mapping parsed;
    mixed name, qty;
    int id;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    name = parsed["name"];
    qty = parsed["qty"];
    if (typeof(name) != T_STRING || name == "" || typeof(qty) != T_INT) {
	return fail(400, "Bad Request", "name and qty required");
    }
    id = INVENTORYD->create_item(name, qty, principal);
    return respond(201, "Created", ([ "id" : id ]));
}

private mixed *do_update_item(int id, string body, string principal)
{
    mapping parsed;
    mixed name, qty;

    parsed = parse_body(body);
    if (!parsed) {
	return fail(400, "Bad Request", "malformed body");
    }
    name = parsed["name"];
    qty = parsed["qty"];
    if (typeof(name) != T_STRING || name == "" || typeof(qty) != T_INT) {
	return fail(400, "Bad Request", "name and qty required");
    }
    switch (INVENTORYD->update_item(id, name, qty, principal)) {
    case 1:
	return respond(200, "OK", ([ "id" : id ]));
    case 0:
	return fail(404, "Not Found", "no such item");
    default:
	return fail(403, "Forbidden", "not the item's creator");
    }
}

private mixed *do_wipe(string principal)
{
    if (INVENTORYD->wipe(principal) != 1) {
	return fail(403, "Forbidden",
		    "requires the example:inventory-admin capability");
    }
    return respond(200, "OK", ([ "wiped" : 1 ]));
}

private mixed *do_report(string principal)
{
    mixed summary;

    summary = INVENTORYD->report(principal);
    if (typeof(summary) != T_MAPPING) {
	return fail(403, "Forbidden",
		    "requires the example:delegation-demo capability");
    }
    return respond(200, "OK", summary);
}

private mixed *do_audit()
{
    return respond(200, "OK", ([ "audit" : INVENTORYD->query_audit() ]));
}


/*
 * the WWW servers' handler contract
 */
mixed *handle(string method, string path, string body, string authorization)
{
    string principal, uuid, streamToken;
    int id, heartbeat;

    /* the anonymous surfaces */
    if (method == "GET" && path == "/inventory/health") {
	return ({ 200, "OK", "text/plain; charset=utf-8", "ok\n" });
    }
    if (method == "GET" && path == "/auth/challenge") {
	return do_challenge("webauthn");
    }
    if (method == "GET" && path == "/auth/recover-challenge") {
	return do_challenge("recover");
    }
    if (method == "POST" && path == "/auth/recover") {
	return do_recover(body);
    }
    if (method == "POST" && path == "/auth/register") {
	return do_register(body);
    }
    if (method == "POST" && path == "/auth/login") {
	return do_login(body);
    }
    if (method == "POST" && path == "/auth/logout") {
	return do_logout(authorization);
    }
    if (method == "POST" && path == "/auth/agent-login") {
	return do_agent_login(body);
    }
    if (method == "GET" &&
	(path == "/inventory/events" ||
	 sscanf(path, "/inventory/events?heartbeat=%d", heartbeat) != 0)) {
	return do_audit_stream(heartbeat);
    }
    if (method == "GET" &&
	(path == "/auth/agents/stream" ||
	 sscanf(path, "/auth/agents/stream?token=%s", streamToken) != 0)) {
	return do_agent_stream(streamToken);
    }
    if (method == "GET" && path == "/inventory/items") {
	return do_list_items();
    }
    if (method == "GET" && path == "/inventory/audit") {
	return do_audit();
    }

    /* everything below requires an authenticated identity */
    principal = bearer_principal(authorization);
    if (!principal) {
	return fail(401, "Unauthorized", "authentication required");
    }

    if (method == "POST" && path == "/auth/recovery-codes") {
	return do_recovery_codes(body, authorization);
    }
    if (path == "/auth/agents") {
	if (method == "GET") {
	    return do_list_agents(authorization);
	}
	if (method == "POST") {
	    return do_mint_agent(authorization);
	}
    }
    if (method == "POST") {
	if (sscanf(path, "/auth/agents/%s/suspend", uuid) != 0) {
	    return do_agent_suspend(uuid, authorization);
	}
	if (sscanf(path, "/auth/agents/%s/resume", uuid) != 0) {
	    return do_agent_resume(uuid, authorization);
	}
	if (sscanf(path, "/auth/agents/%s/delegate", uuid) != 0) {
	    return do_agent_delegation(uuid, body, authorization, TRUE);
	}
	if (sscanf(path, "/auth/agents/%s/undelegate", uuid) != 0) {
	    return do_agent_delegation(uuid, body, authorization, FALSE);
	}
    }

    if (method == "GET" && path == "/inventory/report") {
	return do_report(principal);
    }
    if (method == "POST" && path == "/inventory/items") {
	return do_create_item(body, principal);
    }
    if (method == "PUT" &&
	sscanf(path, "/inventory/items/%d", id) != 0) {
	return do_update_item(id, body, principal);
    }
    if (method == "DELETE" && path == "/inventory/items") {
	return do_wipe(principal);
    }

    return fail(404, "Not Found", "no such route");
}
