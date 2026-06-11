/*
 * Merry subsystem initialization daemon.
 *
 * Compiles the Merry sublanguage runtime (merry daemon,
 * invocation API, per-script clonable + base class, yacc grammar
 * + continuation context). Compile-order: data clonables compile
 * first so new_object() can find their masters, then the daemon
 * compiles (its create() initializes the parser via /lib/util/fileparse
 * and pre-compiles the data wrappers).
 */

void create()
{
    /* data clonables -- masters must be compiled before sys/merry::create
     * calls compile_object("/usr/Merry/data/merry") + .../mcontext,
     * because new_object() on a data clonable requires its master to
     * be present (no auto-compile via the new_object path). */
    compile_object("data/merry");
    compile_object("data/mcontext");

    compile_object("sys/merry");

    /* The admin_console extension. The library carries the verb
     * implementations; the obj master inheriting it is what
     * /kernel/sys/admin_console_registry stores by path and what
     * /kernel/obj/admin_console::process() find_object's at unknown-
     * verb dispatch time. The master must be resident before the
     * first dispatch routes here. */
    compile_object("obj/admin_console_ext");
}
