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
    compile_object("sys/xml_daemon");
}
