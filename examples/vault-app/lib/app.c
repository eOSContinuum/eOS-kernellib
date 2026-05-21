/*
 * Vault-application library.
 *
 * Thin layer on top of ~Vault/lib/vault_node. Daemons that inherit this
 * lib become a participating Vault node anchored at a domain-owned
 * filesystem root. The lib exists so that an application with multiple
 * domain singletons (a state-root daemon plus, say, a test driver) can
 * share the create-with-root protocol without each singleton restating
 * the Vault registration boilerplate.
 *
 * Inheritors are static masters (one-of-a-kind daemons under sys/),
 * not clonables. They call ::create("/usr/<Domain>/data/<store>") to
 * register; subsequent stores under this root land at
 * <root>/<colons-as-slashes>.xml on disk.
 */

# include <type.h>

inherit "~Vault/lib/vault_node";

static void create(string root)
{
    ::create(root);
}
