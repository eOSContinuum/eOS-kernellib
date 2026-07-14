/*
 * API-surface form of the buffered HTTP/1 connection layer, so
 * applications outside this domain can compose it (the /usr/HTTP/lib
 * form is domain-private and cannot be inherited cross-domain).
 *
 * The buffered layer keeps input framing internal: its setMode
 * override localizes Connection1's mode changes and its receiveBytes
 * consumes a raw byte stream, so a client is immune to the
 * driver-level line/raw mode races that hit a response whose head and
 * body arrive in one segment. The TLS variants (TlsServer1,
 * TlsClient1) compose it for the same reason; see
 * examples/composite-app/Inventory/obj/client.c for the plain-client
 * form.
 */

inherit "/usr/HTTP/lib/BufferedConnection1";
