/*
 * Continuation context for Merry $delay() statements.
 *
 * Carries (mode, signal, label, args) across a delayed call and
 * resumes the script via run_merries() when the call fires.
 *
 * Lifted from SkotOS /usr/SkotOS/data/mcontext.c per LM-2.
 * Conventions adjusted to eos-kernellib:
 *   configure -> create (cloud-server _F_init dispatch per L14 #10)
 *   inherit "/lib/string"               -> inherit "/lib/util/ascii"
 *   inherit "/usr/SkotOS/lib/merryapi"  -> inherit "/usr/Merry/lib/merryapi"
 *   SYS_MERRY->update_resource          -> MERRY->update_resource
 *   name(args[0])                       -> inlined via /lib/util/lpc name()
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

atomic
static void create(string m, string s, string l, mapping a) {
   mode = lower_case(m);
   signal = lower_case(s);
   label = l;
   args = a;
}

void
merry_continuation(object this) {
   int ticks;

   ticks = status()[ST_TICKS];
   run_merries(this, signal, mode, args, label);
}

string describe_delayed_call(string fun, mixed *args, mixed delay) {
   return "Merry $delay(" + delay + ", " + label + ") on " +
      name(args[0]) + "/" + mode + ":" + signal;
}
