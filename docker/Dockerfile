# Reproducible build environment for esp32-s31-linux.
#
# Mirrors .github/workflows/release-images.yml (ubuntu-24.04) so a local build
# matches CI. Solves three macOS blockers: the Makefile's GNU-only tools
# (nproc, stat -c, truncate, sha256sum), Apple's make 3.81 (Buildroot needs
# make 4.x), and the fact that a stock ESP-IDF checkout has no esp32s31 target.
#
# The prebuilt S31 toolchain is published as a single x86_64 tarball, so this
# image is amd64 and runs emulated on Apple Silicon.

FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Package list is the CI set, plus what the Buildroot rootfs and the JFFS2
# persist image need on top of it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        bison bc build-essential ccache cpio device-tree-compiler flex gperf \
        libffi-dev libssl-dev ninja-build rsync unzip \
        ca-certificates curl git wget file python3 python3-pip python3-venv \
        libncurses-dev mtd-utils squashfs-tools xxd sudo \
    && rm -rf /var/lib/apt/lists/*

# Pinned by CI. ESP_IDF_REF is a master commit that predates any release tag,
# and is the only revision known to carry working esp32s31 support.
ENV IDF_PATH=/opt/esp-idf
ENV ESP_IDF_REF=2067f3ae32b7e7b61a0f3a052df63d619cd7c61d
ENV TOOLCHAIN_RELEASE_TAG=esp32s31-linux-gcc-15.2.0-4

RUN git clone --filter=blob:none https://github.com/espressif/esp-idf.git "$IDF_PATH" \
    && git -C "$IDF_PATH" checkout "$ESP_IDF_REF" \
    && git -C "$IDF_PATH" submodule update --init --recursive --depth 1

# install.sh verifies every tool it installs by running it, and openocd-esp32
# needs libusb at runtime. Without this the whole install aborts with
# "libusb-1.0.so.0: cannot open shared object file", even though this build
# never uses openocd. Kept as its own layer so the ESP-IDF clone above stays
# cached when the package list changes.
RUN apt-get update && apt-get install -y --no-install-recommends libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Buildroot refuses to run as root, so every build runs as this user. The uid
# and gid are baked in at build time to match the host user, so bind-mounted
# output stays writable on the host and ESP-IDF's tool cache under
# /home/builder stays writable in the container.
ARG BUILDER_UID=1000
ARG BUILDER_GID=1000
RUN if getent group "$BUILDER_GID" >/dev/null; then \
            groupmod -n builder "$(getent group "$BUILDER_GID" | cut -d: -f1)"; \
        else \
            groupadd -g "$BUILDER_GID" builder; \
        fi \
    && if getent passwd "$BUILDER_UID" >/dev/null; then \
            usermod -l builder -d /home/builder -m -g "$BUILDER_GID" \
                "$(getent passwd "$BUILDER_UID" | cut -d: -f1)"; \
        else \
            useradd -m -s /bin/bash -u "$BUILDER_UID" -g "$BUILDER_GID" builder; \
        fi \
    && echo 'builder ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/builder \
    && chown -R builder:builder "$IDF_PATH" /home/builder

# Create the ccache and /work directories in the image so the named volumes
# mounted over them inherit builder ownership; Docker initialises a fresh
# volume from the image path, and an absent path would give a root-owned one.
#
# /work holds the toolchain and all build output. It must NOT live on the
# /src bind mount: colima mounts that over virtiofs, where the toolchain
# tarball's read-only directories cannot be written into afterwards, so
# extraction fails with "Cannot open: Permission denied". Buildroot and the
# kernel build create hardlinks and read-only trees too, so the whole build
# stays on a real Linux filesystem.
RUN install -d -o builder -g builder /home/builder/.ccache /work

USER builder

# Installs the esp32s31 cross toolchain and the Python environment ESP-IDF
# needs. Done as builder so the tool cache lands in the right home directory.
RUN "$IDF_PATH/install.sh" esp32s31

WORKDIR /src

# The entrypoint runs `bash -lc`, and a login shell reads /etc/profile rather
# than ~/.bashrc, so the export has to live in profile.d to take effect.
# Placed after install.sh so that layer stays cached when this changes.
USER root
RUN printf '. "$IDF_PATH/export.sh" >/dev/null 2>&1 || true\n' \
        > /etc/profile.d/esp-idf.sh
USER builder

ENTRYPOINT ["/bin/bash", "-lc"]
CMD ["make help || make -n all"]
