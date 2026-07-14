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
 *     minting response.
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

static void create()
{
    ::create();
    identities = ([ ]);
    credentialIndex = ([ ]);
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

    default:
	error("identity: unknown credential type");
    }
}

/*
 * bind a row on a record and in the global index; the credential id
 * must be globally unused
 */
private void bind_row(object identity, string uuid, string id, mapping row)
{
    if (credentialIndex[id]) {
	error("identity: credential already bound");
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
    string *ids, text;
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

    default:
	_emit(user, "usage: " + cmd + " [mint <n> | show <uuid> | " +
		    "rotate-codes <uuid> <n> | redeem <uuid> <code> | " +
		    "revoke <uuid> <id>]\n");
	return;
    }
}
