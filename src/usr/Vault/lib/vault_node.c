/*
 * Vault node — inherited by each domain directory that wants to be part
 * of the persistent state storage system. Provides the spawn-create +
 * spawn-configure pipeline that the Vault daemon calls during single-
 * object load.
 *
 * Lifted from SkotOS /usr/SID/lib/vaultnode.c (commit feature/cohesive-lift,
 * LV-4) with the following changes per workstream Decisions:
 *
 *   - Note 1 (element distinction): root-element discriminator switched
 *     from the SkotOS /obj/ path-convention heuristic
 *     (sscanf(program_attr, "%*s/obj/")) to structural element-name check
 *     <object> for singletons (find_or_load path) vs <clone> for instances
 *     (clone_object path). The element name carries the kind information,
 *     eliminating the fragile path-convention discriminator.
 *
 *   - Note 3 (distribution layer drops): removed the entire distribution
 *     machinery — query_local_blocking_objects, kill_local_blocking_objects,
 *     rename_local_blocking_objects, do_rename, do_disable, query_master,
 *     check_mastery, spawn_now, find_named_objects_recursively (function),
 *     parse_archives state machine (ST_CLEAR/ST_CREATE/ST_CONFIGURE),
 *     refresh_list_file, compile_archives, append, collect, compile,
 *     query_halt, halt(). Also dropped the state vars supporting those
 *     paths (blocking, include, exclude, master, halt, nice,
 *     total_errors, test_only) and the relay callback parameter on
 *     spawn_create_one. Cross-server mastery, .xma archive packing,
 *     blocker-rename, vault.lst include/exclude resolution all belong
 *     to a future cross-runtime workstream, not this lift.
 *
 *   - LV-2 path renames: VAULT daemon path -> ~Vault/sys/vault (defines
 *     in the inheriting daemon, not here); /usr/XML/lib/* -> ~XML/lib/*.
 *     The vault_node lib lives at ~Vault/lib/vault_node.
 *
 *   - LV-2.5 + LV-2.5b helper-name refactor: SysLog -> sysLog,
 *     Debug -> debugLog, dump_value -> dumpValue, ur_name(ob) inlined
 *     as object_name(ob), find_or_load -> findOrLoad,
 *     query_colour_value -> queryColourValue. lower_case stays
 *     (eOS-kernellib already has it in /lib/util/ascii).
 */

# include <type.h>

private inherit "/lib/util/lpc";

private inherit "~Marshal/XmlBinding/lib/stateimpex";
private inherit "~XML/lib/xmd";

inherit "~XML/lib/xmlparse";

# define VAULT		"/usr/Vault/sys/vault"
# define INDEX		"/usr/Index/sys/index_daemon"

/* LV-5 L8 closure: vault_node tests whether an object already exists.
 * find_object is a path-lookup kfun; logical names registered via
 * set_object_name (set_name on Index after LV-4.5d) do not resolve
 * through it. This helper tries path lookup first, then falls back to
 * Index->query_object. */
private object find_by_path_or_name(string name)
{
    object ob;

    if (ob = find_object(name)) {
	return ob;
    }
    return INDEX->query_object(name);
}

mapping initializers;

private string root;


static
void create(string str) {
   ::create();

   if (!str || !strlen(str)) {
      error("bad root");
   }
   if (str[strlen(str) - 1] == '/') {
      str = str[.. strlen(str) - 2];
   }
   root = str;

   VAULT->register_node();
}

void patch_register() {
   VAULT->register_node();
}

/*
 * NAME:        spawn_create_one()
 * DESCRIPTION: Create an object from XML root if it does not yet exist.
 *              Element-distinction (Note 1): <clone> -> clone_object path,
 *              <object> -> find_or_load path. Returns 1 for created clone,
 *              2 for created initializer clone, 3 for created lost
 *              initializer, 0 if object already existed.
 */
int spawn_create_one(string name, string path, string source) {
   string root_elem, program;
   object ob;
   mixed state;

   state = parse_xml(source, path, TRUE);	/* PEEK flag set */
   state = queryColourValue(xmdForceToData(state));
   if (sizeof(state) == 0) {
      error("XML missing root element");
   }
   root_elem = xmdElement(state[0]);
   if (root_elem != "object" && root_elem != "clone") {
      error("root element must be 'object' or 'clone', not " +
	    dumpValue(root_elem));
   }
   state = state[0];
   if (xmdAttributes(state)[0] != "program") {
      error("bad or missing 'program' attribute to root element");
   }
   program = xmdAttributes(state)[1];

   if (!find_by_path_or_name(name)) {
      if (root_elem == "clone") {
	 /* cloneable path */
	 string before, after;

	 ob = clone_object(program);
	 ob->set_object_name(name);

	 sysLog("Created object: " + name(ob));

	 /* for clones only, see if this is an initializer object */
	 if (sscanf(name, "%s:Initial:%s", before, after)) {
	    name = before + ":" + after;
	    if (!find_by_path_or_name(name)) {
	       /* only create if does not already exist */
	       ob = clone_object(program);
	       ob->set_object_name(name);
	       initializers[name] = TRUE;
	       sysLog("Adding initializer: " + dumpValue(initializers));
	       return 2;
	    }
	 }
	 return 1;
      }
      /* singleton (object) path */
      ob = find_object(program);
      if (!ob) {
	 ob = findOrLoad(program);
	 ob->set_object_name(name);
	 return 1;
      }
      ob->set_object_name(name);
      return 0;
   } else {
      /* object already exists; check initializer-for-destroyed-target case */
      if (root_elem == "clone") {
	 string before, after;

	 if (sscanf(name, "%s:Initial:%s", before, after)) {
	    name = before + ":" + after;
	    if (!find_by_path_or_name(name)) {
	       /* only create if does not already exist */
	       ob = clone_object(program);
	       ob->set_object_name(name);
	       initializers[name] = TRUE;
	       sysLog("Adding lost initializer: " + dumpValue(initializers));
	       return 3;
	    }
	 }
	 return 0;
      }
   }
}

/*
 * NAME:        spawn_configure_one()
 * DESCRIPTION: Apply state from XML body to an already-created object via
 *              stateimpex import_state. Handles the initializer-target
 *              configuration case for :Initial: clones.
 */
int spawn_configure_one(string name, string path, string source) {
   object ob;
   mixed state;

   if (ob = find_by_path_or_name(name)) {
      string before, after;

      state = parse_xml(source, path, FALSE, TRUE);
      /*
       * Assumption here is that the content of the source hasn't changed
       * since spawn_create_one() looked at it.
       */
      state = queryColourValue(xmdForceToData(state))[0];
      state = queryColourValue(xmdForceToData(xmdContent(state)));
      if (sizeof(state) > 1) {
	 error("root element has more than one child");
      }
      import_state(ob, state[0]);
      debugLog("Configured " + name);

      if (sscanf(name, "%s:Initial:%s", before, after)) {
	 name = before + ":" + after;
	 if (initializers[name]) {
	    if (ob = find_by_path_or_name(name)) {
	       import_state(ob, state[0]);
	       return 2;
	    }
	 }
      }
      return 1;
   }
   return 0;
}

/*
 * callback hook for inheritors to mask — invoked when archive parsing
 * completes (currently a no-op stub; archive parsing itself dropped per
 * Note 3).
 */
void archives_parsed() {}
