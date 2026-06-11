/*
 * Schema subsystem initialization daemon.
 *
 * Compiles the Schema subsystem (DTD lib + dtd_daemon + schema_node
 * lib+obj + schema_daemon + 5 core schema XML files in data/schema/).
 *
 * Boot order is governed by /usr/System/initd.c::create(), which iterates
 * /usr/[A-Z]* domains alphabetically after the TLS/HTTP/LPC prefix.
 * Schema (S) loads before XML (X), so Schema's initd brings up the
 * dtd_daemon first; XML's initd then compiles xml_daemon, whose create()
 * registers the XML types and colours against the dtd_daemon already in
 * place. schema_daemon's configure_initial_nodes() references XML_BOOL
 * as a constant string ("xml_bool"); the type-handler lookup happens
 * lazily at marshaling time when both daemons are loaded.
 *
 * The XML files in data/schema/ carry the on-disk format reference for
 * the lifted Ur:Hierarchy / UrChild / UrChildren / Core:Entry / Entries
 * primitives. XML-driven load runs through the marshaler;
 * at this lift the primitives are code-defined in
 * schema_daemon::configure_initial_nodes().
 */

void create()
{
    compile_object("sys/dtd_daemon");
    compile_object("sys/schema_daemon");
}
