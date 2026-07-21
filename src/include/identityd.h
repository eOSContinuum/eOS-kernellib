/*
 * Identity registry: object paths and credential-row vocabulary for the
 * platform identity substrate.
 *
 * identityd (/usr/System/sys/identityd) owns the identity records: one
 * clone of /usr/System/obj/identity per identity, indexed by uuid and by
 * bound credential id. A record's principal string ("identity:<uuid>")
 * is the form capabilityd principals take when a platform capability is
 * granted to an authenticated identity. Credential rows are mappings
 * over the CRED_* keys below; the row types are passkeys (public key
 * material, WebAuthn bookkeeping) and hashed single-use recovery codes
 * on human records, and agent keys (public key material) and hashed
 * expiring agent tokens on agent records. A record's kind (ID_KIND_*)
 * separates the two: an agent record carries an immutable controller
 * edge naming the human identity accountable for it, and only agent
 * records can be suspended.
 */

# define IDENTITYD	"/usr/System/sys/identityd"
# define OBJ_IDENTITY	"/usr/System/obj/identity"
# define WEBAUTHND	"/usr/System/sys/webauthnd"
# define SESSIOND	"/usr/System/sys/sessiond"
# define AGENTAUTHD	"/usr/System/sys/agentauthd"

/* credential row keys */
# define CRED_TYPE		"type"		/* row type, below */
# define CRED_ALG		"alg"		/* COSE algorithm (int) */
# define CRED_KEY		"key"		/* raw public key (string) */
# define CRED_SCHEME		"scheme"	/* crypto-kfun verify scheme */
# define CRED_SIGNCOUNT		"signCount"	/* authenticator counter */
# define CRED_TRANSPORTS	"transports"	/* string array, optional */
# define CRED_FLAGS		"flags"		/* authData flags (int) */
# define CRED_HASH		"hash"		/* recovery-code / agent-token
						   hash (hex) */
# define CRED_CREATED		"created"	/* row creation time() */
# define CRED_LASTUSED		"lastUsed"	/* last successful use */
# define CRED_EXPIRES		"expires"	/* expiry time(); required on
						   agent-token rows */

/* credential row types */
# define CRED_TYPE_PASSKEY	"passkey"
# define CRED_TYPE_RECOVERY	"recovery-code"
# define CRED_TYPE_AGENT_KEY	"agent-key"
# define CRED_TYPE_AGENT_TOKEN	"agent-token"

/* record kinds; a nil kind on a stored record reads as human */
# define ID_KIND_HUMAN		"human"
# define ID_KIND_AGENT		"agent"

/* agent-token expiry policy: expiry is required; a bind may set any
   ttl up to the cap, and a non-positive ttl takes the default */
# define AGENT_TOKEN_TTL	2592000		/* 30 days */
# define AGENT_TOKEN_MAX_TTL	7776000		/* 90 days */

/* an agent-token row's credential id, derived from the token's SHA-256
   hex hash -- shared by the minting side (identityd) and the ceremony
   side (agentauthd) */
# define AGENT_TOKEN_ID(hash)	("at:" + (hash)[.. 11])

/* the agent key ceremony signs a domain-separated message (this tag +
   the challenge), so an agent-auth signature can never be replayed
   into another protocol that has the agent sign something */
# define AGENT_AUTH_DOMAIN	"eos-agent-auth-v1:"

/* capability-grant bookkeeping sources (identityd tracks why a grant
   exists so revocation removes the store entry only with its last
   source) */
# define GRANT_SOURCE_OPERATOR	"operator"
# define GRANT_SOURCE_DELEGATED	"delegated"

/* the mutation-notification seam: a sys-tier daemon (the WWW servers'
   push_event trust boundary) registers once via subscribe_events(obj);
   every committed mutation then delivers identity_event(event, data)
   to each observer in its own zero-delay call_out, armed inside the
   atomic mutator -- an aborted mutation rolls its notifications back
   with everything else, and an observer failure (catch-wrapped, in its
   own execution round) never touches identity state. Destructed
   observers drop from the object-keyed registry on their own; there is
   no explicit unsubscribe. Event data is a mapping carrying only what
   the audit trails and the console already surface -- uuids, capability
   names, credential ids, counts; never key material, hashes, or
   plaintext. The keys each event carries are noted beside its name */
# define IDEV_CREATED		"created"	/* uuid, kind */
# define IDEV_RECOVERED		"recovered"	/* uuid */
# define IDEV_CRED_BOUND	"credential-bound"	/* uuid,
					   credentialId, type */
# define IDEV_CRED_REMOVED	"credential-removed"	/* uuid,
					   credentialId, type */
# define IDEV_CODES_ROTATED	"codes-rotated"	/* uuid, count */
# define IDEV_AGENT_MINTED	"agent-minted"	/* uuid, controller */
# define IDEV_SUSPENDED		"suspended"	/* uuid, revoked (the
					   live-session count) */
# define IDEV_RESUMED		"resumed"	/* uuid */
# define IDEV_GRANTED		"granted"	/* uuid, capability */
# define IDEV_REVOKED		"revoked"	/* uuid, capability */
# define IDEV_DELEGATED		"delegated"	/* agent, controller,
					   capability */
# define IDEV_UNDELEGATED	"undelegated"	/* agent, controller,
					   capability */
