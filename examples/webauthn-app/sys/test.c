/*
 * Boot-time test driver for the WebAuthn codec substrate.
 *
 * Exercises the three pure-function codec layers a WebAuthn relying
 * party needs before any ceremony logic exists:
 *
 *   1. base64url (/lib/util/base64 urlEncode/urlDecode) -- the RFC
 *      4648 section 10 vectors, plus the URL-safe alphabet bytes and
 *      unpadded-input round trips.
 *
 *   2. CBOR decoding (/lib/util/cbor) -- the RFC 8949 Appendix A
 *      vectors for the supported subset (integers, byte and text
 *      strings, arrays, maps, false/true/null), decodePrefix offset
 *      accounting over concatenated items, and a reject battery:
 *      truncation, indefinite lengths, tags, floats, undefined,
 *      malformed initial bytes, duplicate map keys, null map values,
 *      and trailing bytes.
 *
 *   3. COSE_Key extraction (/lib/util/cose) -- an ES256 (EC2/P-256)
 *      key mapping to the uncompressed-point form the ECDSA verify
 *      kfun expects, an EdDSA (OKP/Ed25519) key mapping to the raw
 *      32-byte form, a reject battery for wrong key types, algorithm
 *      and curve mismatches, and malformed coordinates, and a
 *      pipeline test decoding a CBOR-encoded COSE key with trailing
 *      data via decodePrefix, as it appears embedded in
 *      authenticator data.
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/WebAuthn/data/test-result.log (the application-tier logging
 * convention shared by the bundled examples). Phases are wrapped in
 * catch{} so a failure in one does not mask another.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/WebAuthn/lib/app";
private inherit base64 "/lib/util/base64";
private inherit hex "/lib/util/hex";
private inherit cbor "/lib/util/cbor";
private inherit cose "/lib/util/cose";

# define PERSIST_HELPER	"/usr/System/sys/persist_helper"
# define RESULT_FILE	"/usr/WebAuthn/data/test-result.log"

# define X_HEX	"1111111111111111111111111111111111111111111111111111111111111111"
# define Y_HEX	"2222222222222222222222222222222222222222222222222222222222222222"

static void run_tests();
private void log_line(string msg);


static void create()
{
    /* Defer to a call_out so System/initd has finished its load loop
     * before the driver runs. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    /* /usr/WebAuthn/data/ may not exist on first boot. */
    catch(make_dir("/usr/WebAuthn/data"));
    catch(remove_file(RESULT_FILE));
    run_tests();

    /* selfexit: snapshot + clean exit via the System persist helper */
    PERSIST_HELPER->trigger_dump_and_exit();
}


/*
 * TRUE iff decoding the hex-encoded CBOR item raises an error
 */
private int rejects(string hexData)
{
    return !!catch(cbor::decode(hex::decodeString(hexData)));
}

/*
 * decode the hex-encoded CBOR item
 */
private mixed dec(string hexData)
{
    return cbor::decode(hex::decodeString(hexData));
}


private void test_b64url_vectors()
{
    string *plain, *encoded;
    int i;

    /* RFC 4648 section 10 vectors (padding stripped for base64url) */
    plain = ({ "", "f", "fo", "foo", "foob", "fooba", "foobar" });
    encoded = ({ "", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy" });

    for (i = 0; i < sizeof(plain); i++) {
	if (base64::urlEncode(plain[i]) != encoded[i]) {
	    log_line("WebAuthn:test: FAIL: urlEncode(\"" + plain[i] +
		     "\") != \"" + encoded[i] + "\"");
	    return;
	}
	if (base64::urlDecode(encoded[i]) != plain[i]) {
	    log_line("WebAuthn:test: FAIL: urlDecode(\"" + encoded[i] +
		     "\") != \"" + plain[i] + "\"");
	    return;
	}
    }
    log_line("WebAuthn:test: B64URL-VECTORS OK");
}

private void test_b64url_alphabet()
{
    string bytes;

    /* 0xfb 0xef 0xff encodes to "--__" in the URL-safe alphabet
     * (would be "++//" in standard base64) */
    bytes = hex::decodeString("fbefff");
    if (base64::urlEncode(bytes) != "--__") {
	log_line("WebAuthn:test: FAIL: URL-safe alphabet encode");
	return;
    }
    if (base64::urlDecode("--__") != bytes) {
	log_line("WebAuthn:test: FAIL: URL-safe alphabet decode");
	return;
    }
    log_line("WebAuthn:test: B64URL-ALPHABET OK");
}

private void test_cbor_ints()
{
    if (dec("00") != 0 || dec("17") != 23 || dec("1818") != 24 ||
	dec("1903e8") != 1000 || dec("1a000f4240") != 1000000 ||
	dec("20") != -1 || dec("29") != -10 || dec("3863") != -100) {
	log_line("WebAuthn:test: FAIL: CBOR integer vectors");
	return;
    }
    log_line("WebAuthn:test: CBOR-INTS OK");
}

private void test_cbor_strings()
{
    if (dec("40") != "" || dec("4401020304") != hex::decodeString("01020304") ||
	dec("60") != "" || dec("6449455446") != "IETF") {
	log_line("WebAuthn:test: FAIL: CBOR string vectors");
	return;
    }
    log_line("WebAuthn:test: CBOR-STRINGS OK");
}

private void test_cbor_arrays()
{
    mixed value;

    value = dec("80");
    if (typeof(value) != T_ARRAY || sizeof(value) != 0) {
	log_line("WebAuthn:test: FAIL: CBOR empty array");
	return;
    }
    value = dec("83010203");
    if (sizeof(value) != 3 || value[0] != 1 || value[1] != 2 ||
	value[2] != 3) {
	log_line("WebAuthn:test: FAIL: CBOR flat array");
	return;
    }
    value = dec("8301820203820405");
    if (sizeof(value) != 3 || value[0] != 1 ||
	sizeof(value[1]) != 2 || value[1][0] != 2 || value[1][1] != 3 ||
	sizeof(value[2]) != 2 || value[2][0] != 4 || value[2][1] != 5) {
	log_line("WebAuthn:test: FAIL: CBOR nested array");
	return;
    }
    log_line("WebAuthn:test: CBOR-ARRAYS OK");
}

private void test_cbor_maps()
{
    mixed value;

    value = dec("a0");
    if (typeof(value) != T_MAPPING || map_sizeof(value) != 0) {
	log_line("WebAuthn:test: FAIL: CBOR empty map");
	return;
    }
    value = dec("a201020304");
    if (map_sizeof(value) != 2 || value[1] != 2 || value[3] != 4) {
	log_line("WebAuthn:test: FAIL: CBOR int-keyed map");
	return;
    }
    value = dec("a26161016162820203");
    if (map_sizeof(value) != 2 || value["a"] != 1 ||
	sizeof(value["b"]) != 2 || value["b"][0] != 2 ||
	value["b"][1] != 3) {
	log_line("WebAuthn:test: FAIL: CBOR string-keyed map");
	return;
    }
    /* negative integer keys, as COSE_Key uses */
    value = dec("a301022001214401020304");
    if (map_sizeof(value) != 3 || value[1] != 2 || value[-1] != 1 ||
	value[-2] != hex::decodeString("01020304")) {
	log_line("WebAuthn:test: FAIL: CBOR negative-keyed map");
	return;
    }
    log_line("WebAuthn:test: CBOR-MAPS OK");
}

private void test_cbor_simple()
{
    if (dec("f4") != 0 || dec("f5") != 1 || dec("f6") != nil) {
	log_line("WebAuthn:test: FAIL: CBOR simple values");
	return;
    }
    log_line("WebAuthn:test: CBOR-SIMPLE OK");
}

private void test_cbor_prefix()
{
    string data;
    mixed *sub;

    /* two concatenated items: 0x01, then 0x17 */
    data = hex::decodeString("0117");
    sub = cbor::decodePrefix(data, 0);
    if (sub[0] != 1 || sub[1] != 1) {
	log_line("WebAuthn:test: FAIL: decodePrefix first item");
	return;
    }
    sub = cbor::decodePrefix(data, sub[1]);
    if (sub[0] != 23 || sub[1] != 2) {
	log_line("WebAuthn:test: FAIL: decodePrefix second item");
	return;
    }
    log_line("WebAuthn:test: CBOR-PREFIX OK");
}

private void test_cbor_rejects()
{
    string *bad;
    int i;

    bad = ({
	"1903",			/* truncated 2-byte integer */
	"44010203",		/* byte string shorter than declared */
	"5f4101ff",		/* indefinite-length byte string */
	"c000",			/* tag */
	"f93c00",		/* half-precision float */
	"f7",			/* undefined */
	"1c",			/* malformed initial byte (info 28) */
	"a201020103",		/* duplicate map key */
	"a101f6",		/* null map value */
	"8101f4"		/* array declaring 1 item, trailing byte */
    });
    for (i = 0; i < sizeof(bad); i++) {
	if (!rejects(bad[i])) {
	    log_line("WebAuthn:test: FAIL: CBOR accepted malformed input " +
		     bad[i]);
	    return;
	}
    }
    log_line("WebAuthn:test: CBOR-REJECTS OK");
}

private void test_cose_es256()
{
    string *result, x, y;

    x = hex::decodeString(X_HEX);
    y = hex::decodeString(Y_HEX);
    result = cose::verifyKey(([ 1 : 2, 3 : -7, -1 : 1, -2 : x, -3 : y ]));
    if (result[0] != "ECDSA-SECP256R1-SHA256") {
	log_line("WebAuthn:test: FAIL: ES256 scheme name");
	return;
    }
    if (strlen(result[1]) != 65 || result[1][0] != 0x04 ||
	result[1][1 .. 32] != x || result[1][33 .. 64] != y) {
	log_line("WebAuthn:test: FAIL: ES256 uncompressed point");
	return;
    }
    log_line("WebAuthn:test: COSE-ES256 OK");
}

private void test_cose_ed25519()
{
    string *result, x;

    x = hex::decodeString(X_HEX);
    result = cose::verifyKey(([ 1 : 1, 3 : -8, -1 : 6, -2 : x ]));
    if (result[0] != "Ed25519" || result[1] != x) {
	log_line("WebAuthn:test: FAIL: Ed25519 raw key");
	return;
    }
    log_line("WebAuthn:test: COSE-ED25519 OK");
}

private void test_cose_rejects()
{
    mapping *bad;
    string x, y;
    int i;

    x = hex::decodeString(X_HEX);
    y = hex::decodeString(Y_HEX);
    bad = ({
	([ 1 : 3, 3 : -7, -1 : 1, -2 : x, -3 : y ]),	/* wrong kty */
	([ 1 : 2, 3 : -8, -1 : 1, -2 : x, -3 : y ]),	/* EC2 with EdDSA alg */
	([ 1 : 2, 3 : -7, -1 : 2, -2 : x, -3 : y ]),	/* wrong curve */
	([ 1 : 2, 3 : -7, -1 : 1, -2 : x[.. 30], -3 : y ]),  /* short x */
	([ 1 : 2, 3 : -7, -1 : 1, -2 : x ])		/* missing y */
    });
    for (i = 0; i < sizeof(bad); i++) {
	if (!catch(cose::verifyKey(bad[i]))) {
	    log_line("WebAuthn:test: FAIL: COSE accepted malformed key " +
		     (string) i);
	    return;
	}
    }
    log_line("WebAuthn:test: COSE-REJECTS OK");
}

private void test_cose_pipeline()
{
    string data, *result;
    mixed *sub;

    /* a CBOR-encoded ES256 COSE_Key in CTAP2 canonical order, followed
     * by a trailing item, as a key appears embedded mid-stream in
     * authenticator data */
    data = hex::decodeString("a5010203262001215820" + X_HEX +
			     "225820" + Y_HEX + "f5");
    sub = cbor::decodePrefix(data, 0);
    if (typeof(sub[0]) != T_MAPPING || sub[1] != strlen(data) - 1) {
	log_line("WebAuthn:test: FAIL: pipeline COSE key offset");
	return;
    }
    result = cose::verifyKey(sub[0]);
    if (result[0] != "ECDSA-SECP256R1-SHA256" || strlen(result[1]) != 65) {
	log_line("WebAuthn:test: FAIL: pipeline verify key");
	return;
    }
    sub = cbor::decodePrefix(data, sub[1]);
    if (sub[0] != 1 || sub[1] != strlen(data)) {
	log_line("WebAuthn:test: FAIL: pipeline trailing item");
	return;
    }
    log_line("WebAuthn:test: COSE-PIPELINE OK");
}


static void run_tests()
{
    log_line("WebAuthn:test: starting");

    catch { test_b64url_vectors(); } : { log_line("WebAuthn:test: FAIL: b64url vectors threw"); }
    catch { test_b64url_alphabet(); } : { log_line("WebAuthn:test: FAIL: b64url alphabet threw"); }
    catch { test_cbor_ints(); } : { log_line("WebAuthn:test: FAIL: cbor ints threw"); }
    catch { test_cbor_strings(); } : { log_line("WebAuthn:test: FAIL: cbor strings threw"); }
    catch { test_cbor_arrays(); } : { log_line("WebAuthn:test: FAIL: cbor arrays threw"); }
    catch { test_cbor_maps(); } : { log_line("WebAuthn:test: FAIL: cbor maps threw"); }
    catch { test_cbor_simple(); } : { log_line("WebAuthn:test: FAIL: cbor simple threw"); }
    catch { test_cbor_prefix(); } : { log_line("WebAuthn:test: FAIL: cbor prefix threw"); }
    catch { test_cbor_rejects(); } : { log_line("WebAuthn:test: FAIL: cbor rejects threw"); }
    catch { test_cose_es256(); } : { log_line("WebAuthn:test: FAIL: cose es256 threw"); }
    catch { test_cose_ed25519(); } : { log_line("WebAuthn:test: FAIL: cose ed25519 threw"); }
    catch { test_cose_rejects(); } : { log_line("WebAuthn:test: FAIL: cose rejects threw"); }
    catch { test_cose_pipeline(); } : { log_line("WebAuthn:test: FAIL: cose pipeline threw"); }
}


private void log_line(string msg)
{
    mixed *info;
    int size;

    catch {
	info = file_info(RESULT_FILE);
	size = info ? info[0] : 0;
	write_file(RESULT_FILE, msg + "\n", size);
    }
}
