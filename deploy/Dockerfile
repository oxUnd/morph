# Build morph-fastcgi as a self-contained image.
# Stage 1 — compile against full toolchain.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates \
    libfcgi-dev libsqlite3-dev libcjson-dev \
    libcurl4-openssl-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -S . -B build -DBUILD_FASTCGI=ON -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j

# Stage 2 — runtime image only.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libfcgi0ldbl libsqlite3-0 libcjson1 libcurl4 libssl3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home /var/lib/morph --shell /usr/sbin/nologin morph \
 && mkdir -p /var/lib/morph /run/morph-fastcgi \
 && chown -R morph:morph /var/lib/morph /run/morph-fastcgi

COPY --from=build /src/build/src/fastcgi/morph-fastcgi /usr/local/bin/morph-fastcgi

USER morph
ENV MORPH_FCGI_LISTEN=:9000
ENV MORPH_FCGI_DB=/var/lib/morph/morph.db
ENV MORPH_FCGI_WORKERS=8
EXPOSE 9000

CMD ["/usr/local/bin/morph-fastcgi"]
