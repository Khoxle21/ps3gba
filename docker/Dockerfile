# PS3 homebrew toolchain: ps3toolchain (PPU/SPU gcc + PSL1GHT) and ps3libraries.
FROM debian:13-slim

ENV PS3DEV=/usr/local/ps3dev
ENV PSL1GHT=${PS3DEV}
ENV PATH=${PATH}:${PS3DEV}/bin:${PS3DEV}/ppu/bin:${PS3DEV}/spu/bin

RUN \
  apt-get -y update && \
  apt-get -y install autoconf bison build-essential cmake flex git libelf-dev libgmp3-dev libncurses5-dev libssl-dev libtool-bin pkg-config python-dev-is-python3 texinfo wget zlib1g-dev && \
  apt-get -y clean autoclean autoremove && \
  rm -rf /var/lib/{apt,dpkg,cache,log}/

ARG PS3TOOLCHAIN_COMMIT=de85d7d5d11dc0a99d823ece3d440eea46c34d0f
RUN \
  git clone https://github.com/ps3dev/ps3toolchain.git && \
  cd ps3toolchain && git checkout ${PS3TOOLCHAIN_COMMIT} && \
  rm scripts/009-ps3libraries.sh && ./toolchain.sh && \
  cd .. && rm -Rf ps3toolchain

# polarssl is skipped: its download is dead and mbedTLS replaces it.
ARG PS3LIBRARIES_COMMIT=b2efde5089af3a2152383b6a92541a64dea066e6
RUN \
  git clone https://github.com/ps3dev/ps3libraries.git && \
  cd ps3libraries && git checkout ${PS3LIBRARIES_COMMIT} && \
  rm scripts/017-polarssl-*.sh && \
  ./libraries.sh && \
  cd .. && rm -Rf ps3libraries

# Rebuild Tiny3D so it never emits misaligned multi-word stores: GCC 13 merges its float
# vertex writes into 8-byte stores, and a misaligned store to RSX memory (cache-inhibited)
# raises an alignment exception on real hardware (RPCS3 doesn't care).
# The patch double-buffers its vertex memory (see the patch for why).
ARG TINY3D_COMMIT=9b02ae6e9f21ff15185f8a3846bdca5304d7e0ae
COPY tiny3d-vertex-double-buffer.patch /tmp/
RUN \
  git clone https://github.com/wargio/tiny3d.git && \
  cd tiny3d && git checkout ${TINY3D_COMMIT} && \
  git apply /tmp/tiny3d-vertex-double-buffer.patch && \
  CFLAGS="-mstrict-align -fno-store-merging" make install -C lib --no-print-directory && \
  cd .. && rm -Rf tiny3d

WORKDIR /build
