# include <type.h>

inherit "~Vault/lib/vault_node";

static void create()
{
    /* /usr/System/initd compiles the domains in alphabetical order, so
     * KV's initd runs before Schema's and Vault's. Registering here
     * would call daemons that do not exist yet. A zero-delay call_out
     * fires after every initd has returned, when both are up. */
    call_out("install", 0);
}

void install()
{
    object node;

    ::create("/usr/KV/data/entries");

    node = clone_object("/usr/Schema/obj/schema_node");
    node->set_name("KV", "Entry");
    node->add_attribute("key", "lpc_str", "query_key");
    node->add_attribute("value", "lpc_str", "query_value");
    node->add_callback("set_key", "key");
    node->add_callback("set_value", "value");
}
