# include <type.h>

/*
 * COSE_Key (RFC 9052/9053) public-key extraction for the credential
 * algorithms the platform verifies natively: ES256 (EC2 / P-256) and
 * EdDSA (OKP / Ed25519). Takes a COSE_Key already decoded to a mapping
 * (see /lib/util/cbor) and returns the crypto-kfun scheme name plus
 * the raw public key in the shape the decrypt("<scheme> verify") kfun
 * expects: the uncompressed EC point 04 || X || Y for P-256, the raw
 * 32-byte key for Ed25519. Everything else -- other key types, other
 * curves, a declared algorithm that does not match the key type, or
 * malformed coordinates -- raises an error.
 */

# define COSE_KTY		1
# define COSE_ALG		3
# define COSE_CRV		-1
# define COSE_X			-2
# define COSE_Y			-3

# define COSE_KTY_OKP		1
# define COSE_KTY_EC2		2
# define COSE_ALG_ES256		-7
# define COSE_ALG_EDDSA		-8
# define COSE_CRV_P256		1
# define COSE_CRV_ED25519	6

/*
 * check that a coordinate is a byte string of the expected size
 */
private string coordinate(mixed value, string which)
{
    if (typeof(value) != T_STRING || strlen(value) != 32) {
	error("cose: " + which + " is not a 32-byte coordinate");
    }
    return value;
}

/*
 * map a decoded COSE_Key to ({ scheme, key }): the crypto-kfun scheme
 * prefix ("ECDSA-SECP256R1-SHA256" or "Ed25519") and the raw public
 * key for decrypt("<scheme> verify", key, signature, message)
 */
static string *verifyKey(mapping coseKey)
{
    mixed kty, alg, crv;
    string x, y;

    kty = coseKey[COSE_KTY];
    alg = coseKey[COSE_ALG];
    crv = coseKey[COSE_CRV];

    switch (kty) {
    case COSE_KTY_EC2:
	if (alg != COSE_ALG_ES256) {
	    error("cose: unsupported EC2 algorithm");
	}
	if (crv != COSE_CRV_P256) {
	    error("cose: unsupported EC2 curve");
	}
	x = coordinate(coseKey[COSE_X], "x");
	y = coordinate(coseKey[COSE_Y], "y");
	return ({ "ECDSA-SECP256R1-SHA256", "\x04" + x + y });

    case COSE_KTY_OKP:
	if (alg != COSE_ALG_EDDSA) {
	    error("cose: unsupported OKP algorithm");
	}
	if (crv != COSE_CRV_ED25519) {
	    error("cose: unsupported OKP curve");
	}
	x = coordinate(coseKey[COSE_X], "x");
	return ({ "Ed25519", x });

    default:
	error("cose: unsupported key type");
    }
}
