/*
 * https_server: the HTTPS (TLS) binary-port bootstrap.
 *
 * The first application binding of the shipped HTTP/1 TLS server
 * (/usr/HTTP/api/lib/TlsServer1): registers as the binary-port manager
 * under the "https" port label and clones the application's TLS server
 * mount per incoming connection, mirroring the plain-HTTP bootstrap
 * (sys/http_server.c) on the "http" label.
 *
 * Registration is conditional on all three of: the TLS stack compiled at
 * boot (the /usr/TLS initd's body is gated on the host driver being built
 * with KF_SECURE_RANDOM), an "https" port label declared (a second binary
 * port configured), and certificate plus key PEM present at the
 * configured paths (defaults below). Anything missing: log and stand
 * down -- a TLS-less boot is a supported posture, not an error.
 *
 * Certificate surface: acquisition and renewal belong to the host (an
 * ACME client such as certbot writing PEM into the platform's tree); the
 * daemon loads at boot and on the tls-cert operator verb, which also
 * re-points the configured paths and completes a deferred registration
 * without a restart -- a certificate that arrives after boot rides hot
 * reload, not a reboot. The paths persist as daemon state; the key
 * material never does.
 *
 * Credential custody: the manager holds no certificate or key in object
 * state, so there is nothing to persist into a statedump.
 * query_credentials reads the PEM files fresh per connection and answers
 * only the TLS server mount program -- the same trust grant as serving
 * connections through that mount. The TLS session drops the key at
 * handshake completion, so established connections carry none either.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>
# include <status.h>
# include <portd.h>

inherit "/usr/System/lib/auto";
inherit "/kernel/lib/capability";
private inherit "/lib/util/lpc";	/* sysLog */

# define APP_TLS_SERVER		"/usr/WWW/obj/tls_server"
# define TLS_SERVER_SESSION	"/usr/TLS/api/lib/ServerSession"
# define DEFAULT_CERT_PATH	"/usr/System/data/tls/cert.pem"
# define DEFAULT_KEY_PATH	"/usr/System/data/tls/key.pem"

private string certPath;	/* configured certificate PEM path */
private string keyPath;		/* configured private-key PEM path */

/*
 * NAME:	tls_compiled()
 * DESCRIPTION:	is the KF_SECURE_RANDOM-gated TLS stack compiled?
 */
private int tls_compiled()
{
    return (status(TLS_SERVER_SESSION, O_INDEX) != nil);
}

/*
 * NAME:	registered()
 * DESCRIPTION:	is this daemon the registered https-label manager?
 */
private int registered()
{
    mixed *slot;

    slot = PORTD->query_label("https");
    return (slot && slot[3] == this_object());
}

/*
 * NAME:	validate()
 * DESCRIPTION:	prove the configured credentials by constructing a TLS
 *		server session from them; nil on success, the failure
 *		otherwise
 */
private string validate()
{
    string cert, key;

    if (!file_info(certPath) || !(cert = read_file(certPath))) {
	return "no certificate at " + certPath;
    }
    if (!file_info(keyPath) || !(key = read_file(keyPath))) {
	return "no key at " + keyPath;
    }
    return catch(new_object(TLS_SERVER_SESSION, cert, key));
}

/*
 * NAME:	activate()
 * DESCRIPTION:	validate and register if not yet registered; return a
 *		status line describing the outcome
 */
private string activate()
{
    string err;

    if (!tls_compiled()) {
	return "TLS stack not compiled (host driver without " +
	       "KF_SECURE_RANDOM); standing down";
    }
    if (!PORTD->query_label("https")) {
	return "no \"https\" port label (no second binary port " +
	       "configured); standing down";
    }
    err = validate();
    if (err) {
	return err + "; standing down";
    }
    if (registered()) {
	return "credentials validated; already registered -- new " +
	       "connections use the new files immediately";
    }
    PORTD->register_manager("https", this_object());
    return "credentials validated; registered as the https manager";
}

/*
 * NAME:	create()
 * DESCRIPTION:	default paths, then attempt activation
 */
static void create()
{
    ::create();
    certPath = DEFAULT_CERT_PATH;
    keyPath = DEFAULT_KEY_PATH;
    sysLog("https: " + activate());
}

/*
 * select a per-connection TLS application server for an incoming
 * connection. Applications mount a clonable TLS server at
 * /usr/WWW/obj/tls_server, the HTTPS analog of the plain-HTTP
 * /usr/WWW/obj/server convention; the accept gate and the mount probe
 * mirror sys/http_server.c. If no TLS server is mounted, the connection
 * is dropped.
 */
object select(string str)
{
    object obj, po;

    po = previous_object();
    if (po && is_allowed("https.binary_manager", object_name(po)) &&
	status(APP_TLS_SERVER, O_INDEX) != nil) {
	catch (obj = clone_object(APP_TLS_SERVER));
	return obj;
    }
}

/*
 * return the connection mode for a freshly selected server: raw, because
 * the outer stream is TLS record ciphertext -- line mode would consume
 * the newline bytes it splits on
 */
int query_mode(object obj)
{
    return MODE_RAW;
}

/*
 * return the connection-login timeout
 */
int query_timeout(object obj)
{
    return DEFAULT_TIMEOUT;
}

/*
 * answer the TLS server mount with fresh-read PEM credentials
 */
string *query_credentials()
{
    string cert, key;

    if (previous_program() != APP_TLS_SERVER) {
	error("Access denied");
    }
    if (!file_info(certPath) || !file_info(keyPath)) {
	error("Certificate not available");
    }
    cert = read_file(certPath);
    key = read_file(keyPath);
    if (!cert || !key) {
	error("Certificate not available");
    }
    return ({ cert, key });
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
 * NAME:	cmd_tls_cert()
 * DESCRIPTION:	the tls-cert operator verb, dispatched by the kernel
 *		admin-console registry:
 *		  tls-cert                    -- status
 *		  tls-cert reload             -- revalidate; register if
 *		                                 boot stood down
 *		  tls-cert paths <cert> <key> -- re-point the configured
 *		                                 paths, then reload
 */
void cmd_tls_cert(object user, string cmd, string str)
{
    string *parts;

    if (!KERNEL()) {
	error("Access denied");
    }

    parts = str ? explode(str, " ") - ({ "" }) : ({ });
    if (sizeof(parts) == 0) {
	_emit(user, "tls-cert: certificate " + certPath + " (" +
		    (file_info(certPath) ? "present" : "missing") +
		    "), key " + keyPath + " (" +
		    (file_info(keyPath) ? "present" : "missing") + ")\n" +
		    "tls-cert: TLS stack " +
		    (tls_compiled() ? "compiled" : "not compiled") +
		    ", https label " +
		    (PORTD->query_label("https") ? "declared" :
		     "not declared") +
		    ", manager " +
		    (registered() ? "registered" : "not registered") + "\n");
	return;
    }

    switch (parts[0]) {
    case "reload":
	if (sizeof(parts) != 1) {
	    _emit(user, "usage: " + cmd + " reload\n");
	    return;
	}
	break;

    case "paths":
	if (sizeof(parts) != 3) {
	    _emit(user, "usage: " + cmd + " paths <cert-path> <key-path>\n");
	    return;
	}
	certPath = parts[1];
	keyPath = parts[2];
	break;

    default:
	_emit(user, "usage: " + cmd + " [reload | paths <cert> <key>]\n");
	return;
    }

    _emit(user, "tls-cert: " + activate() + "\n");
}
