#!/usr/bin/env bash
# Run a build inside the CI-equivalent container.
#
#   docker/build.sh                 # default: make all
#   docker/build.sh make bootloader
#   docker/build.sh bash            # interactive shell
#
# The repo is bind-mounted at /src so source edits on the host are picked up
# directly. build/ and toolchain/ are Docker volumes mounted at their normal
# paths inside that tree, because virtiofs cannot host them: the toolchain
# tarball's read-only directories and Buildroot's hardlinks both fail there.
# Mounting at the normal paths (rather than overriding BUILD_DIR) matters --
# buildroot-external/board/esp32-s31/post-build.sh hardcodes
# "${project_dir}/build/linux/..." and the Buildroot defconfig hardcodes
# "$(BR2_EXTERNAL_ESP32_S31_PATH)/../toolchain/...".
#
# Consequence: build output is NOT visible on the host. Copy images out with
#
#   docker/build.sh 'cp /src/build/fw_payload.bin /src/build/xipImage \
#                       /src/build/rootfs.sqfs /src/images/'

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_RELEASE_TAG="${TOOLCHAIN_RELEASE_TAG:-esp32s31-linux-gcc-15.2.0-4}"

# PLATFORM=linux/arm64 selects the native (non-emulated) image, which builds the
# S31 toolchain from source instead of using the x86_64-only release. Volumes
# are per-platform because a toolchain built for one host cannot run on the
# other, and mixing them would silently corrupt a build tree.
PLATFORM="${PLATFORM:-linux/amd64}"
case "$PLATFORM" in
linux/arm64)
	DOCKERFILE=Dockerfile.arm64
	IMAGE="${IMAGE:-esp32-s31-linux-build-arm64}"
	VOL_PREFIX=esp32-s31-arm64
	;;
linux/amd64)
	DOCKERFILE=Dockerfile
	IMAGE="${IMAGE:-esp32-s31-linux-build}"
	VOL_PREFIX=esp32-s31
	;;
*)
	echo "ERROR: PLATFORM must be linux/amd64 or linux/arm64" >&2
	exit 1
	;;
esac

if ! docker info >/dev/null 2>&1; then
	echo "ERROR: the Docker daemon is not reachable. Start Docker Desktop or 'colima start'." >&2
	exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "--- Building $IMAGE (first run downloads ESP-IDF and its tools) ---"
	# The builder uid/gid must match the caller at image build time: ESP-IDF's
	# tool cache lives in the image under /home/builder and has to stay
	# writable when the container runs as the host user.
	docker build --platform="$PLATFORM" \
		-f "$REPO_ROOT/docker/$DOCKERFILE" \
		--build-arg "BUILDER_UID=$(id -u)" \
		--build-arg "BUILDER_GID=$(id -g)" \
		-t "$IMAGE" "$REPO_ROOT/docker"
fi

# ccache and the whole build tree live on Docker volumes, off the /src bind
# mount. See the /work comment in the Dockerfile: virtiofs cannot handle the
# read-only directory trees and hardlinks the toolchain and Buildroot create.
docker volume create ${VOL_PREFIX}-ccache >/dev/null
docker volume create ${VOL_PREFIX}-work >/dev/null
docker volume create ${VOL_PREFIX}-toolchain >/dev/null

# Both volumes are mounted at paths inside the /src bind mount, because the
# project hardcodes them: the Buildroot defconfig pins
# BR2_TOOLCHAIN_EXTERNAL_PATH to "$(BR2_EXTERNAL_ESP32_S31_PATH)/../toolchain/"
# and post-build.sh pins "${project_dir}/build/linux/". They must nonetheless be
# real filesystems rather than virtiofs. Docker creates a volume mounted inside
# a bind mount as root-owned (there is no image content at that path to seed
# ownership from), so fix both up front. Idempotent and cheap.
docker run --rm --platform="$PLATFORM" -u 0 \
	-v ${VOL_PREFIX}-toolchain:/t -v ${VOL_PREFIX}-work:/b "$IMAGE" \
	"chown $(id -u):$(id -g) /t /b" >/dev/null

# Only allocate a TTY when there is one; -it breaks piped or background runs.
# The ${a[@]+"${a[@]}"} form is required because macOS ships bash 3.2, where
# expanding an empty array under `set -u` is an unbound-variable error.
TTY_ARGS=()
[ -t 0 ] && TTY_ARGS=(-it)

# The image's builder user already carries the caller's uid/gid (see the
# --build-arg above), so no -u override is needed here.
exec docker run --rm ${TTY_ARGS[@]+"${TTY_ARGS[@]}"} \
	--platform="$PLATFORM" \
	-v "$REPO_ROOT:/src" \
	-v ${VOL_PREFIX}-ccache:/home/builder/.ccache \
	-v ${VOL_PREFIX}-work:/src/build \
	-v ${VOL_PREFIX}-toolchain:/src/toolchain \
	-e TOOLCHAIN_RELEASE_TAG="$TOOLCHAIN_RELEASE_TAG" \
	-e IDF_EXPORT=/opt/esp-idf/export.sh \
	-e HOME=/home/builder \
	-e S31_MAKE="make TOOLCHAIN_RELEASE_TAG=$TOOLCHAIN_RELEASE_TAG IDF_EXPORT=/opt/esp-idf/export.sh" \
	-w /src \
	"$IMAGE" \
	"${*:-\$S31_MAKE all}"
