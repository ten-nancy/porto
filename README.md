# Porto

## Overview

Porto is a yet another Linux container management system.

The main goal is providing single entry point for several Linux subsystems
such as cgroups, namespaces, mounts, networking, etc.
Porto is intended to be a base for large infrastructure projects.

### Key Features
* **Nested containers**       - containers could be put into containers
* **Nested virtualizaion**    - containers could use porto service too
* **Flexible configuration**  - all container parameters are optional
* **Reliable service**        - porto upgrades without restarting containers

Container management software build on top of porto could be transparently
enclosed inside porto container.

Porto provides a protobuf interface via an unix socket /run/portod.socket.

Command line tool **portoctl** and C++, Python and Go APIs are included.

Porto requires Linux kernel 4.5 and optionally some offstream patches.

## Dependencies

Ubuntu 22.04 dependencies as follows:

```bash
sudo apt install -y \
    g++ \
    cmake \
    protobuf-compiler \
    libprotobuf-dev \
    libgoogle-perftools-dev \
    libnl-3-dev \
    libnl-genl-3-dev \
    libnl-route-3-dev \
    libnl-idiag-3-dev \
    libncurses5-dev \
    libelf-dev \
    zlib1g-dev \
    pandoc \
    libseccomp-dev \
    libbpf-dev # for focal or newer

# dependencies for deb package building
sudo apt install -y \
    dpkg-dev \
    debhelper \
    pkg-config \
    autoconf \
    libtool \
    dh-python \
    python-all \
    python-setuptools \
    python3-setuptools \
    bash-completion
```

In Ubuntu 24.04 libprotobuf-dev requires libtool-bin, but it
should be installed manually

```bash
sudo apt install -y libtool-bin
```

## Build

```bash
mkdir build && cd build

# use DUSE_SYSTEM_LIBBPF for xenial and bionic
cmake .. # -DUSE_SYSTEM_LIBBPF=OFF

# use make -j for parallel build
make

# default installation path is /usr/sbin
sudo make install
```
or
```bash
# should install additional dependencies for deb package building
./dpkg-buildpackage -b
sudo dpkg -i ../porto_*.deb
```

## Build in Docker

To build in docker's container docker and docker-buildx should be installed

```bash
sudo apt install -y \
    docker.io \
    docker-buildx
```
The following command creates necessary binaries in build directory of porto source tree
```bash
docker build -t env_ubuntu22.04 -f scripts/Dockerfile .
docker run -v $(pwd):/porto docker.io/library/env_ubuntu22.04 bash -c "mkdir /porto/build; cd /porto/build; cmake ..; make -j4"
```

## Run

```bash
sudo groupadd porto
sudo adduser $USER porto
sudo portod start
portoctl exec hello command='echo "Hello, world!"'
```

## Documentation
* [Porto manpage](doc/porto.md)
