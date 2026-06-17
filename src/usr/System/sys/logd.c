/*
 * logd: the platform diagnostic log daemon.
 *
 * The single owner of the diagnostic sink, the emission threshold, and the
 * operator surface. The sysLog / info / debugLog calls in /lib/util/lpc.c
 * forward here (by call_other, so every tier reaches the daemon without the
 * /kernel/lib inheritance restriction that constrains the capability lib);
 * each maps to a fixed level (debugLog=DEBUG, info=INFO, sysLog=NOTICE) and
 * is dropped below the runtime-settable threshold (default INFO). errord
 * drains its error reports here at ERROR level via log_report.
 *
 * Deferred write. DGD forbids write_file inside an atomic function, and
 * logging happens from atomic contexts (Schema import, dispatch, ...). So a
 * log call only buffers the line in memory and schedules a single coalesced
 * call_out(0); the flush runs in a fresh, non-atomic execution where the
 * file write is legal. A line logged inside a committed atomic is written
 * after it commits; one inside a rolled-back atomic rolls back with the
 * buffer (its work did not happen). This also makes logging non-throwing:
 * no in-atomic write_file means no error to feed back into errord (whose
 * report would otherwise re-enter logging and storm). NOTICE-and-above also
 * echo to the operator console from the flush. Rotation/retention are
 * external tooling's job -- the daemon appends and never prunes.
 *
 * Threshold mutation is KERNEL-gated: the operator reaches it through
 * ADMIN_CONSOLE_REGISTRY's verb_set_log_threshold elevation helper (gated
 * by the admin_console.caller capability), never directly -- logd is the
 * first /usr-tier consumer of the capability library.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <log.h>

# define SYSTEMAUTO	"/usr/System/lib/auto"
# define LOG_DIR	"/usr/System/log"
# define LOG_FILE	"/usr/System/log/system.log"
# define TAIL_DEFAULT	40
# define TAIL_BYTES	8192

inherit auto AUTO;
inherit SYSTEMAUTO;
private inherit "/lib/util/ascii";	/* lower_case */

private int threshold;		/* messages below this level are dropped */
private mixed **pending;	/* ({ ({ level, msg }), ... }) awaiting flush */
private int flush_scheduled;	/* a call_out("_flush") is outstanding */
private int in_flush;		/* suppress re-entrant logging during a flush */

/*
 * NAME:	create()
 * DESCRIPTION:	default threshold, empty buffer, ensure the log directory
 */
static void create()
{
   threshold = LOG_INFO;
   pending = ({ });
   flush_scheduled = 0;
   in_flush = 0;
   catch(make_dir(LOG_DIR));
}

/*
 * NAME:	level_name()
 * DESCRIPTION:	printable tag for a severity level
 */
private string level_name(int level)
{
   switch (level) {
   case LOG_DEBUG:  return "DEBUG";
   case LOG_INFO:   return "INFO";
   case LOG_NOTICE: return "NOTICE";
   case LOG_ERROR:  return "ERROR";
   default:         return "LOG";
   }
}

/*
 * NAME:	enqueue()
 * DESCRIPTION:	buffer a line and schedule the deferred flush. Coalesced --
 *		at most one call_out is outstanding, so a burst of logs never
 *		hits the callout roof. The call_out schedule is catch-wrapped
 *		(it may be refused inside an atomic); the line still accrues
 *		in the buffer and the next schedulable log drains it.
 */
private void enqueue(int level, string msg)
{
   pending += ({ ({ level, msg }) });
   if (!flush_scheduled && catch(call_out("_flush", 0)) == nil) {
      flush_scheduled = 1;
   }
}

/*
 * NAME:	log()
 * DESCRIPTION:	public diagnostic entry, called by the sysLog/info/debugLog
 *		forwarders from any tier. Drops messages below the threshold;
 *		buffers the rest for the deferred flush.
 */
void log(int level, string msg)
{
   if (in_flush || !msg || level < threshold) {
      return;
   }
   enqueue(level, msg);
}

/*
 * NAME:	log_report()
 * DESCRIPTION:	buffer a pre-formatted error report at ERROR level. Called by
 *		errord so error reporting becomes durable; errord has already
 *		echoed it to the console. Bypasses the threshold (errors are
 *		never suppressed) but not the in_flush guard (which breaks any
 *		feedback from a flush-time write failure). Restricted to the
 *		trusted System/kernel error path.
 */
void log_report(string report)
{
   if (in_flush || (!SYSTEM() && !KERNEL())) {
      return;
   }
   if (report) {
      enqueue(LOG_ERROR, report);
   }
}

/*
 * NAME:	_flush()
 * DESCRIPTION:	call_out target: drain the buffer to the file in one append
 *		and echo NOTICE-and-above to the console. Runs non-atomic, so
 *		write_file is legal. The in_flush guard drops any logging that
 *		this flush's own side-effects provoke, so a write failure can
 *		never feed back into another flush.
 */
void _flush()
{
   mixed **batch;
   string stamp, blob;
   int i, sz, size;
   mixed *info;

   flush_scheduled = 0;
   batch = pending;
   pending = ({ });
   sz = sizeof(batch);
   if (sz == 0) {
      return;
   }

   in_flush = 1;
   stamp = ctime(time())[4 .. 18];
   blob = "";
   for (i = 0; i < sz; i++) {
      blob += stamp + " " + level_name(batch[i][0]) + " " + batch[i][1] + "\n";
   }
   catch {
      info = file_info(LOG_FILE);
      size = info ? info[0] : 0;
      write_file(LOG_FILE, blob, size);
   }
   for (i = 0; i < sz; i++) {
      if (batch[i][0] >= LOG_NOTICE) {
         catch(DRIVER->message(level_name(batch[i][0]) + ": " + batch[i][1]));
      }
   }
   in_flush = 0;
}

/*
 * NAME:	set_threshold()
 * DESCRIPTION:	raise or lower the emission threshold. KERNEL-gated -- the
 *		operator path reaches it through the registry's
 *		verb_set_log_threshold helper, never directly.
 */
void set_threshold(int level)
{
   if (!KERNEL()) {
      error("logd: set_threshold not callable from outside /kernel");
   }
   if (level < LOG_DEBUG || level > LOG_ERROR) {
      error("logd: invalid level");
   }
   threshold = level;
}

/*
 * NAME:	query_threshold()
 * DESCRIPTION:	current emission threshold (public read)
 */
int query_threshold()
{
   return threshold;
}

/*
 * _emit: route operator-verb output through the user object the dispatcher
 * passes in (admin_console's private message() is not reachable here).
 */
private void _emit(object user, string msg)
{
   if (user) {
      user->message(msg);
   }
}

/*
 * NAME:	level_from_name()
 * DESCRIPTION:	parse a level name to its constant, or 0 if unrecognized
 */
private int level_from_name(string name)
{
   switch (lower_case(name)) {
   case "debug":  return LOG_DEBUG;
   case "info":   return LOG_INFO;
   case "notice": return LOG_NOTICE;
   case "error":  return LOG_ERROR;
   default:       return 0;
   }
}

/*
 * cmd_log [N]
 *
 * Tail the last N lines of the persistent log (default TAIL_DEFAULT). The
 * read is bounded to the final TAIL_BYTES so a large unrotated log does not
 * load wholesale. Read-only -- rides the admin console's existing privilege.
 */
void cmd_log(object user, string cmd, string str)
{
   string *parts, *lines, content;
   mixed *info;
   int n, size, start, sz, i;

   n = TAIL_DEFAULT;
   if (str) {
      parts = explode(str, " ") - ({ "" });
      if (sizeof(parts) >= 1 && sscanf(parts[0], "%d", n) != 1) {
         _emit(user, "usage: log [lines]\n");
         return;
      }
   }
   if (n <= 0) {
      n = TAIL_DEFAULT;
   }

   info = file_info(LOG_FILE);
   size = info ? info[0] : 0;
   if (size == 0) {
      _emit(user, "log: (empty)\n");
      return;
   }
   start = (size > TAIL_BYTES) ? size - TAIL_BYTES : 0;
   content = read_file(LOG_FILE, start, size - start);
   if (!content) {
      _emit(user, "log: (empty)\n");
      return;
   }
   lines = explode(content, "\n") - ({ "" });
   if (start > 0 && sizeof(lines) > 0) {
      lines = lines[1 ..];	/* drop the partial leading line */
   }
   sz = sizeof(lines);
   i = (sz > n) ? sz - n : 0;
   for (; i < sz; i++) {
      _emit(user, lines[i] + "\n");
   }
}

/*
 * cmd_log-level [LEVEL]
 *
 * No arg: report the current threshold. With debug|info|notice|error: set
 * it through the registry's KERNEL-elevated, capability-gated helper.
 */
void cmd_log_level(object user, string cmd, string str)
{
   string *parts;
   int level;
   mixed err;

   parts = str ? explode(str, " ") - ({ "" }) : ({ });
   if (sizeof(parts) == 0) {
      _emit(user, "log-level: " + level_name(threshold) + "\n");
      return;
   }
   level = level_from_name(parts[0]);
   if (level == 0) {
      _emit(user, "log-level: argument must be debug|info|notice|error\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->verb_set_log_threshold(level));
   if (err) {
      _emit(user, "log-level: " + err + "\n");
      return;
   }
   _emit(user, "log-level set to " + level_name(level) + "\n");
}
