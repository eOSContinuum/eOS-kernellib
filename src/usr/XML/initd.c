/*
 * XML subsystem initialization daemon.
 *
 * LV-4.5a source-lifts the XML transport layer (xmd / xmlgen / xmlparse
 * libs, xml_daemon, LWO data wrappers, entities, headers). LV-4.5b enables
 * the compile by lifting Schema -- xml_daemon.c calls
 * DTD->register_type / DTD->register_colour at create() time, and Schema
 * (S) loads before XML (X) in the alphabetical domain-boot order driven
 * by /usr/System/initd.c.
 */

void create()
{
    /* LWO data wrappers must be compiled before xml_daemon's parse /
     * gen paths run, because new_object() requires the master to be
     * present (no auto-compile via the new_object path). Surfaced at
     * LV-4.5c as the "Cannot create new instance of /usr/XML/data/
     * element" load_core_schemas error captured in scope.md OQ-17
     * (CREATE-access gap, secondary). LV-4.5d resolves by compiling
     * the wrappers at boot. */
    compile_object("data/element");
    compile_object("data/pcdata");
    compile_object("data/samref");

    compile_object("sys/xml_daemon");
}
