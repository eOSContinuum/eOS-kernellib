/*
 * Session daemon: mints and validates bearer session tokens for
 * authenticated principals. The primitive only -- no cookie handling
 * and no HTTP bearer parsing ship here. The surface is System-tier; a
 * tier-E transport reaches sessions through the authd facade
 * (sys/authd.c), which mints only for a ceremony-proven principal.
 *
 * Secret discipline: a token's plaintext exists only in the mint
 * response. What persists is SHA-256(token) -> a session record
 * (principal, created, expires); validation hashes the presented token
 * and looks the hash up. The plaintext is never stored, so it cannot
 * reach the statedump -- scripts/session-smoke.sh proves this against a
 * live image, with a scan for the plaintext token bytes and the
 * principal string as a positive control. This follows the platform's
 * transient-secret rule (a secret is cleared when its use ends because
 * a statedump retains an object's bytes until reclaimed): the mint
 * function holds the plaintext in one local, returns it, and retains
 * nothing.
 *
 * Hashing and randomness need the host crypto module; without it the
 * daemon boots, reports the stand-down, and refuses minting cleanly.
 * The surface is System/kernel-tier; the operator face is the
 * "session" console verb.
 */

# include <kernel/kernel.h>
# include <kfun.h>
# include <type.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";
private inherit "/lib/util/lpc";	/* sysLog */
private inherit base64 "/lib/util/base64";
private inherit hex "/lib/util/hex";

# define DEFAULT_TTL	3600	/* one hour */
# define MAX_TTL		86400	/* one day */

# define SESS_PRINCIPAL	"principal"
# define SESS_CREATED	"created"
# define SESS_EXPIRES	"expires"

private mapping sessions;	/* token hash (hex) : session record */

static void create()
{
    ::create();
    sessions = ([ ]);
    sysLog("session: daemon up; crypto module " +
# ifdef KF_SECURE_RANDOM
	   "present"
# else
	   "absent (minting unavailable)"
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

private string token_hash(string token)
{
# ifdef KF_SECURE_RANDOM
    return hex::format(hash_string("SHA256", token));
# else
    error("session: crypto module not loaded");
# endif
}

/*
 * drop any expired records; called opportunistically at each mutating
 * or reading entry so an expired session never validates and the image
 * does not accumulate dead records
 */
private void reap(int now)
{
    string *hashes;
    int i;

    hashes = map_indices(sessions);
    for (i = 0; i < sizeof(hashes); i++) {
	if (sessions[hashes[i]][SESS_EXPIRES] <= now) {
	    sessions[hashes[i]] = nil;
	}
    }
}

/*
 * mint a session for a principal; returns the plaintext token (the only
 * time it exists). ttl caps at MAX_TTL; a non-positive ttl uses the
 * default.
 */
string mint(string principal, varargs int ttl)
{
    string token;
    int now;

    check_system(previous_program());
    if (!principal || strlen(principal) == 0) {
	error("session: a session needs a principal");
    }
    if (ttl <= 0) {
	ttl = DEFAULT_TTL;
    } else if (ttl > MAX_TTL) {
	ttl = MAX_TTL;
    }
# ifdef KF_SECURE_RANDOM
    now = time();
    reap(now);
    token = base64::urlEncode(secure_random(32));
    sessions[token_hash(token)] =
	([ SESS_PRINCIPAL : principal,
	   SESS_CREATED : now,
	   SESS_EXPIRES : now + ttl ]);
    return token;
# else
    error("session: crypto module not loaded");
# endif
}

/*
 * the principal a live token authenticates, or nil for an unknown or
 * expired token
 */
string validate(string token)
{
    mapping record;
    int now;

    check_system(previous_program());
    if (!token) {
	return nil;
    }
    now = time();
    record = sessions[token_hash(token)];
    if (!record) {
	return nil;
    }
    if (record[SESS_EXPIRES] <= now) {
	sessions[token_hash(token)] = nil;
	return nil;
    }
    return record[SESS_PRINCIPAL];
}

/*
 * revoke one session by its token; TRUE iff a live session was removed
 */
int revoke(string token)
{
    check_system(previous_program());
    if (token && sessions[token_hash(token)]) {
	sessions[token_hash(token)] = nil;
	return TRUE;
    }
    return FALSE;
}

/*
 * revoke every session for a principal (a logout-everywhere / account
 * lock primitive); returns the count removed
 */
int revoke_principal(string principal)
{
    string *hashes;
    int i, removed;

    check_system(previous_program());
    hashes = map_indices(sessions);
    for (i = 0; i < sizeof(hashes); i++) {
	if (sessions[hashes[i]][SESS_PRINCIPAL] == principal) {
	    sessions[hashes[i]] = nil;
	    removed++;
	}
    }
    return removed;
}

int query_session_count()
{
    check_system(previous_program());
    reap(time());
    return map_sizeof(sessions);
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
 * NAME:	cmd_session()
 * DESCRIPTION:	the session operator verb, dispatched by the kernel
 *		admin-console registry:
 *		  session                       -- status
 *		  session mint <principal> [ttl] -- new session token
 *		  session validate <token>      -- the principal, or none
 *		  session revoke <token>        -- drop one session
 *		  session revoke-principal <p>  -- drop all of a principal's
 */
void cmd_session(object user, string cmd, string str)
{
    string *parts, err, token, principal;
    int ttl, removed;

    if (!KERNEL()) {
	error("Access denied");
    }

    parts = str ? explode(str, " ") - ({ "" }) : ({ });
    if (sizeof(parts) == 0) {
	_emit(user, "session: sessions: " +
		    (string) query_session_count() + "\n" +
		    "session: crypto module: " +
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
	if (sizeof(parts) < 2 || sizeof(parts) > 3) {
	    _emit(user, "usage: " + cmd + " mint <principal> [ttl]\n");
	    return;
	}
	ttl = (sizeof(parts) == 3 && sscanf(parts[2], "%d", ttl)) ? ttl : 0;
	err = catch(token = mint(parts[1], ttl));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, "session: minted for " + parts[1] + "\n" +
		    "session: token " + token + "\n" +
		    "session: present the token now; only its hash is kept\n");
	return;

    case "validate":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " validate <token>\n");
	    return;
	}
	err = catch(principal = validate(parts[1]));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, principal ? "session: valid, principal " + principal + "\n"
			      : "session: no live session for that token\n");
	return;

    case "revoke":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " revoke <token>\n");
	    return;
	}
	err = catch(removed = revoke(parts[1]));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, removed ? "session: revoked\n"
			    : "session: no live session for that token\n");
	return;

    case "revoke-principal":
	if (sizeof(parts) != 2) {
	    _emit(user, "usage: " + cmd + " revoke-principal <principal>\n");
	    return;
	}
	err = catch(removed = revoke_principal(parts[1]));
	if (err) {
	    _emit(user, err + "\n");
	    return;
	}
	_emit(user, "session: revoked " + (string) removed +
		    " session(s)\n");
	return;

    default:
	_emit(user, "usage: " + cmd + " [mint <principal> [ttl] | " +
		    "validate <token> | revoke <token> | " +
		    "revoke-principal <principal>]\n");
	return;
    }
}
