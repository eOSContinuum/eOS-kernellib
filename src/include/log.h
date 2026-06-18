/*
 * Logging facility: object path and severity levels for the logd daemon.
 *
 * logd (/usr/System/sys/logd) is the diagnostic sink for the platform's
 * sysLog / info / debugLog surface (wired in /lib/util/lpc.c) and the
 * persistent home for errord's error reports. Levels are ordered ascending
 * by severity so a single threshold comparison gates emission.
 */

# define LOGD		"/usr/System/sys/logd"

# define LOG_DEBUG	1	/* debugLog: developer tracing */
# define LOG_INFO	2	/* info: routine progress */
# define LOG_NOTICE	3	/* sysLog: general system events */
# define LOG_ERROR	4	/* errord reports: failures */
