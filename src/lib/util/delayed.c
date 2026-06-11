/*
 * Delayed-call scheduling for script-bearing objects.
 *
 * Merry's $delay() statement schedules a continuation against its
 * binding host. merrynode.c::do_delay invokes
 * ::call_other(host, "delayed_call", mcontext, "merry_continuation",
 * delay, host), so any object that hosts Merry scripts (the type
 * passed as `this` to run_merry / run_merries) inherits this lib to
 * expose the delayed_call / perform_delayed_call pair.
 *
 * The lib holds no state and defines no create(); inheriting it adds
 * the two functions to the host object, where the call_out resolves
 * against the host's own thread of execution.
 */

static void perform_delayed_call(object ob, string fun, mixed *args)
{
    call_other(ob, fun, args...);
}

void delayed_call(object ob, string fun, mixed delay, mixed args...)
{
    call_out("perform_delayed_call", delay, ob, fun, args);
}
