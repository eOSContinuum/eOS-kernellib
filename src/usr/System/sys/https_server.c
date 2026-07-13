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
 * port configured), and certificate plus key PEM present at the fixed
 * paths below. Anything missing: log and stand down -- a TLS-less boot is
 * a supported posture, not an error.
 *
 * Credential custody: the manager holds no certificate or key in object
 * state, so there is nothing to persist into a statedump.
 * query_credentials reads the PEM files fresh per connection and answers
 * only the TLS server mount program -- the same trust grant as serving
 * connections through that mount. Acquisition and renewal belong to the
 * host (ACME/certbot writing the fixed paths); the operator-facing load
 * and reload surface is roadmap work.
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
# define CERT_PATH		"/usr/System/data/tls/cert.pem"
# define KEY_PATH		"/usr/System/data/tls/key.pem"

/*
 * register as binary-port manager on the labeled HTTPS port, when the
 * TLS stack, the port, and the credentials are all present
 */
static void create()
{
    ::create();
    if (status(TLS_SERVER_SESSION, O_INDEX) == nil) {
	sysLog("https: TLS stack not compiled (host driver without " +
	       "KF_SECURE_RANDOM); not registering");
	return;
    }
    if (!PORTD->query_label("https")) {
	sysLog("https: no \"https\" port label (no second binary port " +
	       "configured); not registering");
	return;
    }
    if (!read_file(CERT_PATH) || !read_file(KEY_PATH)) {
	sysLog("https: certificate or key missing under " + CERT_PATH +
	       "; not registering");
	return;
    }
    PORTD->register_manager("https", this_object());
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
    cert = read_file(CERT_PATH);
    key = read_file(KEY_PATH);
    if (!cert || !key) {
	error("Certificate not available");
    }
    return ({ cert, key });
}
