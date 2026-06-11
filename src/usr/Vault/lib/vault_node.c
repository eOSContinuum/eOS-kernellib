/*
 * Vault node — inherited by each domain directory that wants to be part
 * of the persistent state storage system. Provides the spawn-create +
 * spawn-configure pipeline that the Vault daemon calls during single-
 * object load.
 *
 * The root-element name is the spawn discriminator: <object> for
 * singletons (find-or-load path) vs <clone> for instances
 * (clone_object path). The element name carries the kind information,
 * so no path-convention heuristic is needed. Cross-server mastery and
 * archive packing belong to a future cross-runtime layer, not here.
 */

# include <type.h>

private inherit "/lib/util/lpc";

private inherit "~Marshal/XmlBinding/lib/stateimpex";
private inherit "~XML/lib/xmd";

inherit "~XML/lib/xmlparse";

# define VAULT		"/usr/Vault/sys/vault"
# define INDEX		"/usr/Index/sys/index_daemon"

/* vault_node tests whether an object already exists. find_object is a
 * path-lookup kfun; logical names registered via set_object_name do
 * not resolve through it. This helper tries path lookup first, then
 * falls back to Index->query_object. */
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
