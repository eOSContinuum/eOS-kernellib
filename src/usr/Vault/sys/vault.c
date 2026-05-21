/*
 * Vault state-root daemon.
 *
 * Single-coherence-domain persistent object storage. Each participating
 * vault_node registers with this daemon at create time; the daemon
 * dispatches store/load operations and holds the name-to-path mapping
 * convention (colon-separated object names map to URL-encoded
 * filesystem path segments).
 *
 * Lifted from SkotOS /usr/SID/sys/vault.c (commit feature/cohesive-lift,
 * LV-3) with the following changes per workstream Decisions:
 *
 *   - Note 1 (element distinction): on-disk root form unchanged here;
 *     vault.c only stores daemons (singletons), always emits <object
 *     program="..."> form. The <clone .../> form for instances lives
 *     in vault_node.c (LV-4).
 *
 *   - Note 3 (distribution layer drops): query_blocking_objects(),
 *     save_hierarchy(), store_more(), eval_sam_ref() and the SAMD
 *     register_root() call dropped. Cross-server mastery, SAM-ref
 *     resolution, and hierarchy-save chain belong to a future cross-
 *     runtime workstream, not this lift.
 *
 *   - LV-2 path renames: /usr/SID/* -> ~Vault/* etc.; vault_node.c
 *     (renamed from vaultnode.c); set_object_name "Vault:Daemon"
 *     (was "SID:TheVault").
 *
 *   - LV-2.5 helper-name refactor: url_encode -> urlEncode (from
 *     /lib/util/url); pave_way -> paveWay, write_data_to_file ->
 *     writeDataToFile, copy_file -> copyFile (from /lib/util/file).
 *     Bare file kfuns (read_file, remove_file, rename_file) unchanged
 *     and resolve via cloud-server kernel auto-chain wrappers.
 *
 *   - LV-2.5b SkotOS-helper disposition: ur_name(ob) -> object_name(ob)
 *     inlined (SkotOS's ur_name was a shim around DGD's object_name
 *     kfun, present only to bypass SkotOS's own override; eOS-kernellib
 *     does not override object_name so the shim is unnecessary).
 *     INFO/SysLog -> info/sysLog (no-op stubs in /lib/util/lpc, pending
 *     kernel-layer log facility). dump_value -> dumpValue (richer
 *     recursive stringifier in /lib/util/lpc, ported from
 *     admin_console.c). lower_case via /lib/util/ascii inherit.
 *     map(arr, "fname") call patterns rewritten as explicit for-loops
 *     since /lib/util/url helpers are static (inheritance-visible only).
 */

# include <type.h>
# include <XML.h>

inherit "/lib/util/url";

private inherit "/lib/util/ascii";
private inherit "/lib/util/file";
private inherit "/lib/util/lpc";

inherit "/lib/util/named";

private inherit "~Marshal/XmlBinding/lib/stateimpex";
private inherit "~XML/lib/xmd";

# define VAULT		"/usr/Vault/data/vault"

inherit branch "~Vault/lib/vault_node";

object *nodes;

private void do_spawn(string name, string path);
private string name_to_path(string name);
private string path_to_name(string path);

string query_state_root() { return "Vault:Daemon"; }


static
void create() {
   info("Initializing State Vault...");

   ::create("/usr/Vault/data/vault");

   set_object_name("Vault:Daemon");
}

void patch() {
   set_object_name("Vault:Daemon");
}

void boot(int boot) {
   if (object_name(previous_object()) == "/usr/Vault/initd") {
      /* do nothing */
   }
}

void register_node() {
   if (previous_program() == "/usr/Vault/lib/vault_node") {
      if (!nodes) {
	 nodes = ({ });
      }
      nodes |= ({ previous_object() });
   }
}

object *query_nodes() {
   return nodes[..];
}

/* call this function to load a single object from the vault */
void spawn_one_by_name(string name) {
   do_spawn(name, name_to_path(name));
}

/* call this function to load a single object from the vault */
void spawn_one_by_path(string path) {
   do_spawn(path_to_name(path), path);
}

string store(object ob) {
   string program, name, file, *bits, xml_text;
   mixed state;
   int i;

   name = ob->query_object_name();
   if (!name) {
      error("object is not named");
   }

   state = export_state(ob, nil, nil, nil, TRUE); /* vaultflag TRUE */

   program = object_name(ob);
   sscanf(program, "%s#", program);

   state = xmdElts("object",
		    ({ "program", program }),
		    state);

   /* LV-4.5c refactor: SkotOS used new_object("/data/data") + generate_xml
    * (a chunked-data wrapper). eOS-kernellib's XML daemon exposes
    * gen_xml(xml) returning a string (uses StringBuffer internally per
    * LV-4.5a Decision). write_file accepts a string directly. */
   xml_text = XML->gen_xml(state);

   /* create a safe (for unix paths) representation of the name */
   bits = explode(name, ":");
   for (i = 0; i < sizeof(bits); i++) {
      bits[i] = urlEncode(bits[i]);
   }
   name = implode(bits, "/");

   file = VAULT + "/" + name + ".xml";

   paveWay(file);
   catch {
      write_file(file + ".writing", xml_text);
   } : {
      remove_file(file + ".writing");
      error("failed to write file");
   }
   remove_file(file);
   rename_file(file + ".writing", file);
   sysLog(object_name(this_object()) + ": Wrote " + file + " successfully.");
   return file;
}

/*
 * Store in the vault and also create a snapshot, so that others can use this
 * functionality as well.
 */
void store_snapshot(object ob)
{
   string file;

   if (file = store(ob)) {
      copyFile(file, file + "." + time());
   }
}


/* internals */

private
string name_to_path(string name) {
   string *bits;
   int i;

   bits = explode(name, ":");
   for (i = 0; i < sizeof(bits); i++) {
      bits[i] = urlEncode(bits[i]);
   }
   return VAULT + "/" + implode(bits, "/") + ".xml";
}

private
string path_to_name(string path) {
   string *bits;
   int i;

   if (!path || !sscanf(path, VAULT + "/%s.xml", path)) {
      error("path not in VAULT: " + dumpValue(path));
   }
   bits = explode(path, "/");
   for (i = 0; i < sizeof(bits); i++) {
      bits[i] = urlDecode(bits[i]);
   }
   return implode(bits, ":");
}

private
void do_spawn(string name, string path) {
   string text;

   text = read_file(path);

   if (!text) {
      error("spawn_by_name: no such file: " + dumpValue(path));
   }

   sysLog("do_spawn(" + dumpValue(name) + ", " + dumpValue(path) + ")");

   catch {
      if (spawn_create_one(name, path, text)) {
	 sysLog("VAULT: Cloned: " + dumpValue(name));
      } else {
	 sysLog("VAULT: Loaded: " + dumpValue(name));
      }
   } : {
      sysLog("VAULT: Failed to create: " + dumpValue(name));
   }

   catch {
      if (spawn_configure_one(name, path, text)) {
	 sysLog("VAULT: Configured: " + dumpValue(name));
      }
   } : {
      sysLog("VAULT: Configuration failed: " + dumpValue(name));
   }
}

/*
 * Merry calls:
 */

int
query_method(string method)
{
   switch (lower_case(method)) {
   case "xmlsnapshot":
      return TRUE;
   default:
      return FALSE;
   }
}

mixed
call_method(string method, mapping args)
{
   switch (lower_case(method)) {
   case "xmlsnapshot":
      if (typeof(args["obj"]) != T_OBJECT) {
	 error("xmlsnapshot expects an object $obj");
      }
      store_snapshot(args["obj"]);
      break;
   default:
      break;
   }
}
