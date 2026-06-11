/*
 * XML subsystem initialization daemon.
 *
 * Compiles the XML transport layer (xmd / xmlgen / xmlparse libs,
 * xml_daemon, LWO data wrappers, entities, headers). xml_daemon.c calls
 * DTD->register_type / DTD->register_colour at create() time; Schema
 * (S) loads before XML (X) in the alphabetical domain-boot order driven
 * by /usr/System/initd.c.
 */

void create()
{
    /* LWO data wrappers must be compiled before xml_daemon's parse /
     * gen paths run, because new_object() requires the master to be
     * present (no auto-compile via the new_object path); otherwise
     * load_core_schemas fails with "Cannot create new instance of
     * /usr/XML/data/element". */
    compile_object("data/element");
    compile_object("data/pcdata");
    compile_object("data/samref");

    compile_object("sys/xml_daemon");
}
