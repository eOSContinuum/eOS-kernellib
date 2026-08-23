# Build the pinned DGD driver and run the example regressions headlessly,
# with no toolchain on the host. Build context is the repository root:
#
#     docker build -t eos-kernellib .
#     docker run --rm eos-kernellib                 # merry-app to its PASS sentinel
#     docker run --rm eos-kernellib chat-app        # any scripts/run-example.sh example
#
# The image carries the driver and a copy of this checkout; each run starts
# from the clean slate scripts/run-example.sh enforces. Nothing persists
# across runs unless you mount state/ yourself.

FROM debian:bookworm-slim

# The kernel layer requires preprocess_file(): DGD master at or after
# b4da6a96 (docs/building.md). Full hash so the shallow fetch can pin it.
ARG DGD_COMMIT=b4da6a965dca0ff40c0912c5ab4a04e56d47fa4b

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git make gcc g++ bison procps \
    && rm -rf /var/lib/apt/lists/*

RUN git init /opt/dgd \
    && git -C /opt/dgd remote add origin https://github.com/dworkin/dgd.git \
    && git -C /opt/dgd fetch --depth 1 origin "$DGD_COMMIT" \
    && git -C /opt/dgd checkout FETCH_HEAD \
    && make -C /opt/dgd/src install

WORKDIR /kernellib
COPY . .

ENV DGD_BIN=/opt/dgd/bin/dgd
ENTRYPOINT ["scripts/run-example.sh"]
CMD ["merry-app"]
