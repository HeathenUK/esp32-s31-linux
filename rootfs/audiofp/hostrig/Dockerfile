FROM debian:bookworm
RUN apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q gcc libc6-dev libasound2-dev libsdl2-dev alsa-utils procps strace && rm -rf /var/lib/apt/lists/*
