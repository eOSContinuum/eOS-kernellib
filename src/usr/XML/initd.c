/*
 * XML subsystem initialization daemon.
 *
 * LV-4.5a source-lifts the XML transport layer (xmd / xmlgen / xmlparse
 * libs, xml_daemon, LWO data wrappers, entities, headers). compile_object
 * of "sys/xml_daemon" is deferred until LV-4.5b lifts the Schema subsystem
 * — xmlgen.c and xmlparse.c inherit /usr/Schema/lib/dtd, and xml_daemon.c
 * calls DTD->register_type / DTD->register_colour at create() time, both
 * unresolved until LV-4.5b lands. Same LV-3 -> LV-4 deferral pattern.
 */

void create()
{
    /* compile_object("sys/xml_daemon"); -- deferred until LV-4.5b */
}
