/* SPDX-License-Identifier: BSD-2-Clause-Patent */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include "HttpConnection.h"

inherit "/usr/System/lib/auto";

object userd;	/* kernel user daemon */

/*
 * register as binary-port manager on the first binary port
 */
static void create()
{
    ::create();
    userd = find_object(USERD);
    userd->set_binary_manager(0, this_object());
}

/*
 * select a per-connection HTTP/1 server for an incoming connection
 */
object select(string str)
{
    if (previous_object() == userd) {
	return clone_object(HTTP1_SERVER);
    }
}

/*
 * return the connection mode for a freshly selected server
 */
int query_mode(object obj)
{
    return MODE_RAW;
}

/*
 * return the connection-login timeout
 */
int query_timeout(object obj)
{
    return DEFAULT_TIMEOUT;
}
