/*
 * streamd: the composite example's server-sent-events broker.
 *
 * Two topics, one registry of live per-connection WWW server clones:
 *
 *   - audit: mutation-driven push. The inventoryd audit observer's
 *     Merry script calls stream::audit($entry: $new) -- this daemon is
 *     the "stream" script space -- so the notification fires inside
 *     the same atomic write as the mutation and the audit append. The
 *     push itself is a zero-delay call_out per subscriber: an aborted
 *     write rolls the call_out back with everything else, so a stream
 *     never carries an event for a mutation that did not commit.
 *
 *   - agents: poll-diff bridge. The identity substrate has no
 *     notification surface (no roadmap commitment names one), so this
 *     daemon polls authd->query_agents on each subscriber's session at
 *     a short cadence and pushes a snapshot event when the rows
 *     change. The first poll always differs from the empty state, so
 *     a fresh subscriber receives its snapshot within one interval. A
 *     session that stops validating closes the stream. If the
 *     substrate ever grows mutation notifications, this poll loop is
 *     the seam they replace.
 *
 *   - tick: server-pushed heartbeat. A call_out loop pushes a tick
 *     event (counter + server time) to every audit subscriber that
 *     opted in at subscribe time, so a live stream shows the runtime's
 *     async-event machinery working continuously, not only when a
 *     mutation happens to land. Opt-in keeps the headless driver's
 *     stream phases deterministic: a subscriber that did not ask for
 *     ticks never receives one.
 *
 * Subscription entries are gated to this domain (the handler); pushes
 * reach the server clone through its push_event/end_stream entries. A
 * destructed clone (client went away) drops out of the object-keyed
 * mappings on its own -- there is no explicit unsubscribe.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/System/lib/auto";
private inherit json "/lib/util/json";
private inherit "/lib/util/lpc";	/* sysLog */

private mixed *agent_rows(string sessionToken);

# define AUTHD		"/usr/System/sys/authd"
# define MERRY_DAEMON	"/usr/Merry/sys/merry"

# define POLL_INTERVAL	2	/* agent-topic poll cadence, seconds */
# define TICK_INTERVAL	10	/* heartbeat cadence, seconds */

private mapping auditStreams;	/* server clone : 1 */
private mapping agentStreams;	/* server clone : ({ session, last }) */
private mapping tickStreams;	/* server clone : 1 (heartbeat opt-ins) */
private int pollArmed;		/* one poll call_out outstanding */
private int tickArmed;		/* one tick call_out outstanding */
private int tickCount;		/* heartbeats pushed since boot */

static void create()
{
    ::create();
    auditStreams = ([ ]);
    agentStreams = ([ ]);
    tickStreams = ([ ]);
    /* Merry compiles after this domain (alphabetical initd order);
     * defer the script-space registration like the audit observer */
    call_out("register_space", 0);
}

/*
 * The audit observer's script calls stream::audit unconditionally, so
 * an unregistered "stream" space would make every inventory mutation
 * throw inside its atomic write. A registration failure is therefore
 * loud (logged with the reason) and self-healing (retried), never
 * silently swallowed.
 */
static void register_space()
{
    string err;

    err = catch(MERRY_DAEMON->register_script_space("stream",
						    this_object()));
    if (err != nil) {
	sysLog("streamd: stream script-space registration failed (" +
	       err + "); retrying");
	call_out("register_space", 5);
    }
}

/*
 * script-space handler protocol (merrynode.c::Call walks these): the
 * audit observer's script routes stream::audit($entry: $new) here,
 * inside the atomic property write
 */
int query_method(string name)
{
    return name == "audit";
}

mixed call_method(string name, mapping args)
{
    object *servers;
    string data;
    int i;

    if (name != "audit") {
	error("unknown method: " + name);
    }
    servers = map_indices(auditStreams);
    if (sizeof(servers) != 0) {
	data = json::encode(([ "entry" : args["entry"] ]));
	for (i = 0; i < sizeof(servers); i++) {
	    call_out("push_one", 0, servers[i], "audit", data);
	}
    }
    return TRUE;
}

static void push_one(object server, string event, string data)
{
    if (server) {
	catch(server->push_event(event, data));
    }
}

/*
 * subscriptions: same-domain callers only (the HTTP handler)
 */
private void check_domain()
{
    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
}

void subscribe_audit(object server, varargs int heartbeat)
{
    check_domain();
    auditStreams[server] = 1;
    if (heartbeat) {
	tickStreams[server] = 1;
	if (!tickArmed) {
	    tickArmed = TRUE;
	    call_out("push_tick", TICK_INTERVAL);
	}
    }
}

/*
 * one heartbeat: push a tick to every opted-in subscriber, re-arm
 * while any remain (a destructed clone drops out of the object-keyed
 * mapping on its own, like the other topics)
 */
static void push_tick()
{
    object *servers;
    string data;
    int i;

    tickArmed = FALSE;
    servers = map_indices(tickStreams);
    if (sizeof(servers) != 0) {
	tickCount++;
	data = json::encode(([ "n" : tickCount, "t" : time() ]));
	for (i = 0; i < sizeof(servers); i++) {
	    catch(servers[i]->push_event("tick", data));
	}
	tickArmed = TRUE;
	call_out("push_tick", TICK_INTERVAL);
    }
}

void subscribe_agents(object server, string sessionToken)
{
    check_domain();
    agentStreams[server] = ({ sessionToken, nil });
    if (!pollArmed) {
	pollArmed = TRUE;
	call_out("poll_agents", POLL_INTERVAL);
    }
}

/*
 * one poll cycle: refresh every agent subscriber, push on change,
 * close streams whose session no longer validates
 */
static void poll_agents()
{
    object *servers;
    mixed *state, *rows;
    string snapshot, err;
    int i;

    pollArmed = FALSE;
    servers = map_indices(agentStreams);
    for (i = 0; i < sizeof(servers); i++) {
	state = agentStreams[servers[i]];
	err = catch(rows = agent_rows(state[0]));
	if (err != nil) {
	    agentStreams[servers[i]] = nil;
	    catch(servers[i]->push_event("closed",
					 json::encode(([ "error" : err ]))));
	    catch(servers[i]->end_stream());
	    continue;
	}
	snapshot = json::encode(([ "agents" : rows ]));
	if (snapshot != state[1]) {
	    agentStreams[servers[i]] = ({ state[0], snapshot });
	    catch(servers[i]->push_event("agents", snapshot));
	}
    }
    if (map_sizeof(agentStreams) != 0) {
	pollArmed = TRUE;
	call_out("poll_agents", POLL_INTERVAL);
    }
}

/*
 * the subscriber's own-agents view, as JSON-shaped rows; errors out
 * of authd (dead session) propagate to the poll loop
 */
private mixed *agent_rows(string sessionToken)
{
    mixed *rows, *out;
    int i;

    rows = AUTHD->query_agents(sessionToken);
    out = allocate(sizeof(rows));
    for (i = 0; i < sizeof(rows); i++) {
	out[i] = ([ "uuid" : rows[i][0], "suspended" : rows[i][1],
		    "capabilities" : rows[i][2] ]);
    }
    return out;
}
