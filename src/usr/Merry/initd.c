/*
 * Merry subsystem initialization daemon.
 *
 * LM-3 source-lifts the Merry sublanguage runtime (merry daemon,
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
}
