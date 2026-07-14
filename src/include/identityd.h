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
 * material, WebAuthn bookkeeping) and hashed single-use recovery codes.
 */

# define IDENTITYD	"/usr/System/sys/identityd"
# define OBJ_IDENTITY	"/usr/System/obj/identity"
# define WEBAUTHND	"/usr/System/sys/webauthnd"
# define SESSIOND	"/usr/System/sys/sessiond"

/* credential row keys */
# define CRED_TYPE		"type"		/* row type, below */
# define CRED_ALG		"alg"		/* COSE algorithm (int) */
# define CRED_KEY		"key"		/* raw public key (string) */
# define CRED_SCHEME		"scheme"	/* crypto-kfun verify scheme */
# define CRED_SIGNCOUNT		"signCount"	/* authenticator counter */
# define CRED_TRANSPORTS	"transports"	/* string array, optional */
# define CRED_FLAGS		"flags"		/* authData flags (int) */
# define CRED_HASH		"hash"		/* recovery-code hash (hex) */
# define CRED_CREATED		"created"	/* row creation time() */
# define CRED_LASTUSED		"lastUsed"	/* last successful use */

/* credential row types */
# define CRED_TYPE_PASSKEY	"passkey"
# define CRED_TYPE_RECOVERY	"recovery-code"
