/*
 * Marshal subsystem initialization daemon.
 *
 * Hosts the XmlBinding/lib/stateimpex marshaler. Marshal is the
 * format-pluggable marshaling layer: XmlBinding/ is today's
 * XML-binding implementation; future siblings dCBORBinding/,
 * EnvelopeBinding/, TripleBinding/ slot in at the same level when
 * additional format bindings land.
 *
 * stateimpex.c is a lib (private inherit target) for participating
 * subsystems (Vault/sys/vault, Vault/lib/vault_node, Schema/sys/
 * schema_daemon). Marshal does not host a compile_object call here;
 * the inheriting daemons pull stateimpex in via their own inherit
 * chain at compile time.
 */

void create()
{
}
