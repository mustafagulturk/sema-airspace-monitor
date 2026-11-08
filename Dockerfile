# Semâ: multi-stage build. The runtime image holds only the two binaries,
# their shared libraries, the data files and the built web map.
#   docker build -t sema .
#   docker compose -f docker-compose.prod.yml up -d   (see README, "Yayın")

# ---- web map (TypeScript + Vite) ----------------------------------------------
FROM node:22-bookworm-slim AS web
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
# Absolute URL for the link-preview image (og:image); empty keeps it relative.
ARG SEMA_DOMAIN=""
RUN if [ -n "$SEMA_DOMAIN" ]; then export VITE_SITE_URL="https://$SEMA_DOMAIN"; fi && npm run build

# ---- C++ core -------------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update -q \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
      cmake ninja-build g++ libcurl4-openssl-dev nlohmann-json3-dev libpqxx-dev libboost-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY include/ include/
COPY src/ src/
COPY apps/ apps/
COPY sql/ sql/
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEMA_BUILD_TESTS=OFF -DSEMA_WARNINGS_AS_ERRORS=ON \
 && cmake --build build --target sema_collector sema_replay \
 && strip build/sema_collector build/sema_replay

# ---- runtime --------------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update -q \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
      libcurl4t64 libpqxx-7.8t64 ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /app --shell /usr/sbin/nologin sema
WORKDIR /app
COPY --from=build /src/build/sema_collector /src/build/sema_replay /usr/local/bin/
COPY data/ /app/data/
COPY LICENSE THIRD_PARTY_NOTICES.md /app/
COPY --from=web /src/web/dist/ /app/web/
ENV SEMA_DATA_DIR=/app/data \
    SEMA_WEB_ROOT=/app/web \
    SEMA_LIVE_ADDR=0.0.0.0 \
    SEMA_LIVE_PORT=8080
USER sema
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["sema_collector"]
