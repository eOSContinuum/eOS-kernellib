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

private mapping pendingChallenges;	/* challenge : issue time */

static void create()
{
    ::create();
    pendingChallenges = ([ ]);
}

/*
 * single-use challenge store: issue records, consume removes
 */
private string issue_challenge()
{
    string challenge;

    challenge = AUTHD->issue_challenge();
    pendingChallenges[challenge] = time();
    return challenge;
}

private int consume_challenge(string challenge)
{
    if (challenge && pendingChallenges[challenge]) {
	pendingChallenges[challenge] = nil;
	return TRUE;
    }
    return FALSE;
}

/*
 * test seam: plant a known challenge (foreign ceremony vectors embed a
 * fixed one). Same-domain callers only; not a production surface.
 */
void arm_challenge(string challenge)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    pendingChallenges[challenge] = time();
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


private mixed *do_challenge()
{
    string challenge;

    if (catch(challenge = issue_challenge()) != nil) {
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
    if (!consume_challenge(challenge)) {
	return fail(400, "Bad Request", "unknown or consumed challenge");
    }
    if (catch(result = AUTHD->register_identity(challenge, clientDataJSON,
						attestationObject)) != nil) {
	return fail(400, "Bad Request", "registration refused");
    }
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
    if (!consume_challenge(challenge)) {
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

private mixed *do_audit()
{
    return respond(200, "OK", ([ "audit" : INVENTORYD->query_audit() ]));
}


/*
 * the WWW servers' handler contract
 */
mixed *handle(string method, string path, string body, string authorization)
{
    string principal, uuid;
    int id;

    /* the anonymous surfaces */
    if (method == "GET" && path == "/inventory/health") {
	return ({ 200, "OK", "text/plain; charset=utf-8", "ok\n" });
    }
    if (method == "GET" && path == "/auth/challenge") {
	return do_challenge();
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
