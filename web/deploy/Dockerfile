# Build morph-fastcgi and bundle with nginx + SPA in a single image.
# Stage 1 — compile against full toolchain.
FROM debian:bookworm-slim AS build

RUN sed -i 's|deb.debian.org|mirrors.huaweicloud.com|g; s|security.debian.org|mirrors.huaweicloud.com/debian-security|g' /etc/apt/sources.list.d/debian.sources 2>/dev/null \
 && sed -i 's|deb.debian.org|mirrors.huaweicloud.com|g; s|security.debian.org|mirrors.huaweicloud.com/debian-security|g' /etc/apt/sources.list 2>/dev/null; \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates \
    libfcgi-dev libsqlite3-dev \
    libcurl4-openssl-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -S . -B build \
    -DBUILD_FASTCGI=ON \
    -DBUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j --target morph-fastcgi

# Stage 2 — runtime image with nginx + fastcgi.
FROM debian:bookworm-slim

RUN sed -i 's|deb.debian.org|mirrors.huaweicloud.com|g; s|security.debian.org|mirrors.huaweicloud.com/debian-security|g' /etc/apt/sources.list.d/debian.sources 2>/dev/null \
 && sed -i 's|deb.debian.org|mirrors.huaweicloud.com|g; s|security.debian.org|mirrors.huaweicloud.com/debian-security|g' /etc/apt/sources.list 2>/dev/null; \
    apt-get update && apt-get install -y --no-install-recommends \
    nginx \
    libfcgi0ldbl libsqlite3-0 libcurl4 libssl3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home /home/morph --shell /usr/sbin/nologin morph \
 && mkdir -p /home/morph/.morph /var/lib/morph /var/lib/morph/log /var/www/morph /var/lib/nginx \
 && chown -R morph:morph /home/morph /var/lib/morph /var/lib/nginx /var/log/nginx

COPY --from=build /src/build/src/fastcgi/morph-fastcgi /usr/local/bin/morph-fastcgi
COPY web/index.html /var/www/morph/index.html
COPY web/deploy/nginx.conf /etc/nginx/sites-available/morph

RUN ln -sf /etc/nginx/sites-available/morph /etc/nginx/sites-enabled/default \
 && rm -f /etc/nginx/sites-enabled/default.bak \
 && rm -f /etc/nginx/sites-enabled/nginx.conf

ENV MORPH_FCGI_LISTEN=:9000
ENV MORPH_FCGI_DB=/var/lib/morph/morph.db
ENV MORPH_FCGI_WORKERS=4
ENV MORPH_FCGI_CONFIG=/home/morph/.morph/config.toml
ENV MORPH_FCGI_OUTPUT_DIR=/var/lib/morph/output
ENV MORPH_FCGI_LOG_FILE=/var/lib/morph/log/fastcgi.log
EXPOSE 80

CMD ["/bin/bash", "-c", \
    "export HOME=/home/morph MORPH_FCGI_CONFIG=/home/morph/.morph/config.toml \
     MORPH_FCGI_OUTPUT_DIR=/var/lib/morph/output \
     MORPH_FCGI_LOG_FILE=/var/lib/morph/log/fastcgi.log \
    && exec su -s /bin/sh morph -c 'exec /usr/local/bin/morph-fastcgi' & \
     nginx -g 'daemon off;'"]
