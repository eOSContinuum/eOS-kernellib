/*
 * Port-label registry: object path for the portd daemon.
 *
 * portd (/usr/System/sys/portd) is the label layer over the kernel's
 * numeric port-to-manager registration: System-tier boot code declares a
 * label ("admin", "http") for a configured port slot and connection
 * managers register by label, naming the port by role rather than by
 * position in the .dgd port list.
 */

# define PORTD		"/usr/System/sys/portd"
