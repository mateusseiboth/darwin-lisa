# UniXNU's Linux toolchain for building XNU (xnu-12377): clang with the
# __builtin_xnu_type_* patch, ld64 (cctools-port), mig, iig, lzfse.
# The installer writes shims to /usr/bin, so it lives in this image and not
# on the host. Build context: UniXNU's scripts/linux (see build.sh).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends python3 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY linux /opt/unixnu-scripts/linux
ARG JOBS=14
RUN python3 /opt/unixnu-scripts/linux/setup-toolchain.py --package-manager apt -j "$JOBS" \
 && python3 /opt/unixnu-scripts/linux/setup-toolchain.py --check \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
