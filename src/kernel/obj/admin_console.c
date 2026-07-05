# include <kernel/kernel.h>
# include <kernel/user.h>

inherit LIB_ADMIN_CONSOLE;

# define INDEX	"/usr/Index/sys/index_daemon"


private object user;		/* associated user object */

/*
 * NAME:	create()
 * DESCRIPTION:	initialize object
 */
static void create()
{
    ::create(200);
    user = this_user();
}

/*
 * NAME:	message()
 * DESCRIPTION:	pass on a message to the user
 */
static int message(string str)
{
    return user->message(str);
}

/*
 * NAME:	input()
 * DESCRIPTION:	deal with input from user
 */
void input(string str)
{
    if (previous_object() == user) {
	call_limited("process", str);
    }
}

/*
 * NAME:	process()
 * DESCRIPTION:	process user input
 */
static void process(string str)
{
    string arg;

    if (query_editor(this_object())) {
	if (strlen(str) != 0 && str[0] == '!') {
	    str = str[1 ..];
	} else {
	    str = editor(str);
	    if (str) {
		message(str);
	    }
	    return;
	}
    }

    if (str == "") {
	return;
    }

    sscanf(str, "%s %s", str, arg);
    if (arg == "") {
	arg = nil;
    }

    switch (str) {
    case "code":
    case "history":
    case "clear":
    case "compile":
    case "clone":
    case "destruct":
    case "new":

    case "cd":
    case "pwd":
    case "ls":
    case "cp":
    case "mv":
    case "rm":
    case "mkdir":
    case "rmdir":
    case "ed":

    case "access":
    case "grant":
    case "ungrant":
    case "quota":
    case "rsrc":

    case "people":
    case "status":
    case "swapout":
    case "snapshot":
    case "shutdown":
    case "reboot":
	call_other(this_object(), "cmd_" + str, user, str, arg);
	break;

    default:
	{
	    /*
	     * Selective extension. Unknown verbs route through the
	     * KERNEL-tier registry, which holds the verb -> (ext_path,
	     * method) dispatch table populated for /usr/-tier domains
	     * (Merry today; Vault/Schema/HTTP whenever future work
	     * extends the registry's create()). If the registry has no
	     * entry, or the registered extension is not loaded, the
	     * "No command" fallback fires.
	     *
	     * The registry is lazy-loaded -- DGD's string-form call_other
	     * does not auto-compile, so find_object + compile_object on
	     * first dispatch keeps the boot path free of an explicit
	     * registry compile in driver.c. Subsequent dispatches find
	     * the loaded master.
	     */
	    object reg;
	    mapping entry;
	    object ext;

	    reg = find_object(ADMIN_CONSOLE_REGISTRY);
	    if (!reg) {
		reg = compile_object(ADMIN_CONSOLE_REGISTRY);
	    }
	    if (reg) {
		entry = reg->query_dispatch(str);
		if (entry) {
		    ext = find_object(entry["path"]);
		    if (!ext) {
			ext = compile_object(entry["path"]);
		    }
		    if (ext) {
			call_other(ext, entry["method"], user, str, arg);
			break;
		    }
		}
	    }
	    message("No command: " + str + "\n");
	}
	break;
    }
}

/*
 * NAME:	translate_name()
 * DESCRIPTION:	resolve a colon-shaped verb argument: LPC object name
 *		first, then the Index logical-name registry -- the same
 *		order the coercion codec uses for object references.
 *		Returns the canonical object name on resolution, or nil
 *		after messaging when neither route resolves. Logical
 *		names are the sanctioned address for clones, which have
 *		no stable path-form address of their own.
 */
private string translate_name(string str)
{
    object obj, ixd;

    obj = find_object(str);
    if (!obj) {
	ixd = find_object(INDEX);
	if (ixd) {
	    obj = ixd->query_object(str);
	}
    }
    if (!obj) {
	message("No such object or Index name.\n");
	return nil;
    }
    return object_name(obj);
}

/*
 * The object-taking verbs, masked to accept Index logical names.
 * Only colon-shaped arguments (the logical-name grammar) are
 * translated; paths, $refs, and empty arguments pass through to the
 * library verb byte-identical to before. The library re-resolves the
 * substituted canonical name through its own machinery, so per-verb
 * guards (master-only for clone, LWO-master-only for new) behave
 * unchanged.
 */

/*
 * NAME:	cmd_clone()
 * DESCRIPTION:	clone, accepting Index logical names
 */
static void cmd_clone(object user, string cmd, string str)
{
    if (str && str[0] != '$' && sscanf(str, "%*s:") != 0) {
	str = translate_name(str);
	if (!str) {
	    return;
	}
    }
    ::cmd_clone(user, cmd, str);
}

/*
 * NAME:	cmd_destruct()
 * DESCRIPTION:	destruct, accepting Index logical names
 */
static void cmd_destruct(object user, string cmd, string str)
{
    if (str && str[0] != '$' && sscanf(str, "%*s:") != 0) {
	str = translate_name(str);
	if (!str) {
	    return;
	}
    }
    ::cmd_destruct(user, cmd, str);
}

/*
 * NAME:	cmd_new()
 * DESCRIPTION:	new, accepting Index logical names
 */
static void cmd_new(object user, string cmd, string str)
{
    if (str && str[0] != '$' && sscanf(str, "%*s:") != 0) {
	str = translate_name(str);
	if (!str) {
	    return;
	}
    }
    ::cmd_new(user, cmd, str);
}

/*
 * NAME:	cmd_status()
 * DESCRIPTION:	status, accepting Index logical names
 */
static void cmd_status(object user, string cmd, string str)
{
    if (str && str[0] != '$' && sscanf(str, "%*s:") != 0) {
	str = translate_name(str);
	if (!str) {
	    return;
	}
    }
    ::cmd_status(user, cmd, str);
}
