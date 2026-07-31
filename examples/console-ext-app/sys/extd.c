/*
 * ConsoleExt reference daemon: a first-class operator verb registered
 * through the kernel admin-console registry's capability-gated
 * extend() surface, instead of tunneling operator actions through the
 * console's code verb (whose _code objects compile outside the
 * application's domain and are correctly refused by same-domain caller
 * gates).
 *
 * The lifecycle this demonstrates:
 *   1. boot: create() attempts registration; before the operator has
 *      approved the domain the attempt is refused, and the refusal is
 *      recorded (query_boot_attempt) rather than failing the deploy.
 *   2. the operator approves the domain: console-ext approve ConsoleExt
 *   3. registration succeeds (register_verbs, driven from the console),
 *      and `ext-hello` dispatches to cmd_ext_hello like any built-in
 *      extension verb.
 *
 * cmd_ext_hello keeps the built-in extensions' caller posture: the
 * kernel admin console is the only legitimate caller, so a code-verb
 * tunnel (compiled outside /kernel) is refused. This file assumes the
 * ConsoleExt mount (deploy as console-ext-app:ConsoleExt).
 */

# include <kernel/kernel.h>
# include <kernel/user.h>

inherit "/usr/System/lib/auto";

# define EXT_PATH	"/usr/ConsoleExt/sys/extd"

private string boot_attempt;	/* create()-time registration outcome */

static void create()
{
    ::create();
    boot_attempt = catch(ADMIN_CONSOLE_REGISTRY->extend("ext-hello",
							EXT_PATH,
							"cmd_ext_hello"));
    if (!boot_attempt) {
	boot_attempt = "registered";
    }
}

string query_boot_attempt()
{
    return boot_attempt;
}

/*
 * the registration pair the operator (or a boot driver) calls once the
 * domain holds admin_console.extend
 */
void register_verbs()
{
    ADMIN_CONSOLE_REGISTRY->extend("ext-hello", EXT_PATH, "cmd_ext_hello");
}

void unregister_verbs()
{
    ADMIN_CONSOLE_REGISTRY->retract("ext-hello");
}

/*
 * pass-throughs so the registration surface's validation errors can be
 * driven from a domain program (the console's code objects are not
 * /usr/ConsoleExt-tier, so they cannot reach extend directly)
 */
void try_extend(string verb, string path, string method)
{
    ADMIN_CONSOLE_REGISTRY->extend(verb, path, method);
}

void try_retract(string verb)
{
    ADMIN_CONSOLE_REGISTRY->retract(verb);
}

/*
 * NAME:	cmd_ext_hello()
 * DESCRIPTION:	the registered operator verb, dispatched by the kernel
 *		admin console exactly like a built-in extension verb
 */
void cmd_ext_hello(object user, string cmd, string str)
{
    if (!KERNEL()) {
	error("Access denied");
    }
    if (user) {
	user->message("ext-hello: hello from the ConsoleExt domain" +
		      (str ? " to " + str : "") + "\n");
    }
}
