# OCERA build/dev environment
# Every teammate gets this exact same compiler + tools, regardless of
# whether their host machine is Windows, Mac, or Linux.

FROM ubuntu:24.04

# Avoid interactive prompts during package install
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gdb \
    git \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Nothing is copied in here on purpose - in development we mount the
# source tree as a live volume (see devcontainer.json / docker run -v),
# so edits on your host machine show up instantly inside the container
# without rebuilding the image.

CMD ["bash"]
