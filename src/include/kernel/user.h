# define USERD			"/kernel/sys/userd"
# define LIB_CONN		"/kernel/lib/connection"
# define LIB_USER		"/kernel/lib/user"
# define LIB_ADMIN_CONSOLE		"/kernel/lib/admin_console"
# define TELNET_CONN		"/kernel/obj/telnet"
# define BINARY_CONN		"/kernel/obj/binary"
# define DATAGRAM_CONN		"/kernel/obj/datagram"
# define API_USER		"/kernel/lib/api/user"

# define DEFAULT_USER		"/kernel/obj/user"
# define DEFAULT_ADMIN_CONSOLE	"/kernel/obj/admin_console"
# define DEFAULT_USER_DIR	"/kernel/data"

# define ADMIN_CONSOLE_REGISTRY	"/kernel/sys/admin_console_registry"

# define MODE_DISCONNECT	0
# define MODE_NOECHO		1	/* telnet */
# define MODE_LINE		1	/* binary */
# define MODE_ECHO		2	/* telnet */
# define MODE_EDIT		2	/* binary */
# define MODE_RAW		3	/* binary */
# define MODE_NOCHANGE		4	/* telnet + binary */
# define MODE_UNBLOCK		5	/* unblock to previous mode */
# define MODE_BLOCK		6	/* block input */

# define DEFAULT_TIMEOUT	120	/* two minutes */
# define DISCONNECT_TIMEOUT	2	/* 2 seconds */
