/*
 * Continuation context for Merry $delay() statements.
 *
 * Carries (mode, signal, label, args) across a delayed call and
 * resumes the script via run_merries() when the call fires.
 */

# include <status.h>

# define MERRY		"/usr/Merry/sys/merry"

private inherit "/lib/util/ascii";
private inherit "/usr/Merry/lib/merryapi";
private inherit "/lib/util/lpc";

mapping args;

string mode;
string signal;
string label;
object code;        /* the compiled merry object that scheduled the delay,
                     * when known; nil for legacy convention-only delays */

atomic
static void create(string m, string s, string l, mapping a, varargs object c) {
   mode = lower_case(m);
   signal = lower_case(s);
   label = l;
   args = a;
   code = c;
}

void
merry_continuation(object this) {
   int ticks;

   ticks = status()[ST_TICKS];
   /* Resume the captured compiled object directly when present -- this is
    * the path dispatcher-fired observers take, whose merry:on:<path>:<timing>
    * storage run_merries cannot relocate by the find_merry convention.
    * Fall back to the convention lookup for legacy delays that did not
    * carry a compiled-object reference. */
   if (code) {
      code->evaluate(this, signal, mode, args, label);
   } else {
      run_merries(this, signal, mode, args, label);
   }
}

string describe_delayed_call(string fun, mixed *args, mixed delay) {
   return "Merry $delay(" + delay + ", " + label + ") on " +
      name(args[0]) + "/" + mode + ":" + signal;
}
