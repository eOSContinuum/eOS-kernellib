# Reference HTTP application

A minimal HTTP/1 application that runs on top of eOS-kernellib. Demonstrates the platform contracts an HTTP/1 application server honors; the full walkthrough lives in `docs/http-applications.md`.

The code in this directory is a stripped-down derivative of a verified working HTTP/1 application; the inheritance pattern, the `::create` clone-arg wiring, and the binary-manager glue are preserved verbatim from the verified source, with simplified inline routing replacing a registry pattern.

## Routes

- `GET /health` -- returns `200 OK` with body `ok\n`.
- `GET /status` -- returns `200 OK` with the `status()` health vector's capacity-headroom counts, one `key=used/cap` line each (`objects`, `callouts`, `swap-sectors`, `users`, plus `uptime` in seconds) -- the machine-readable form of `docs/operations.md` Monitoring signals.
- `POST /echo` -- returns `200 OK` echoing the request body.
- any other path -- returns `404 Not Found`.

## Deployment

The platform's HTTP/1 bootstrap (`src/usr/System/sys/http_server.c`) clones the application at the kernel-defined path `/usr/WWW/obj/server`. To deploy this application, copy the directory into the kernel layer's `src/usr/`:

```sh
cp -R examples/http-app src/usr/WWW
```

Then start the driver against `example.dgd`. The new `/usr/WWW/` user-layer domain is picked up automatically by the System initd's `/usr/[A-Z]*/initd.c` iteration.

That iteration runs only at **cold boot**: a start that restores from a snapshot skips it, so a domain copied in while the platform was down is not compiled on restore. Boot cold (no snapshot argument) after deploying a new domain.

## Verify

```sh
curl http://localhost:8080/health
# ok

curl http://localhost:8080/status
# objects=234/10000
# callouts=8/10000
# swap-sectors=567/65535
# users=1/255
# uptime=4

curl -d 'hello' http://localhost:8080/echo
# hello

curl -i http://localhost:8080/no-such-route
# HTTP/1.1 404 Not Found
# ...
# 404 Not Found
```

## Files

- `initd.c` -- domain initd; compiles `obj/server` at boot.
- `obj/server.c` -- per-connection HTTP/1 server (clonable). The platform clones one per incoming connection.
