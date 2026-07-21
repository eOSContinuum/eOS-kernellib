/*
 * Demo-page handler: serves the static page that drives the /auth and
 * /inventory routes from a real browser + authenticator -- the
 * browser-real counterpart of the headless foreign-vector driver
 * (sys/test.c). Registered under the /demo prefix by the domain initd;
 * the page itself is data/demo.html, read per request so an edit to
 * the deployed copy shows on the next reload. The per-request read
 * keeps the SERVER current; the Cache-Control: no-store header keeps
 * the BROWSER current -- without it Safari heuristically caches the
 * page and a returning visitor can sit on a stale copy until a hard
 * refresh.
 *
 * The interesting requirement is the origin: WebAuthn needs a secure
 * context whose origin matches the webauthnd relying-party
 * configuration (default https://localhost:8443), so this page is
 * only useful behind the labeled https port with a certificate the
 * browser genuinely trusts -- see the example README's browser-path
 * section for the recipe.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

mixed *handle(string method, string path, string body, string authorization)
{
    string page;

    if (method == "GET" && (path == "/demo" || path == "/demo/")) {
	page = read_file("/usr/Inventory/data/demo.html");
	if (!page) {
	    return ({ 500, "Internal Server Error",
		      "text/plain; charset=utf-8", "demo page missing\n" });
	}
	return ({ 200, "OK", "text/html; charset=utf-8", page,
		  ([ "Cache-Control" : "no-store" ]) });
    }
    return ({ 404, "Not Found", "text/plain; charset=utf-8",
	      "404 Not Found\n" });
}
