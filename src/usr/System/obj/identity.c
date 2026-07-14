/*
 * One platform identity: the shared substrate record every application
 * consumes. Carries the fixed uuid, the principal string derived from
 * it, and the typed credential rows (passkeys, hashed recovery codes)
 * bound to it.
 *
 * All mutation goes through identityd: the daemon owns the cross-record
 * credential index and the never-zero-credentials invariant (validated
 * at the end of its atomic compound operations), so the record refuses
 * any other mutator. Reads are System-tier only; rows are returned as
 * copies so a caller cannot mutate the stored mapping behind the
 * daemon's back.
 */

# include <kernel/kernel.h>
# include <identityd.h>

private string uuid;		/* fixed at configure() */
private mapping credentials;	/* credential id : row mapping */
private int created;		/* record creation time */

static void create()
{
    credentials = ([ ]);
}

/*
 * identityd is the sole mutator; System-tier reads. The caller's
 * program is captured at each public entry and passed in --
 * previous_program() inside a private helper would name this file,
 * not the true external caller.
 */
private void check_daemon(string caller)
{
    if (caller != IDENTITYD) {
	error("Access denied");
    }
}

/*
 * fix the uuid; called once by identityd at creation
 */
void configure(string id)
{
    check_daemon(previous_program());
    if (uuid) {
	error("identity: already configured");
    }
    uuid = id;
    created = time();
}

/*
 * bind a credential row under an id unused on this record
 */
void add_credential(string id, mapping row)
{
    check_daemon(previous_program());
    if (!id || !row) {
	error("identity: bad credential");
    }
    if (credentials[id]) {
	error("identity: credential already bound");
    }
    credentials[id] = row;
}

/*
 * remove a credential row; the daemon polices the never-zero invariant
 */
void remove_credential(string id)
{
    check_daemon(previous_program());
    if (!credentials[id]) {
	error("identity: no such credential");
    }
    credentials[id] = nil;
}

/*
 * update one field of a bound row (signCount, lastUsed)
 */
void update_credential(string id, string field, mixed value)
{
    check_daemon(previous_program());
    if (!credentials[id]) {
	error("identity: no such credential");
    }
    credentials[id][field] = value;
}

private void check_system(string caller)
{
    if (sscanf(caller, "/usr/System/%*s") == 0 &&
	sscanf(caller, "/kernel/%*s") == 0) {
	error("Access denied");
    }
}

string query_uuid()
{
    check_system(previous_program());
    return uuid;
}

string query_principal()
{
    check_system(previous_program());
    return uuid ? "identity:" + uuid : nil;
}

int query_created()
{
    check_system(previous_program());
    return created;
}

string *query_credential_ids()
{
    check_system(previous_program());
    return map_indices(credentials);
}

int query_credential_count()
{
    check_system(previous_program());
    return map_sizeof(credentials);
}

/*
 * a copy of one row, or nil
 */
mapping query_credential(string id)
{
    mapping row;

    check_system(previous_program());
    row = credentials[id];
    return row ? row + ([ ]) : nil;
}
