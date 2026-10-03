# syntax=docker/dockerfile:1
#
#   docker build -t tinykv .
#   docker run --rm -p 9999:9999 -v tinykv-data:/data tinykv
#
# The build stage compiles with -Werror and runs the whole test suite, so an
# image only exists if every test passed.

# ---- build + test ----
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev cmake make python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DTINYKV_WERROR=ON \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---- runtime: just the binary ----
FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --no-create-home tinykv \
 && mkdir /data && chown tinykv /data
COPY --from=build /src/build/kvserver /usr/local/bin/kvserver
USER tinykv
# the WAL and its snapshot live here; mount a volume to keep data across containers
VOLUME /data
EXPOSE 9999
# kvserver handles SIGTERM itself (graceful shutdown), so `docker stop`
# finishes in-flight writes instead of waiting 10 s and then SIGKILLing.
# Extra flags can be appended: docker run tinykv --threads 4
ENTRYPOINT ["kvserver", "--wal", "/data/kv.log"]
