/*
 * Vault subsystem initialization daemon.
 *
 * Compiles the Vault subsystem's objects at boot time: vault.c (the
 * daemon) and, via its `inherit branch` chain, vault_node.c (the
 * library participating domains inherit).
 */

void create()
{
    compile_object("sys/vault");
}
