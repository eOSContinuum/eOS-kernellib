/*
 * Master object for Merry's admin_console extension. Inherits the
 * library at LIB_MERRY_ADMIN_CONSOLE_EXT; ADMIN_CONSOLE_REGISTRY's
 * dispatch_table stores this path (OBJ_MERRY_ADMIN_CONSOLE_EXT), and
 * admin_console::process() find_object's it at unknown-verb dispatch
 * time. /usr/Merry/initd compile_object's this file at boot so the
 * master is resident before first verb fires.
 */

# include <Merry.h>

inherit LIB_MERRY_ADMIN_CONSOLE_EXT;
