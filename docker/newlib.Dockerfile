FROM ubuntu:18.04 as base

RUN apt update
RUN apt install -y build-essential git cmake gcc-8 g++-8

WORKDIR /usr
RUN git clone https://github.com/riscv/riscv-gnu-toolchain
WORKDIR /usr/riscv-gnu-toolchain
RUN git submodule update --init riscv-binutils
RUN git submodule update --init riscv-gcc
RUN git submodule update --init riscv-newlib

RUN apt install -y \
  autoconf \
  automake \
  autotools-dev \
  libmpc-dev \
  libmpfr-dev \
  libgmp-dev \
  zlib1g-dev \
  curl \
  gawk \
  bison \
  flex \
  texinfo

ENV RISCV_INSTALL /usr/riscv
ENV RISCV_ARCH    rv32gc
ENV RISCV_ABI     ilp32d

ENV CXX g++-8
ENV CC  gcc-8
RUN ./configure --prefix=$RISCV_INSTALL --with-arch=$RISCV_ARCH --with-abi=$RISCV_ABI --disable-gdb
RUN make -j8

WORKDIR /usr/riscv-gnu-toolchain
RUN rm -rf build-*

ENV PATH $RISCV_INSTALL/bin:$PATH
RUN mkdir -p /usr/outside
WORKDIR /usr/outside
