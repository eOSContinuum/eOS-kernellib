# include <kernel/kernel.h>
# include <kernel/access.h>
# include <kernel/user.h>
# include <kernel/rsrc.h>
# include <status.h>

inherit access API_ACCESS;
inherit rsrc API_RSRC;


mapping resources;	/* saved initial resources at cold boot */

/*
 * NAME:	restore()
 * DESCRIPTION:	restore resource maxima for an owner
 */
private void restore(string owner)
{
    mapping rsrc;
    mixed max;

    rsrc = resources[owner];
    if (rsrc) {
	max = rsrc["fileblocks"];
	if (max) {
	    rsrc_set_limit(owner, "fileblocks", max);
	}
	max = rsrc["objects"];
	if (max) {
	    rsrc_set_limit(owner, "objects", max);
	}
	max = rsrc["stack"];
	if (max) {
	    rsrc_set_limit(owner, "stack", max);
	}
	max = rsrc["ticks"];
	if (max) {
	    rsrc_set_limit(owner, "ticks", max);
	}
    }
}

/*
 * NAME:	load()
 * DESCRIPTION:	compile and initialize an object
 */
private void load(string path)
{
    call_other(compile_object(path), "???");
}

/*
 * NAME:	create()
 * DESCRIPTION:	initialize the system
 */
static void create()
{
    string *owners, *domains;
    string domain;
    int i, sz;

    access::create();
    rsrc::create();

    /* object registry */
    load("sys/objectd");

    /* global access */
    set_global_access("System", TRUE);
    set_global_access("XML", TRUE);
    set_global_access("Schema", TRUE);
    set_global_access("Marshal", TRUE);
    set_global_access("Index", TRUE);
    set_global_access("Vault", TRUE);
    set_global_access("Merry", TRUE);

    /* server objects */
    load("sys/errord");
    load("sys/logd");
    load("sys/upgraded");
    load("sys/portd");
    load("sys/userd");
    load("sys/persist_helper");

    /* clonables */
    compile_object("obj/user");

    /* global objects */
    compile_object("/sys/utf8encode");
    compile_object("/sys/utf8decode");
    compile_object("/sys/jsonstrdecode");
    compile_object("/sys/jsonencode");
    compile_object("/sys/jsondecode");
    compile_object("/lib/IntIterator");
    compile_object("/lib/String");
    compile_object("/lib/StringBuffer");
    compile_object("/lib/Array");
    compile_object("/lib/GMTime");
    compile_object("/lib/ChainedContinuation");
    compile_object("/lib/DelayedContinuation");
    compile_object("/lib/IterativeContinuation");
    compile_object("/lib/DistContinuation");
    compile_object("/lib/KVstore");
    compile_object("/obj/kvnode");

    resources = ([ ]);
    restore_object("data/rsrc.dat");
    rsrc_incr(nil, "fileblocks",
	      DRIVER->file_size("/lib", TRUE) +
	      DRIVER->file_size("/obj", TRUE));
    owners = query_owners();
    for (i = 1, sz = sizeof(owners); i < sz; i++) {
	restore(owners[i]);
    }

    /* Domain stuff */
    domains = ({ "TLS", "HTTP", "LPC" });
    domains += get_dir("/usr/[A-Z]*")[0] - (domains + ({ "System" }));
    /* Two-pass: register all domain owners first so cross-domain inherits
     * resolve regardless of alphabetical-iteration order. */
    for (i = 0, sz = sizeof(domains); i < sz; i++) {
	domain = domains[i];
	add_owner(domain);
	restore(domain);

	rsrc_incr(domain, "fileblocks",
		  DRIVER->file_size("/usr/" + domain, TRUE));
    }
    for (i = 0, sz = sizeof(domains); i < sz; i++) {
	domain = domains[i];
	if (file_info("/usr/" + domain + "/initd.c")) {
	    load("/usr/" + domain + "/initd");
	}
    }

    /* HTTP/1 server on the first binary port */
    load("sys/http_server");
}

/*
 * NAME:	reboot()
 * DESCRIPTION:	get file quotas right after a reboot
 */
void reboot()
{
    if (previous_program() == DRIVER) {
	string *owners;
	int i, sz;

	rsrc_incr(nil, "fileblocks",
		  DRIVER->file_size("/doc", TRUE) +
		  DRIVER->file_size("/include", TRUE) +
		  DRIVER->file_size("/lib", TRUE) +
		  DRIVER->file_size("/obj", TRUE) +
		  DRIVER->file_size("/sys", TRUE) -
		  rsrc_get(nil, "fileblocks")[RSRC_USAGE]);
	owners = query_owners();
	for (i = 1, sz = sizeof(owners); i < sz; i++) {
	    rsrc_incr(owners[i], "fileblocks",
		      DRIVER->file_size("/usr/" + owners[i], TRUE) -
		      rsrc_get(owners[i], "fileblocks")[RSRC_USAGE]);
	}
    }
}
