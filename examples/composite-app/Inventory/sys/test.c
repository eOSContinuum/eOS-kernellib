/*
 * Boot-time test driver for the composite example.
 *
 * Every phase below reaches the application the way a real client
 * does: over TCP to the platform's own binary port, through the WWW
 * mount-point server, the route registry, and the Inventory handler,
 * down to the platform daemons. The driver clones obj/client (an
 * Http1Client subclass, the library's first in-tree consumer) once per
 * request; nothing here calls the handler directly except the
 * documented test seam (arm_challenge, which plants the fixed
 * challenge the foreign-generated WebAuthn vectors embed -- the
 * vectors are scripts/gen-webauthn-vectors.py output, shared with
 * examples/webauthn-app).
 *
 * Boot 1 (cold, selfexit), with the crypto module (32 sentinels
 * total):
 *
 *   HEALTH OK                    transport -> router -> handler chain
 *   ROUTE-MISS OK                unregistered prefix is the server's 404
 *   CHALLENGE OK                 ceremony challenge issued via authd
 *   REGISTER OK                  TOFU registration over the wire; the
 *                                platform mints identity + session
 *   CHALLENGE-REPLAY-REFUSED OK  the handler's single-use challenge
 *                                store refuses a consumed challenge
 *   BAD-ORIGIN-REFUSED OK        forged-origin clientDataJSON refused
 *   AUTH-REQUIRED OK             mutating route without a bearer: 401
 *   ITEM-CREATE OK               authenticated create reaches the
 *                                persistent daemon
 *   AUDIT-TRAIL OK               the Merry observer wrote the audit
 *                                entry synchronously inside the write
 *   LOGIN OK                     assertion ceremony mints a session
 *   ASSERT-REPLAY-REFUSED OK     replayed signature counter refused
 *   SECOND-IDENTITY OK           second registration (Ed25519 vector)
 *                                mints a distinct principal
 *   OWNER-ONLY OK                application-tier authorization: the
 *                                second identity may not edit the
 *                                first's item
 *   ADMIN-REFUSED OK             platform-capability gate: wipe
 *                                refused without the operator grant
 *   LOGOUT OK                    revoked session no longer validates
 *   AGENT-MINT OK                a live identity session mints an
 *                                agent; the response carries the
 *                                token's only plaintext
 *   AGENT-LIST OK                the controller's own-agents view
 *                                shows the new agent, active
 *   AGENT-LOGIN OK               the agent-token ceremony mints an
 *                                agent session
 *   AGENT-NOT-OWN-REFUSED OK     another identity cannot suspend it
 *   AGENT-DELEGATE-REFUSED OK    delegation refused when the atomic
 *                                substrate checks fail (capability
 *                                not held, not flagged delegable)
 *   AGENT-SUSPEND OK             suspension revokes the agent's live
 *                                session, now
 *   AGENT-SUSPENDED-REFUSED OK   a suspended agent's token ceremony
 *                                refuses
 *   AGENT-RESUME OK              resume restores the ability to
 *                                authenticate: the ceremony works
 *                                again
 *   SSE-STREAM-OPEN OK           the audit event stream opens: 200,
 *                                chunked, held open past the response
 *   SSE-AUDIT-PUSH OK            a mutation's audit event reaches the
 *                                open stream, observer-driven
 *   SSE-AGENTS-SNAPSHOT OK       a fresh agent stream receives its
 *                                own-agents snapshot within one poll
 *   SSE-AGENTS-PUSH OK           an agent-state change (suspend)
 *                                reaches the open stream as a new
 *                                snapshot
 *   SSE-AUTH-REFUSED OK          the agent stream refuses a bogus
 *                                session token
 *   PERSIST SETUP OK             restore probe armed, snapshot dumped
 *
 * Boot 2 (restore): the call_out armed before the dump fires and
 * drives three more wire-level checks against the restored image:
 *
 *   PERSIST-ITEMS OK             items survived the restore
 *   PERSIST-SESSION OK           a pre-restore session token still
 *                                authenticates (hash-at-rest store)
 *   PERSIST-OBSERVER OK          the observer binding survived: a
 *                                post-restore write extends the audit
 *
 * Without the crypto module the ceremony surface stands down
 * (challenge returns 503); the driver then runs the transport-only
 * subset: HEALTH, ROUTE-MISS, AUTH-REQUIRED, PERSIST SETUP, and
 * PERSIST-HTTP after restore -- 5 sentinels, the profile default. Run with
 * LPC_EXT_CRYPTO=<module> EXPECTED_OK=32 for the full set.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";
private inherit json "/lib/util/json";
private inherit base64 "/lib/util/base64";
private inherit hex "/lib/util/hex";

# include "vectors.h"

# define HANDLER		"/usr/Inventory/sys/handler"
# define CLIENT			"/usr/Inventory/obj/client"
# define STREAM_CLIENT		"/usr/Inventory/obj/stream_client"
# define PERSIST_HELPER		"/usr/System/sys/persist_helper"
# define RESULT_FILE		"/usr/Inventory/data/test-result.log"

/* phase numbers: boot 1 */
# define P_HEALTH		1
# define P_ROUTE_MISS		2
# define P_CHALLENGE		3
# define P_REGISTER		4
# define P_CH_REPLAY		5
# define P_BAD_ORIGIN		6
# define P_AUTH_REQUIRED	7
# define P_ITEM_CREATE		8
# define P_AUDIT		9
# define P_LOGIN		10
# define P_ASSERT_REPLAY	11
# define P_SECOND_IDENTITY	12
# define P_OWNER_ONLY		13
# define P_ADMIN_REFUSED	14
# define P_LOGOUT		15
# define P_LOGOUT_PROBE		16
# define P_AGENT_MINT		17
# define P_AGENT_LIST		18
# define P_AGENT_LOGIN		19
# define P_AGENT_NOT_OWN	20
# define P_AGENT_DELEGATE_REF	21
# define P_AGENT_SUSPEND	22
# define P_AGENT_SUSPENDED	23
# define P_AGENT_RESUME		24
# define P_AGENT_RELOGIN	25
# define P_SSE_AUDIT_OPEN	26
# define P_SSE_AUDIT_PUSH	27
# define P_SSE_AGENTS_OPEN	28
# define P_SSE_AGENTS_PUSH	29
# define P_SSE_BAD_TOKEN	30
/* phase numbers: boot 2 (restore) */
# define P_PERSIST_ITEMS	31
# define P_PERSIST_SESSION	32
# define P_PERSIST_OBSERVER	33
# define P_PERSIST_HTTP		34	/* no-crypto restore probe */

# define STREAM_DEADLINE	8	/* seconds an awaited event may take */

private int phase;		/* current phase */
private int cryptoMode;		/* ceremony surface available */

/* survives the snapshot; the restore phases read these back */
private string token1;		/* first identity's session */
private string token2;		/* login-minted session (revoked) */
private string tokenEd;		/* second identity's session */
private string principal1;	/* first identity */
private string principalEd;	/* second identity */
private int itemId;		/* the created item */
private int auditCount;		/* audit entries at dump time */
private string agentUuid;	/* the minted agent */
private string agentToken;	/* its mint-time token (plaintext) */
private object auditStream;	/* held-open audit event stream */
private object agentStream;	/* held-open agent-state event stream */

private void log_line(string msg);
static void start_phase();
private void arm_deadline();
private void drop_streams();


static void create()
{
    /* Defer past the System initd's boot iteration AND the queued
     * call_out-0 registrations (router routes, observer binding). */
    call_out("setup_and_run", 1);
}

static void setup_and_run()
{
    catch(make_dir("/usr/Inventory/data"));
    catch(remove_file(RESULT_FILE));
    phase = P_HEALTH;
    start_phase();
}


private void log_line(string msg)
{
    mixed *info;
    int size;

    catch {
	info = file_info(RESULT_FILE);
	size = info ? info[0] : 0;
	write_file(RESULT_FILE, msg + "\n", size);
    }
}

private void pass(string sentinel)
{
    log_line("Composite:test: " + sentinel + " OK");
}

private void stop(string why)
{
    log_line("Composite:test: FAIL: " + why);
    PERSIST_HELPER->trigger_dump_and_exit();
}

/*
 * issue one HTTP exchange through a fresh client clone
 */
private void http(string method, string path, string auth, string body)
{
    object client;

    client = clone_object(CLIENT);
    client->fetch(this_object(), method, path, auth, body);
}

private string bearer(string token)
{
    return "Bearer " + token;
}

private mapping jbody(string body)
{
    mixed parsed;

    if (!body || body == "" ||
	catch(parsed = json::decode(body)) != nil ||
	typeof(parsed) != T_MAPPING) {
	return ([ ]);
    }
    return parsed;
}

/*
 * a hex-encoded vector as a base64url wire field
 */
private string wire(string hexData)
{
    return base64::urlEncode(hex::decodeString(hexData));
}

private string register_body(string challenge, string cdjHex, string aoHex)
{
    return json::encode(([ "challenge" : challenge,
			   "clientDataJSON" : wire(cdjHex),
			   "attestationObject" : wire(aoHex) ]));
}

private string login_body(string challenge, string credIdHex, string cdjHex,
			  string adHex, string sigHex)
{
    return json::encode(([ "challenge" : challenge,
			   "credentialId" : wire(credIdHex),
			   "clientDataJSON" : wire(cdjHex),
			   "authenticatorData" : wire(adHex),
			   "signature" : wire(sigHex) ]));
}

private string item_body(string name, int qty)
{
    return json::encode(([ "name" : name, "qty" : qty ]));
}


/*
 * issue the current phase's request (any local setup first)
 */
static void start_phase()
{
    switch (phase) {
    case P_HEALTH:
	http("GET", "/inventory/health", nil, nil);
	break;

    case P_ROUTE_MISS:
	http("GET", "/nope", nil, nil);
	break;

    case P_CHALLENGE:
	http("GET", "/auth/challenge", nil, nil);
	break;

    case P_REGISTER:
	HANDLER->arm_challenge(WA_CH_REG);
	http("POST", "/auth/register", nil,
	     register_body(WA_CH_REG, WA_REG_CDJ_HEX, WA_REG_AO_HEX));
	break;

    case P_CH_REPLAY:
	/* same challenge again, NOT re-armed: the store consumed it */
	http("POST", "/auth/register", nil,
	     register_body(WA_CH_REG, WA_REG_CDJ_HEX, WA_REG_AO_HEX));
	break;

    case P_BAD_ORIGIN:
	HANDLER->arm_challenge(WA_CH_REG);
	http("POST", "/auth/register", nil,
	     register_body(WA_CH_REG, WA_REG_CDJ_BAD_ORIGIN_HEX,
			   WA_REG_AO_HEX));
	break;

    case P_AUTH_REQUIRED:
	http("POST", "/inventory/items", nil, item_body("widget", 3));
	break;

    case P_ITEM_CREATE:
	http("POST", "/inventory/items", bearer(token1),
	     item_body("widget", 3));
	break;

    case P_AUDIT:
	http("GET", "/inventory/audit", nil, nil);
	break;

    case P_LOGIN:
	HANDLER->arm_challenge(WA_CH_A1);
	http("POST", "/auth/login", nil,
	     login_body(WA_CH_A1, WA_ES_CRED_ID_HEX, WA_A1_CDJ_HEX,
			WA_A1_AD_HEX, WA_A1_SIG_HEX));
	break;

    case P_ASSERT_REPLAY:
	/* fresh armed challenge, replayed assertion: the signature
	 * counter (6) is no longer greater than the stored counter */
	HANDLER->arm_challenge(WA_CH_A1);
	http("POST", "/auth/login", nil,
	     login_body(WA_CH_A1, WA_ES_CRED_ID_HEX, WA_A1_CDJ_HEX,
			WA_A1_AD_HEX, WA_A1_SIG_HEX));
	break;

    case P_SECOND_IDENTITY:
	HANDLER->arm_challenge(WA_CH_REG2);
	http("POST", "/auth/register", nil,
	     register_body(WA_CH_REG2, WA_ED_REG_CDJ_HEX,
			   WA_ED_REG_AO_HEX));
	break;

    case P_OWNER_ONLY:
	http("PUT", "/inventory/items/" + itemId, bearer(tokenEd),
	     item_body("widget", 9));
	break;

    case P_ADMIN_REFUSED:
	http("DELETE", "/inventory/items", bearer(token1), nil);
	break;

    case P_LOGOUT:
	http("POST", "/auth/logout", bearer(token2), nil);
	break;

    case P_LOGOUT_PROBE:
	http("PUT", "/inventory/items/" + itemId, bearer(token2),
	     item_body("widget", 4));
	break;

    case P_AGENT_MINT:
	http("POST", "/auth/agents", bearer(token1), nil);
	break;

    case P_AGENT_LIST:
	http("GET", "/auth/agents", bearer(token1), nil);
	break;

    case P_AGENT_LOGIN:
    case P_AGENT_SUSPENDED:
    case P_AGENT_RELOGIN:
	http("POST", "/auth/agent-login", nil,
	     json::encode(([ "token" : agentToken ])));
	break;

    case P_AGENT_NOT_OWN:
	/* the second identity is not the agent's controller */
	http("POST", "/auth/agents/" + agentUuid + "/suspend",
	     bearer(tokenEd), nil);
	break;

    case P_AGENT_DELEGATE_REF:
	/* the controller does not hold the capability (ADMIN-REFUSED
	 * proved that), so the atomic delegation checks refuse */
	http("POST", "/auth/agents/" + agentUuid + "/delegate",
	     bearer(token1),
	     json::encode(([ "capability" : "example:inventory-admin" ])));
	break;

    case P_AGENT_SUSPEND:
	http("POST", "/auth/agents/" + agentUuid + "/suspend",
	     bearer(token1), nil);
	break;

    case P_AGENT_RESUME:
	http("POST", "/auth/agents/" + agentUuid + "/resume",
	     bearer(token1), nil);
	break;

    case P_SSE_AUDIT_OPEN:
	auditStream = clone_object(STREAM_CLIENT);
	auditStream->open(this_object(), "/inventory/events");
	arm_deadline();
	break;

    case P_SSE_AUDIT_PUSH:
	/* an audited mutation while the stream is open; the sentinel
	 * lands when the observer-driven event arrives on the wire */
	http("POST", "/inventory/items", bearer(token1),
	     item_body("streamed", 1));
	arm_deadline();
	break;

    case P_SSE_AGENTS_OPEN:
	agentStream = clone_object(STREAM_CLIENT);
	agentStream->open(this_object(),
			  "/auth/agents/stream?token=" + token1);
	arm_deadline();
	break;

    case P_SSE_AGENTS_PUSH:
	/* a state change while the agent stream is open */
	http("POST", "/auth/agents/" + agentUuid + "/suspend",
	     bearer(token1), nil);
	arm_deadline();
	break;

    case P_SSE_BAD_TOKEN:
	http("GET", "/auth/agents/stream?token=bogus", nil, nil);
	break;

    case P_PERSIST_ITEMS:
    case P_PERSIST_HTTP:
	http("GET", "/inventory/items", nil, nil);
	break;

    case P_PERSIST_SESSION:
	http("POST", "/inventory/items", bearer(token1),
	     item_body("gadget", 1));
	break;

    case P_PERSIST_OBSERVER:
	http("GET", "/inventory/audit", nil, nil);
	break;
    }
}

private void advance()
{
    phase++;
    start_phase();
}

/*
 * awaited-event deadline: if the phase has not moved on when this
 * fires, the expected stream event never arrived
 */
private void arm_deadline()
{
    call_out("stream_deadline", STREAM_DEADLINE, phase);
}

static void stream_deadline(int armedPhase)
{
    if (phase == armedPhase) {
	stop("stream event deadline in phase " + phase);
    }
}

private void drop_streams()
{
    if (auditStream) {
	destruct_object(auditStream);
    }
    if (agentStream) {
	destruct_object(agentStream);
    }
}

/*
 * arm the restore probe and dump: the call_out survives the snapshot
 * and fires once the restored system is back up
 */
private void persist_setup()
{
    pass("PERSIST SETUP");
    call_out("restore_verify", 3);
    PERSIST_HELPER->trigger_dump_and_exit();
}

static void restore_verify()
{
    phase = cryptoMode ? P_PERSIST_ITEMS : P_PERSIST_HTTP;
    start_phase();
}


/*
 * one exchange completed; validate against the current phase
 */
void http_done(int code, string body)
{
    mapping parsed;
    mixed value;

    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }

    switch (phase) {
    case P_HEALTH:
	if (code != 200 || body != "ok\n") {
	    stop("HEALTH: " + code);
	    return;
	}
	pass("HEALTH");
	advance();
	break;

    case P_ROUTE_MISS:
	if (code != 404) {
	    stop("ROUTE-MISS: " + code);
	    return;
	}
	pass("ROUTE-MISS");
	advance();
	break;

    case P_CHALLENGE:
	if (code == 503) {
	    /* ceremony surface stood down: no crypto module. Run the
	     * transport-only subset. */
	    cryptoMode = FALSE;
	    log_line("Composite:test: ceremony phases skipped " +
		     "(crypto module absent)");
	    phase = P_AUTH_REQUIRED;
	    start_phase();
	    return;
	}
	parsed = jbody(body);
	value = parsed["challenge"];
	if (code != 200 || typeof(value) != T_STRING || value == "") {
	    stop("CHALLENGE: " + code);
	    return;
	}
	cryptoMode = TRUE;
	pass("CHALLENGE");
	advance();
	break;

    case P_REGISTER:
	parsed = jbody(body);
	if (code != 201 ||
	    typeof(parsed["principal"]) != T_STRING ||
	    sscanf(parsed["principal"], "identity:%*s") == 0 ||
	    typeof(parsed["token"]) != T_STRING) {
	    stop("REGISTER: " + code + " " + body);
	    return;
	}
	principal1 = parsed["principal"];
	token1 = parsed["token"];
	pass("REGISTER");
	advance();
	break;

    case P_CH_REPLAY:
	if (code != 400) {
	    stop("CHALLENGE-REPLAY: expected 400, got " + code);
	    return;
	}
	pass("CHALLENGE-REPLAY-REFUSED");
	advance();
	break;

    case P_BAD_ORIGIN:
	if (code != 400) {
	    stop("BAD-ORIGIN: expected 400, got " + code);
	    return;
	}
	pass("BAD-ORIGIN-REFUSED");
	advance();
	break;

    case P_AUTH_REQUIRED:
	if (code != 401) {
	    stop("AUTH-REQUIRED: expected 401, got " + code);
	    return;
	}
	pass("AUTH-REQUIRED");
	if (cryptoMode) {
	    advance();
	} else {
	    persist_setup();
	}
	break;

    case P_ITEM_CREATE:
	parsed = jbody(body);
	if (code != 201 || typeof(parsed["id"]) != T_INT ||
	    parsed["id"] < 1) {
	    stop("ITEM-CREATE: " + code + " " + body);
	    return;
	}
	itemId = parsed["id"];
	pass("ITEM-CREATE");
	advance();
	break;

    case P_AUDIT:
	parsed = jbody(body);
	value = parsed["audit"];
	if (code != 200 || typeof(value) != T_ARRAY ||
	    sizeof(value) < 1 ||
	    typeof(value[sizeof(value) - 1]) != T_STRING ||
	    sscanf(value[sizeof(value) - 1], "create %*s") == 0) {
	    stop("AUDIT-TRAIL: " + code + " " + body);
	    return;
	}
	auditCount = sizeof(value);
	pass("AUDIT-TRAIL");
	advance();
	break;

    case P_LOGIN:
	parsed = jbody(body);
	if (code != 200 || typeof(parsed["token"]) != T_STRING ||
	    parsed["principal"] != principal1) {
	    stop("LOGIN: " + code + " " + body);
	    return;
	}
	token2 = parsed["token"];
	pass("LOGIN");
	advance();
	break;

    case P_ASSERT_REPLAY:
	if (code != 401) {
	    stop("ASSERT-REPLAY: expected 401, got " + code);
	    return;
	}
	pass("ASSERT-REPLAY-REFUSED");
	advance();
	break;

    case P_SECOND_IDENTITY:
	parsed = jbody(body);
	if (code != 201 || typeof(parsed["principal"]) != T_STRING ||
	    parsed["principal"] == principal1 ||
	    typeof(parsed["token"]) != T_STRING) {
	    stop("SECOND-IDENTITY: " + code + " " + body);
	    return;
	}
	principalEd = parsed["principal"];
	tokenEd = parsed["token"];
	pass("SECOND-IDENTITY");
	advance();
	break;

    case P_OWNER_ONLY:
	if (code != 403) {
	    stop("OWNER-ONLY: expected 403, got " + code);
	    return;
	}
	pass("OWNER-ONLY");
	advance();
	break;

    case P_ADMIN_REFUSED:
	if (code != 403) {
	    stop("ADMIN-REFUSED: expected 403, got " + code);
	    return;
	}
	pass("ADMIN-REFUSED");
	advance();
	break;

    case P_LOGOUT:
	parsed = jbody(body);
	if (code != 200 || parsed["revoked"] != 1) {
	    stop("LOGOUT: " + code + " " + body);
	    return;
	}
	advance();	/* the sentinel lands after the probe */
	break;

    case P_LOGOUT_PROBE:
	if (code != 401) {
	    stop("LOGOUT: revoked token still accepted, " + code);
	    return;
	}
	pass("LOGOUT");
	advance();
	break;

    case P_AGENT_MINT:
	parsed = jbody(body);
	if (code != 201 || typeof(parsed["uuid"]) != T_STRING ||
	    typeof(parsed["token"]) != T_STRING) {
	    stop("AGENT-MINT: " + code + " " + body);
	    return;
	}
	agentUuid = parsed["uuid"];
	agentToken = parsed["token"];
	pass("AGENT-MINT");
	advance();
	break;

    case P_AGENT_LIST:
	parsed = jbody(body);
	value = parsed["agents"];
	if (code != 200 || typeof(value) != T_ARRAY ||
	    sizeof(value) != 1 || value[0]["uuid"] != agentUuid ||
	    value[0]["suspended"] != 0) {
	    stop("AGENT-LIST: " + code + " " + body);
	    return;
	}
	pass("AGENT-LIST");
	advance();
	break;

    case P_AGENT_LOGIN:
	parsed = jbody(body);
	if (code != 200 ||
	    parsed["principal"] != "identity:" + agentUuid ||
	    typeof(parsed["token"]) != T_STRING) {
	    stop("AGENT-LOGIN: " + code + " " + body);
	    return;
	}
	pass("AGENT-LOGIN");
	advance();
	break;

    case P_AGENT_NOT_OWN:
	if (code != 403) {
	    stop("AGENT-NOT-OWN: expected 403, got " + code);
	    return;
	}
	pass("AGENT-NOT-OWN-REFUSED");
	advance();
	break;

    case P_AGENT_DELEGATE_REF:
	if (code != 403) {
	    stop("AGENT-DELEGATE: expected 403, got " + code);
	    return;
	}
	pass("AGENT-DELEGATE-REFUSED");
	advance();
	break;

    case P_AGENT_SUSPEND:
	parsed = jbody(body);
	if (code != 200 || parsed["revoked"] != 1) {
	    stop("AGENT-SUSPEND: " + code + " " + body);
	    return;
	}
	pass("AGENT-SUSPEND");
	advance();
	break;

    case P_AGENT_SUSPENDED:
	if (code != 401) {
	    stop("AGENT-SUSPENDED: expected 401, got " + code);
	    return;
	}
	pass("AGENT-SUSPENDED-REFUSED");
	advance();
	break;

    case P_AGENT_RESUME:
	if (code != 200) {
	    stop("AGENT-RESUME: " + code + " " + body);
	    return;
	}
	advance();	/* the sentinel lands after the ceremony probe */
	break;

    case P_AGENT_RELOGIN:
	parsed = jbody(body);
	if (code != 200 ||
	    parsed["principal"] != "identity:" + agentUuid) {
	    stop("AGENT-RESUME: relogin " + code + " " + body);
	    return;
	}
	pass("AGENT-RESUME");
	advance();
	break;

    case P_SSE_AUDIT_PUSH:
	/* the mutation's own response; the phase completes when the
	 * event arrives in stream_event below */
	if (code != 201) {
	    stop("SSE-AUDIT-PUSH: create " + code + " " + body);
	}
	break;

    case P_SSE_AGENTS_PUSH:
	/* the suspend's own response; the phase completes on the
	 * pushed snapshot */
	if (code != 200) {
	    stop("SSE-AGENTS-PUSH: suspend " + code + " " + body);
	}
	break;

    case P_SSE_BAD_TOKEN:
	if (code != 401) {
	    stop("SSE-AUTH: expected 401, got " + code);
	    return;
	}
	pass("SSE-AUTH-REFUSED");
	persist_setup();
	break;

    case P_PERSIST_ITEMS:
	parsed = jbody(body);
	value = parsed["items"];
	if (code != 200 || typeof(value) != T_ARRAY ||
	    sizeof(value) < 1 || value[0]["id"] != itemId ||
	    value[0]["name"] != "widget") {
	    stop("PERSIST-ITEMS: " + code + " " + body);
	    return;
	}
	pass("PERSIST-ITEMS");
	advance();
	break;

    case P_PERSIST_SESSION:
	parsed = jbody(body);
	if (code != 201 || typeof(parsed["id"]) != T_INT) {
	    stop("PERSIST-SESSION: " + code + " " + body);
	    return;
	}
	pass("PERSIST-SESSION");
	advance();
	break;

    case P_PERSIST_OBSERVER:
	parsed = jbody(body);
	value = parsed["audit"];
	if (code != 200 || typeof(value) != T_ARRAY ||
	    sizeof(value) <= auditCount) {
	    stop("PERSIST-OBSERVER: " + code + " " + body);
	    return;
	}
	pass("PERSIST-OBSERVER");
	/* boot 2 is a timed window; nothing left to do */
	break;

    case P_PERSIST_HTTP:
	if (code != 200) {
	    stop("PERSIST-HTTP: " + code);
	    return;
	}
	pass("PERSIST-HTTP");
	break;
    }
}

/*
 * an exchange failed at the connection level
 */
void http_fail(int errorcode)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    stop("connect failed in phase " + phase + " (error " + errorcode + ")");
}

/*
 * stream-client callbacks: response head, per-frame events, failure
 */
void stream_open(int code)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    switch (phase) {
    case P_SSE_AUDIT_OPEN:
	if (code != 200) {
	    stop("SSE-STREAM-OPEN: " + code);
	    return;
	}
	pass("SSE-STREAM-OPEN");
	advance();
	break;

    case P_SSE_AGENTS_OPEN:
	if (code != 200) {
	    stop("SSE-AGENTS: open " + code);
	}
	/* the sentinel lands on the snapshot event */
	break;
    }
}

void stream_event(string event, string data)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    switch (phase) {
    case P_SSE_AUDIT_PUSH:
	if (event == "audit" && sscanf(data, "%*screate%*s") != 0) {
	    pass("SSE-AUDIT-PUSH");
	    advance();
	}
	break;

    case P_SSE_AGENTS_OPEN:
	if (event == "agents" && sscanf(data, "%*s" + agentUuid) != 0) {
	    pass("SSE-AGENTS-SNAPSHOT");
	    advance();
	}
	break;

    case P_SSE_AGENTS_PUSH:
	if (event == "agents" &&
	    sscanf(data, "%*s\"suspended\":1%*s") != 0) {
	    pass("SSE-AGENTS-PUSH");
	    drop_streams();
	    advance();
	}
	break;
    }
}

void stream_fail(int errorcode)
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    stop("stream connect failed in phase " + phase +
	 " (error " + errorcode + ")");
}
