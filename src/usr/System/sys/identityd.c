/*
 * Identity registry: the System-tier owning daemon for the platform's
 * shared identity substrate. Mints identity records (clones of
 * /usr/System/obj/identity), owns the global credential-id index, and
 * enforces the record invariants:
 *
 *   - a credential id binds to at most one identity, ever checked at
 *     bind time (the substrate half of the no-re-bind rule; ceremony
 *     policy layers on top);
 *   - a record never reaches zero credentials -- single-step removals
 *     are guarded up front, and compound operations (rotation,
 *     redemption) run as atomic functions validated at the end, so a
 *     violating compound rolls back whole;
 *   - recovery codes are stored hashed, single-use, and are generated
 *     here (never imported), so plaintext codes exist only in the
 *     minting response;
 *   - agent records carry an immutable controller edge that must name
 *     a human record, so control never chains (an agent cannot control
 *     an agent) and every agent action is attributable to a human;
 *   - agent credential rows bind only to agent records and human rows
 *     only to human records, checked at the single bind choke point;
 *   - agent tokens follow the recovery-code secret discipline (stored
 *     hashed, generated here, plaintext only in the minting response)
 *     plus a required expiry;
 *   - a capability grant on an identity is source-tracked here, so a
 *     revocation drops the capabilityd entry only when the last reason
 *     for the grant is gone.
 *
 * Randomness and hashing come from the host crypto module
 * (secure_random, SHA256); without the module the daemon boots, reports
 * the stand-down in its status verb, and refuses minting cleanly.
 *
 * The mutating and reading surface is System-tier (SYSTEM/KERNEL
 * callers); the operator face is the "identity" console verb,
 * dispatched by the kernel admin-console registry.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>
# include <kfun.h>
# include <type.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */
private inherit base64 "/lib/util/base64";
private inherit hex "/lib/util/hex";

# define MAX_CODES	16	/* recovery codes per mint/rotation */

private mapping identities;		/* uuid : identity object */
private mapping credentialIndex;	/* credential id : uuid */
private mapping controllerIndex;	/* controller uuid : agent uuid set */
private mapping grantSources;		/* uuid : capability : source set */

static void create()
{
    ::create();
    identities = ([ ]);
    credentialIndex = ([ ]);
    controllerIndex = ([ ]);
    grantSources = ([ ]);
    compile_object(OBJ_IDENTITY);
    sysLog("identity: registry up; crypto module " +
# ifdef KF_SECURE_RANDOM
	   "present"
# else
	   "absent (minting unavailable)"
# endif
	   );
}


/*
 * gates. The caller's program is captured at each public entry and
 * passed in: previous_program() inside a private helper would name this
 * file, not the true external caller (the admin-console registry's
 * _check_caller documents the same trap).
 */
private void check_system(string caller)
{
    if (sscanf(caller, "/usr/System/%*s") == 0 &&
	sscanf(caller, "/kernel/%*s") == 0) {
	error("Access denied");
    }
}

private object need_identity(string uuid)
{
    object identity;

    identity = identities[uuid];
    if (!identity) {
	error("identity: no such identity");
    }
    return identity;
}


/*
 * crypto-backed generation; each errors cleanly without the module
 */
private string generate_uuid()
{
# ifdef KF_SECURE_RANDOM
    string bytes, hexStr;

    bytes = secure_random(16);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;	/* version 4 */
    bytes[8] = (bytes[8] & 0x3f) | 0x80;	/* RFC 4122 variant */
    hexStr = hex::format(bytes);
    return hexStr[.. 7] + "-" + hexStr[8 .. 11] + "-" + hexStr[12 .. 15] +
	   "-" + hexStr[16 .. 19] + "-" + hexStr[20 ..];
# else
    error("identity: crypto module not loaded");
# endif
}

private string generate_code()
{
# ifdef KF_SECURE_RANDOM
    return base64::urlEncode(secure_random(15));
# else
    error("identity: crypto module not loaded");
# endif
}

private string code_hash(string code)
{
# ifdef KF_SECURE_RANDOM
    return hex::format(hash_string("SHA256", code));
# else
    error("identity: crypto module not loaded");
# endif
}

/*
 * a recovery row's credential id is derived from its hash
 */
private string code_id(string hash)
{
    return "rc:" + hash[.. 11];
}

private mapping code_row(string hash)
{
    return ([ CRED_TYPE : CRED_TYPE_RECOVERY,
	      CRED_HASH : hash,
	      CRED_CREATED : time() ]);
}

/*
 * agent tokens follow the recovery-code secret discipline (generated
 * here, stored hashed, plaintext only in the minting response) plus a
 * required expiry; the credential id is derived from the hash
 */
private string generate_token()
{
# ifdef KF_SECURE_RANDOM
    return base64::urlEncode(secure_random(32));
# else
    error("identity: crypto module not loaded");
# endif
}

private string token_id(string hash)
{
    return "at:" + hash[.. 11];
}

private int token_expiry(int ttl)
{
    if (ttl <= 0) {
	ttl = AGENT_TOKEN_TTL;
    } else if (ttl > AGENT_TOKEN_MAX_TTL) {
	ttl = AGENT_TOKEN_MAX_TTL;
    }
    return time() + ttl;
}

private mapping token_row(string hash, int expires)
{
    return ([ CRED_TYPE : CRED_TYPE_AGENT_TOKEN,
	      CRED_HASH : hash,
	      CRED_EXPIRES : expires,
	      CRED_CREATED : time() ]);
}

/*
 * validate a caller-supplied credential row
 */
private void validate_row(string id, mapping row)
{
    if (!id || strlen(id) == 0 || !row) {
	error("identity: bad credential");
    }
    switch (row[CRED_TYPE]) {
    case CRED_TYPE_PASSKEY:
	if (typeof(row[CRED_KEY]) != T_STRING ||
	    typeof(row[CRED_SCHEME]) != T_STRING) {
	    error("identity: passkey row lacks key material");
	}
	break;

    case CRED_TYPE_RECOVERY:
	if (typeof(row[CRED_HASH]) != T_STRING) {
	    error("identity: recovery row lacks a hash");
	}
	break;

    case CRED_TYPE_AGENT_KEY:
	if (typeof(row[CRED_KEY]) != T_STRING ||
	    typeof(row[CRED_SCHEME]) != T_STRING) {
	    error("identity: agent key row lacks key material");
	}
	break;

    case CRED_TYPE_AGENT_TOKEN:
	if (typeof(row[CRED_HASH]) != T_STRING ||
	    typeof(row[CRED_EXPIRES]) != T_INT ||
	    row[CRED_EXPIRES] <= 0) {
	    error("identity: agent token row lacks hash or expiry");
	}
	break;

    default:
	error("identity: unknown credential type");
    }
}

/*
 * the row-kind discipline: agent rows bind only to agent records,
 * human rows (passkeys, recovery codes) only to human records
 */
private int agent_row(mapping row)
{
    return row[CRED_TYPE] == CRED_TYPE_AGENT_KEY ||
	   row[CRED_TYPE] == CRED_TYPE_AGENT_TOKEN;
}

/*
 * bind a row on a record and in the global index; the credential id
 * must be globally unused and the row type must match the record kind
 */
private void bind_row(object identity, string uuid, string id, mapping row)
{
    if (credentialIndex[id]) {
	error("identity: credential already bound");
    }
    if (agent_row(row)) {
	if (identity->query_kind() != ID_KIND_AGENT) {
	    error("identity: agent credential on a non-agent record");
	}
    } else if (identity->query_kind() != ID_KIND_HUMAN) {
	error("identity: human credential on an agent record");
    }
    identity->add_credential(id, row);
    credentialIndex[id] = uuid;
}

private void unbind_row(object identity, string id)
{
    identity->remove_credential(id);
    credentialIndex[id] = nil;
}


/*
 * create an identity with its first credential; a record never exists
 * empty
 */
atomic string create_identity(string credentialId, mapping row)
{
    object identity;
    string uuid;

    check_system(previous_program());
    validate_row(credentialId, row);
    uuid = generate_uuid();
    identity = clone_object(OBJ_IDENTITY);
    identity->configure(uuid);
    identities[uuid] = identity;
    bind_row(identity, uuid, credentialId, row);
    return uuid;
}

/*
 * operator mint: create an identity whose initial credentials are n
 * freshly generated recovery codes; returns ({ uuid, code... }) --
 * the only time the plaintext codes exist
 */
atomic mixed *mint_with_codes(int n)
{
    object identity;
    string uuid, code, hash;
    mixed *result;
    int i;

    check_system(previous_program());
    if (n < 1 || n > MAX_CODES) {
	error("identity: 1 to " + (string) MAX_CODES + " recovery codes");
    }
    uuid = generate_uuid();
    identity = clone_object(OBJ_IDENTITY);
    identity->configure(uuid);
    identities[uuid] = identity;
    result = allocate(n + 1);
    result[0] = uuid;
    for (i = 0; i < n; i++) {
	code = generate_code();
	hash = code_hash(code);
	bind_row(identity, uuid, code_id(hash), code_row(hash));
	result[i + 1] = code;
    }
    return result;
}

/*
 * bind an additional credential to an existing identity
 */
atomic void bind_credential(string uuid, string credentialId, mapping row)
{
    check_system(previous_program());
    validate_row(credentialId, row);
    bind_row(need_identity(uuid), uuid, credentialId, row);
}

/*
 * remove a credential; refuses to empty the record
 */
atomic void unbind_credential(string uuid, string credentialId)
{
    object identity;

    check_system(previous_program());
    identity = need_identity(uuid);
    if (identity->query_credential_count() <= 1) {
	error("identity: record cannot reach zero credentials");
    }
    unbind_row(identity, credentialId);
}

/*
 * rotation: bind the replacement, then remove the old row, as one
 * atomic step -- the record never passes through a zero-credential or
 * old-only state observable from outside
 */
atomic void rotate_credential(string uuid, string newId, mapping row,
			      string oldId)
{
    object identity;

    check_system(previous_program());
    validate_row(newId, row);
    identity = need_identity(uuid);
    bind_row(identity, uuid, newId, row);
    unbind_row(identity, oldId);
}

/*
 * replace the record's recovery-code set with n fresh codes; returns
 * the plaintext codes. Removals run first and the never-zero check
 * runs last, so a rotation that would empty a codes-only record
 * aborts and rolls back whole.
 */
atomic string *rotate_recovery_codes(string uuid, int n)
{
    object identity;
    string *ids, *codes;
    string code, hash;
    mapping row;
    int i;

    check_system(previous_program());
    if (n < 0 || n > MAX_CODES) {
	error("identity: 0 to " + (string) MAX_CODES + " recovery codes");
    }
    identity = need_identity(uuid);
    ids = identity->query_credential_ids();
    for (i = 0; i < sizeof(ids); i++) {
	row = identity->query_credential(ids[i]);
	if (row[CRED_TYPE] == CRED_TYPE_RECOVERY) {
	    unbind_row(identity, ids[i]);
	}
    }
    codes = allocate(n);
    for (i = 0; i < n; i++) {
	code = generate_code();
	hash = code_hash(code);
	bind_row(identity, uuid, code_id(hash), code_row(hash));
	codes[i] = code;
    }
    if (identity->query_credential_count() == 0) {
	error("identity: record cannot reach zero credentials");
    }
    return codes;
}

/*
 * redeem a recovery code: single-use, so a successful match removes
 * the row. Refuses to consume the last credential -- recovery that
 * replaces the credential set pairs redemption with a bind in
 * redeem_and_replace below.
 */
atomic int redeem_recovery_code(string uuid, string code)
{
    object identity;
    string id;
    mapping row;

    check_system(previous_program());
    identity = need_identity(uuid);
    id = code_id(code_hash(code));
    row = identity->query_credential(id);
    if (!row || row[CRED_TYPE] != CRED_TYPE_RECOVERY ||
	row[CRED_HASH] != code_hash(code)) {
	error("identity: unknown recovery code");
    }
    if (identity->query_credential_count() <= 1) {
	error("identity: cannot redeem the last credential; " +
	      "pair redemption with a replacement");
    }
    unbind_row(identity, id);
    return TRUE;
}

/*
 * the recovery ceremony's substrate step: redeem a code and bind the
 * replacement credential as one atomic operation, valid even when the
 * code is the record's last credential
 */
atomic void redeem_and_replace(string uuid, string code, string newId,
			       mapping row)
{
    object identity;
    string id;
    mapping old;

    check_system(previous_program());
    validate_row(newId, row);
    identity = need_identity(uuid);
    id = code_id(code_hash(code));
    old = identity->query_credential(id);
    if (!old || old[CRED_TYPE] != CRED_TYPE_RECOVERY ||
	old[CRED_HASH] != code_hash(code)) {
	error("identity: unknown recovery code");
    }
    bind_row(identity, uuid, newId, row);
    unbind_row(identity, id);
}

/*
 * agent minting: an agent record is an ordinary identity plus kind
 * agent and an immutable controller edge that must name an existing
 * human record -- control never chains, so delegation depth is bounded
 * by the record shape
 */
private object new_agent(string controllerUuid, string uuid)
{
    object controller, identity;

    controller = need_identity(controllerUuid);
    if (controller->query_kind() != ID_KIND_HUMAN) {
	error("identity: an agent's controller must be a human identity");
    }
    identity = clone_object(OBJ_IDENTITY);
    identity->configure(uuid, controllerUuid);
    identities[uuid] = identity;
    if (!controllerIndex) {
	controllerIndex = ([ ]);
    }
    if (!controllerIndex[controllerUuid]) {
	controllerIndex[controllerUuid] = ([ ]);
    }
    controllerIndex[controllerUuid][uuid] = 1;
    return identity;
}

/*
 * mint an agent with a caller-supplied agent-key credential (public
 * key material only; the private key never reaches the platform)
 */
atomic string mint_agent(string controllerUuid, string credentialId,
			 mapping row)
{
    object identity;
    string uuid;

    check_system(previous_program());
    validate_row(credentialId, row);
    if (!agent_row(row)) {
	error("identity: an agent needs an agent credential");
    }
    uuid = generate_uuid();
    identity = new_agent(controllerUuid, uuid);
    bind_row(identity, uuid, credentialId, row);
    return uuid;
}

/*
 * mint an agent whose first credential is a fresh agent token; returns
 * ({ uuid, token }) -- the only time the plaintext exists
 */
atomic string *mint_agent_with_token(string controllerUuid, varargs int ttl)
{
    object identity;
    string uuid, token, hash;

    check_system(previous_program());
    uuid = generate_uuid();
    identity = new_agent(controllerUuid, uuid);
    token = generate_token();
    hash = code_hash(token);
    bind_row(identity, uuid, token_id(hash),
	     token_row(hash, token_expiry(ttl)));
    return ({ uuid, token });
}

/*
 * bind a fresh agent token to an existing agent record; returns
 * ({ credential id, token })
 */
atomic string *bind_agent_token(string uuid, varargs int ttl)
{
    object identity;
    string token, hash;

    check_system(previous_program());
    identity = need_identity(uuid);
    if (identity->query_kind() != ID_KIND_AGENT) {
	error("identity: agent tokens bind to agent records");
    }
    token = generate_token();
    hash = code_hash(token);
    bind_row(identity, uuid, token_id(hash),
	     token_row(hash, token_expiry(ttl)));
    return ({ token_id(hash), token });
}

/*
 * suspend an agent: authentication refuses at ceremony time and every
 * live session dies now. The record and its audit trail persist.
 * Returns the count of sessions revoked.
 */
atomic int suspend_agent(string uuid)
{
    object identity;

    check_system(previous_program());
    identity = need_identity(uuid);
    identity->set_suspended(TRUE);
    return SESSIOND->revoke_principal(identity->query_principal());
}

/*
 * resume restores the ability to authenticate only; anything revoked
 * at suspension is re-established deliberately, never automatically
 */
void resume_agent(string uuid)
{
    check_system(previous_program());
    need_identity(uuid)->set_suspended(FALSE);
}

/*
 * WebAuthn bookkeeping on a bound passkey
 */
void update_sign_count(string uuid, string credentialId, int count)
{
    object identity;

    check_system(previous_program());
    identity = need_identity(uuid);
    if (!identity->query_credential(credentialId)) {
	error("identity: no such credential");
    }
    identity->update_credential(credentialId, CRED_SIGNCOUNT, count);
    identity->update_credential(credentialId, CRED_LASTUSED, time());
}


/*
 * lookups
 */
object find_identity(string uuid)
{
    check_system(previous_program());
    return identities[uuid];
}

string find_by_credential(string credentialId)
{
    check_system(previous_program());
    return credentialIndex[credentialId];
}

int query_identity_count()
{
    check_system(previous_program());
    return map_sizeof(identities);
}

/*
 * the agents a controller's uuid controls
 */
string *query_agents(string controllerUuid)
{
    mapping set;

    check_system(previous_program());
    set = controllerIndex ? controllerIndex[controllerUuid] : nil;
    return set ? map_indices(set) : ({ });
}


/*
 * capability binding: the operator grant path for platform capabilities
 * to an authenticated identity. capabilityd's grant/revoke stay
 * KERNEL()-gated; this routes through the console registry's
 * KERNEL-elevated, admin_console.caller-gated verb_grant_capability
 * helper, which also constrains the principal to the identity namespace.
 * The identity's principal string is derived here from its uuid, never
 * caller-supplied, so an operator grants to "the identity", not to an
 * arbitrary principal.
 *
 * Grants are source-tracked: capabilityd's store entry is one bit, but
 * two reasons for it can coexist (an operator grant and, on an agent, a
 * controller's delegation of the same capability). Each grant path
 * records its source and a revocation drops the store entry only when
 * the last source dies -- otherwise revoking one path would silently
 * destroy the other's grant, or destroy nothing.
 */
private void add_grant_source(string uuid, string capability, string source)
{
    if (!grantSources) {
	grantSources = ([ ]);
    }
    if (!grantSources[uuid]) {
	grantSources[uuid] = ([ ]);
    }
    if (!grantSources[uuid][capability]) {
	grantSources[uuid][capability] = ([ ]);
    }
    grantSources[uuid][capability][source] = 1;
}

/*
 * drop one source; TRUE iff no source remains for (uuid, capability),
 * including the pre-tracking case where no source was ever recorded
 */
private int drop_grant_source(string uuid, string capability, string source)
{
    mapping byCap, set;

    if (!grantSources || !(byCap = grantSources[uuid]) ||
	!(set = byCap[capability])) {
	return TRUE;
    }
    set[source] = nil;
    if (map_sizeof(set) == 0) {
	byCap[capability] = nil;
	if (map_sizeof(byCap) == 0) {
	    grantSources[uuid] = nil;
	}
	return TRUE;
    }
    return FALSE;
}

string *query_grant_sources(string uuid, string capability)
{
    mapping byCap, set;

    check_system(previous_program());
    set = (grantSources && (byCap = grantSources[uuid])) ?
	   byCap[capability] : nil;
    return set ? map_indices(set) : ({ });
}

atomic void grant_capability(string uuid, string capability)
{
    object identity;

    check_system(previous_program());
    if (!capability || strlen(capability) == 0) {
	error("identity: a grant needs a capability");
    }
    identity = need_identity(uuid);
    ADMIN_CONSOLE_REGISTRY->verb_grant_capability(capability,
						  identity->query_principal());
    add_grant_source(uuid, capability, GRANT_SOURCE_OPERATOR);
}

atomic void revoke_capability(string uuid, string capability)
{
    object identity;

    check_system(previous_program());
    identity = need_identity(uuid);
    if (drop_grant_source(uuid, capability, GRANT_SOURCE_OPERATOR)) {
	ADMIN_CONSOLE_REGISTRY->verb_revoke_capability(capability,
						       identity->query_principal());
    }
}

/*
 * the platform capabilities an identity's principal holds, read back
 * through capabilityd's public introspection
 */
string *query_grants(string uuid)
{
    object identity;
    string principal, *caps, *held;
    int i;

    check_system(previous_program());
    identity = need_identity(uuid);
    principal = identity->query_principal();
    caps = CAPABILITYD->query_capabilities();
    held = ({ });
    for (i = 0; i < sizeof(caps); i++) {
	if (CAPABILITYD->is_allowed(caps[i], principal)) {
	    held += ({ caps[i] });
	}
    }
    return held;
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
 * one record, formatted for the operator
 */
private string show_identity(string uuid)
{
    object identity;
    string *ids, *grants, text;
    mapping row;
    int i;

    identity = identities[uuid];
    if (!identity) {
	return "identity: no such identity\n";
    }
    ids = identity->query_credential_ids();
    text = "identity: principal " + identity->query_principal() + "\n" +
	   "identity: credentials: " + (string) sizeof(ids) + "\n";
    for (i = 0; i < sizeof(ids); i++) {
	row = identity->query_credential(ids[i]);
	text += "identity:   " + ids[i] + " " + row[CRED_TYPE] +
		((row[CRED_TYPE] == CRED_TYPE_PASSKEY) ?
		  " signCount " + (string) row[CRED_SIGNCOUNT] : "") + "\n";
    }
    grants = query_grants(uuid);
    text += "identity: capabilities: " + (string) sizeof(grants) + "\n";
    for (i = 0; i < sizeof(grants); i++) {
	text += "identity:   grant " + grants[i] + "\n";
    }
    return text;
}

/*
 * NAME:	cmd_identity()
 * DESCRIPTION:	the identity operator verb, dispatched by the kernel
 *		admin-console registry:
 *		  identity                        -- status
 *		  identity mint <n>               -- new identity with n
 *		                                     recovery codes
 *		  identity show <uuid>            -- one record
 *		  identity rotate-codes <uuid> <n> -- replace the code set
 *		  identity redeem <uuid> <code>   -- consume one code
 *		  identity revoke <uuid> <id>     -- remove a credential
 *		  identity grant <uuid> <cap>     -- grant a platform capability
 *		  identity ungrant <uuid> <cap>   -- revoke a platform capability
 */
void cmd_identity(object user, string cmd, string str)
{
    string *parts, err;
    mixed *minted;
    string *codes;
    int i;

    if (!KERNEL()) {
	error("Access denied");
    }

    parts = str ? explode(str, " ") - ({ "" }) : ({ });
    if (sizeof(parts) == 0) {
	_emit(user, "identity: identities: " +
		    (string) map_sizeof(identities) + "\n" +
		    "identity: crypto module: " +
# ifdef KF_SECURE_RANDOM
		    "present"
# else
		    "absent (minting unavailable)"
# endif
		    + "\n");
	return;
    }

    switch (parts[0]) {
    case "mint":
	if (sizeof(parts) != 2 || sscanf(parts[1], "%d", i) != 1) {
	    _emit(user, "usage: " + cmd + " mint <codes>\n");
	    return;
	}
	err = catch(minted = mint_with_codes(i));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, "identity: minted identity:" + minted[0] + " with " +
		    (string) (sizeof(minted) - 1) + " recovery codes\n");
	for (i = 1; i < sizeof(minted); i++) {
	    _emit(user, "identity: code " + (string) i + ": " + minted[i] +
			"\n");
	}
	_emit(user, "identity: store the codes now; only their hashes " +
		    "are kept\n");
	return;

    case "show":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " show <uuid>\n");
	    return;
	}
	_emit(user, show_identity(parts[1]));
	return;

    case "rotate-codes":
	if (sizeof(parts) != 3 || sscanf(parts[2], "%d", i) != 1) {
	    _emit(user, "usage: " + cmd + " rotate-codes <uuid> <codes>\n");
	    return;
	}
	err = catch(codes = rotate_recovery_codes(parts[1], i));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, "identity: rotated recovery codes: " +
		    (string) sizeof(codes) + "\n");
	for (i = 0; i < sizeof(codes); i++) {
	    _emit(user, "identity: code " + (string) (i + 1) + ": " +
			codes[i] + "\n");
	}
	return;

    case "redeem":
	if (sizeof(parts) != 3) {
	    _emit(user, "usage: " + cmd + " redeem <uuid> <code>\n");
	    return;
	}
	err = catch(redeem_recovery_code(parts[1], parts[2]));
	_emit(user, err ? err + "\n" : "identity: redeemed\n");
	return;

    case "revoke":
	if (sizeof(parts) != 3) {
	    _emit(user, "usage: " + cmd + " revoke <uuid> <credential-id>\n");
	    return;
	}
	err = catch(unbind_credential(parts[1], parts[2]));
	_emit(user, err ? err + "\n" : "identity: revoked\n");
	return;

    case "grant":
	if (sizeof(parts) != 3) {
	    _emit(user, "usage: " + cmd + " grant <uuid> <capability>\n");
	    return;
	}
	err = catch(grant_capability(parts[1], parts[2]));
	_emit(user, err ? err + "\n"
			: "identity: granted " + parts[2] + "\n");
	return;

    case "ungrant":
	if (sizeof(parts) != 3) {
	    _emit(user, "usage: " + cmd + " ungrant <uuid> <capability>\n");
	    return;
	}
	err = catch(revoke_capability(parts[1], parts[2]));
	_emit(user, err ? err + "\n"
			: "identity: ungranted " + parts[2] + "\n");
	return;

    default:
	_emit(user, "usage: " + cmd + " [mint <n> | show <uuid> | " +
		    "rotate-codes <uuid> <n> | redeem <uuid> <code> | " +
		    "revoke <uuid> <id> | grant <uuid> <capability> | " +
		    "ungrant <uuid> <capability>]\n");
	return;
    }
}
