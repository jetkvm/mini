# syntax=docker/dockerfile:1
# A bench firmware image of this checkout: the web UI, then the firmware with
# the UI embedded. Every bench image is built here, through
# tools/combined_load/runner/build.mjs, which tools/combined_load/run.sh and
# tools/e2e/run.sh call.
#
#   docker buildx build --build-arg VERSION=<PROJECT_VER> --build-arg BOARD=<ethernet|wifi> \
#       [--build-arg MOCK_CLOUD_JWKS_URL=<url>] [--build-arg WIFI_SETUP_LOG_CODE=1] \
#       [--build-arg USB_DEBUGGING=1] --output type=local,dest=<dir> .
#
# USB_DEBUGGING=1 keeps the USB Serial/JTAG port on by default.
#
# <dir> gets jetkvm-mini.bin (the update image) plus what
# tools/e2e/serial_flash.sh writes over USB: bootloader.bin,
# partition-table.bin, ota_data_initial.bin and flasher_args.json. The context
# has no .git (.dockerignore), so the version is an argument.

FROM node:22.21.1-bookworm-slim AS ui
WORKDIR /src/frontend/jetkvm-ui
COPY frontend/jetkvm-ui/package.json frontend/jetkvm-ui/package-lock.json ./
RUN npm ci
# The whole directory: public/sse.html is a link to ../../internal/logging.
COPY frontend /src/frontend
RUN npm run build:device -- --outDir dist-p4

FROM espressif/idf:v6.0.2 AS firmware
SHELL ["/bin/bash", "-c"]
ENV IDF_CCACHE_ENABLE=1 CCACHE_DIR=/ccache CCACHE_BASEDIR=/src CCACHE_NOHASHDIR=1 \
    CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=8G \
    IDF_COMPONENT_CACHE_PATH=/component-cache
WORKDIR /src
COPY . .
COPY --from=ui /src/frontend/jetkvm-ui/dist-p4 frontend/jetkvm-ui/dist-p4
ARG VERSION
ARG BOARD
ARG MOCK_CLOUD_JWKS_URL=
ARG WIFI_SETUP_LOG_CODE=0
ARG USB_DEBUGGING=0
# A fresh tree's first configure stops once and asks for another run
# (CMakeLists.txt). The Component Manager does not lock its cache, so only one
# configure at a time may hold it.
RUN --mount=type=cache,target=/component-cache,sharing=locked \
    . "$IDF_PATH/export.sh" > /dev/null && \
    defaults=() && \
    case "$USB_DEBUGGING" in \
        0) ;; \
        1) echo CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=y > /tmp/usb_debugging.defaults && \
           defaults=("-DSDKCONFIG_DEFAULTS=/src/sdkconfig.defaults;/tmp/usb_debugging.defaults") ;; \
        *) echo "USB_DEBUGGING must be 0 or 1" >&2; exit 1 ;; \
    esac && \
    configure() { \
        idf.py -B /build -DSDKCONFIG=/build/sdkconfig -DPROJECT_VER="$VERSION" \
            -DJETKVM_BOARD_VARIANT="$BOARD" -DJETKVM_WIFI_SETUP_LOG_CODE="$WIFI_SETUP_LOG_CODE" \
            -DJETKVM_MOCK_CLOUD_JWKS_URL="$MOCK_CLOUD_JWKS_URL" "${defaults[@]}" reconfigure; \
    } && \
    { configure || configure; }
RUN --mount=type=cache,target=/ccache \
    . "$IDF_PATH/export.sh" > /dev/null && \
    idf.py -B /build build

# tools/run_all_tests.sh on a build host (tools/run_host_tests_remote.sh):
#
#   docker buildx build --target host-tests --build-arg TEST_RUN=<new value> \
#       --output type=local,dest=<dir> .
#
# <dir> gets host-tests.log and host-tests.status. A failing test does not
# fail the build, so the log comes back. A new TEST_RUN reruns the tests.
FROM espressif/idf:v6.0.2 AS host-tests-run
SHELL ["/bin/bash", "-c"]
ENV IDF_COMPONENT_CACHE_PATH=/component-cache
RUN apt-get update && \
    apt-get install -y --no-install-recommends libssl-dev zlib1g-dev iproute2 nodejs jq xorriso genisoimage && \
    rm -rf /var/lib/apt/lists/*
RUN . "$IDF_PATH/export.sh" > /dev/null && python3 -m pip install --quiet pillow pyserial jinja2
WORKDIR /src
COPY . .
# The combined_load tests read the checkout's commit; the context has no .git.
RUN git init -q && git add -A && \
    git -c user.name=host-tests -c user.email=host-tests@localhost commit -q -m "host tests"
# components/web_app needs a built UI to configure; the host tests ignore it.
RUN --mount=type=cache,target=/component-cache,sharing=locked \
    . "$IDF_PATH/export.sh" > /dev/null && \
    mkdir -p frontend/jetkvm-ui/dist-p4 && \
    echo '<!doctype html><title>host tests</title>' > frontend/jetkvm-ui/dist-p4/index.html && \
    { idf.py -B build-ci reconfigure || idf.py -B build-ci reconfigure; }
ARG TEST_RUN
RUN --network=none \
    . "$IDF_PATH/export.sh" > /dev/null && \
    export SDKCONFIG="$(ls build-ci/sdkconfig.jetkvm-*)" && \
    export JETKVM_TEST_TOOLCHAIN_BIN="$(dirname "$(command -v riscv32-esp-elf-objcopy)")" && \
    mkdir /out && \
    { tools/run_all_tests.sh 2>&1 | tee /out/host-tests.log; echo "${PIPESTATUS[0]}" > /out/host-tests.status; }

FROM scratch AS host-tests
COPY --from=host-tests-run /out /

FROM scratch
COPY --from=firmware /build/jetkvm-mini.bin /build/bootloader/bootloader.bin \
    /build/partition_table/partition-table.bin /build/ota_data_initial.bin \
    /build/flasher_args.json /
