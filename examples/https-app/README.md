# Reference HTTPS application

A minimal HTTPS application that runs on top of eOS-kernellib: the first application binding of the shipped HTTP/1 TLS server (`/usr/HTTP/api/lib/TlsServer1.c`). The platform terminates TLS 1.3 natively -- no reverse proxy in front. Routing and platform contracts mirror `examples/http-app`; the walkthrough for those contracts lives in `docs/http-applications.md`.

## Routes

- `GET /health` -- returns `200 OK` with body `ok\n`.
- `POST /echo` -- returns `200 OK` echoing the request body.
- any other path -- returns `404 Not Found`.

## Requirements

Native TLS needs three things beyond the plain-HTTP setup:

1. **The crypto extension module.** The TLS stack's body is gated on the host driver being built with `KF_SECURE_RANDOM`; build the crypto module with `make crypto` in [`dworkin/lpc-ext`](https://github.com/dworkin/lpc-ext) and load it via a `modules` line in the `.dgd` configuration (`docs/operations.md`, Host-driver extensions). The checked-in `example.dgd` stays module-less; without the module the HTTPS bootstrap logs that it is standing down and the rest of the platform boots normally.
2. **A second binary port.** The HTTPS bootstrap serves the `https` port label, which the port-label registry declares for binary port index 1 when one is configured: `binary_port = ([ "*" : 8080, "*" : 8443 ]);`.
3. **A certificate.** PEM certificate and key at `/usr/System/data/tls/cert.pem` and `key.pem` (paths as the platform sees them, i.e. under the configured `directory`). Acquisition and renewal belong to the host -- an ACME client such as certbot writing those paths. For a local run, a self-signed P-256 certificate works:

   ```sh
   mkdir -p src/usr/System/data/tls
   openssl ecparam -name prime256v1 -genkey -noout -out src/usr/System/data/tls/key.pem
   openssl req -x509 -key src/usr/System/data/tls/key.pem \
       -out src/usr/System/data/tls/cert.pem -days 30 -subj "/CN=localhost"
   ```

## Deployment

The platform's HTTPS bootstrap (`src/usr/System/sys/https_server.c`) clones the application at the kernel-defined path `/usr/WWW/obj/tls_server` -- the HTTPS analog of the plain-HTTP `/usr/WWW/obj/server` convention. To deploy:

```sh
cp -R examples/https-app src/usr/WWW
```

Then start the driver against the module-loading configuration. The `/usr/WWW/` domain is picked up automatically at **cold boot** (a restore from snapshot skips the domain iteration -- boot cold after deploying).

A domain may mount both servers (`obj/server.c` for HTTP and `obj/tls_server.c` for HTTPS) side by side; this example ships only the TLS one.

## Verify

```sh
curl -k https://localhost:8443/health
# ok

curl -k -d 'hello' https://localhost:8443/echo
# hello

curl -ki https://localhost:8443/no-such-route
# HTTP/1.1 404 Not Found
```

The server speaks TLS 1.3 only, so the client must too. The stock macOS curl (SecureTransport backend) cannot; `openssl s_client` works everywhere:

```sh
{ printf 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'; sleep 3; } | \
    openssl s_client -connect localhost:8443 -tls1_3 -quiet -no_ign_eof
# HTTP/1.1 200 OK
# ...
# ok
```

`scripts/https-smoke.sh` is the executable form of this recipe: it deploys the example, generates a throwaway certificate, boots with the module loaded and the second port configured, and drives these probes plus a TLS-1.3 negotiation check and a cleartext-refusal check.

## Files

- `initd.c` -- domain initd; compiles `obj/tls_server` at boot.
- `obj/tls_server.c` -- per-connection HTTPS server (clonable). The platform clones one per incoming connection; certificate and key are pulled from the HTTPS bootstrap at clone time and live only for the connection.
