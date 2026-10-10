# Build for the host architecture: Apple Silicon uses Linux ARM64, not AVX2 emulation.
FROM ubuntu:24.04 AS build
ARG BUILD_JOBS=2
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build git g++ libssl-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt LICENSE THIRD_PARTY_NOTICES.txt ./
COPY cmake/ cmake/
COPY patches/ patches/
COPY src/ src/
COPY include/ include/
COPY examples/ examples/
COPY tests/ tests/
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DNATIVE_ARCH=OFF -DBUILD_TESTING=ON \
    && cmake --build /build --parallel "${BUILD_JOBS}" \
    && cmake --install /build --prefix /opt/duckonwire \
    && cmake --install /build --prefix /opt/duckonwire --component Tests
# Documentation updates preserve the cached native engine build.
COPY README.md AGENTS.md ./

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libcurl4t64 libssl3t64 openssl postgresql-client util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 duckonwire \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin duckonwire \
    && install -d -m 0700 -o 10001 -g 10001 /var/lib/duckonwire \
    && install -d /opt/duckonwire/lib
COPY --from=build /opt/duckonwire/bin/ /opt/duckonwire/bin/
COPY --from=build /opt/duckonwire/lib/libduckdb.so /opt/duckonwire/lib/libduckdb.so
COPY --from=build /opt/duckonwire/examples/ /opt/duckonwire/examples/
COPY --from=build /opt/duckonwire/share/duckonwire/ /opt/duckonwire/share/duckonwire/
COPY --from=build /source/README.md /source/AGENTS.md /opt/duckonwire/
COPY --chmod=0755 docker/entrypoint.sh /usr/local/bin/duckonwire-entrypoint
ENV PGHOST=localhost PGPORT=9496 PGDATABASE=duckonwire PGUSER=writer \
    PGSSLMODE=verify-full PGSSLROOTCERT=/var/lib/duckonwire/.runtime/tls/ca.pem \
    PGPASSFILE=/var/lib/duckonwire/.runtime/pgpass
USER 10001:10001
WORKDIR /var/lib/duckonwire
EXPOSE 9496
HEALTHCHECK --interval=10s --timeout=5s --start-period=20s --retries=6 \
    CMD pg_isready -q
ENTRYPOINT ["/usr/local/bin/duckonwire-entrypoint"]
