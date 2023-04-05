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

Porto requires Linux kernel 3.18 and optionally some offstream patches.

## Dependencies

```bash
sudo apt install -y \
    g++ \
    cmake \
    protobuf-compiler \
    libprotobuf-dev \
    libnl-3-dev \
    libnl-route-3-dev \
    libnl-idiag-3-dev \
    libncurses5-dev \
    libelf-dev \
    zlib1g-dev \
    libbpf-dev \
    pandoc

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

## Build

```bash
mkdir build && cd build
cmake ..

# use make -j for parallel build
make

# default installation path is /usr/sbin
sudo make install
```
or
```bash
# should install additional dependencies for deb package building
dpkg-buildpackage -b
sudo dpkg -i ../porto_*.deb
```

## Run

```bash
sudo groupadd porto
sudo adduser $USER porto
sudo portod start
portoctl exec hello command='echo "Hello, world!"'
```

## Documentation
* [Porto manpage](porto.md)
