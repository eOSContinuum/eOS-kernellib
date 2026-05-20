/*
 * Vault subsystem initialization daemon.
 *
 * Compiles the Vault subsystem's objects at boot time. LV-3 lifted vault.c
 * (the daemon); LV-4 lifts vault_node.c (the library inherited by vault.c
 * via `inherit branch`). The compile chain enables at LV-4; vault_ops.c
 * (renamed tool/vault.c) lifts in a future task and will be added here
 * when it lands.
 */

void create()
{
    compile_object("sys/vault");
}
