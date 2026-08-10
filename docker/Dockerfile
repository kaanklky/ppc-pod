# powerpc-apple-darwin9 cross-compiler: cctools-port + Clang, then GCC targeting PowerPC.
#
# Clang alone is NOT enough: clang-14's PowerPC/Darwin backend has an internal LLVM
# datalayout bug for this exact triple ("backend data layout ... does not match expected
# target description", Mach-O 'm:o' vs ELF 'm:e' name mangling mismatch) - confirmed by
# actually compiling with oppc32-clang, not assumed. GCC is required.
#
# Building GCC 6.1.0 (osxcross ppc-test's own tested default) under bookworm's gcc-12 host
# hits ISO C++ conformance errors in GCC's own source (e.g. gcc/ubsan.c comparing a pointer
# to a non-null-constant integer) that older host compilers accepted. GCC's own compile
# error names its fix directly: [-fpermissive]. Passing that to the host CXX avoids needing
# a whole separate era-appropriate host toolchain (tried via Debian jessie - that path is a
# dead end anyway, since jessie's ancient libtool can't handle cctools-port's bundled
# Makefiles, and jessie's glibc is too old to run bookworm-built binaries at all).
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    clang \
    patch \
    libxml2-dev \
    bash \
    build-essential \
    gcc \
    g++ \
    zlib1g-dev \
    libmpc-dev \
    libmpfr-dev \
    libgmp-dev \
    libssl-dev \
    autoconf \
    automake \
    libtool \
    cmake \
    curl \
    ca-certificates \
    cpio \
    python3 \
    uuid-dev \
    llvm-dev \
    wget \
    bzip2 \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/osxcross

RUN git clone --branch ppc-test --single-branch https://github.com/tpoechtrager/osxcross.git . \
    && (test -x tools/get_dependencies.sh && tools/get_dependencies.sh || true)

# The project only needs C (protocol client) and Objective-C (Phase 5 Cocoa UI), not C++.
# Building C++/Obj-C++ support means cross-building libstdc++ against this old Apple SDK's
# own bundled (GCC 4.0.0-era) libstdc++ headers, which fails with cascading "not declared"
# errors from a header integration mismatch unrelated to the host-compiler issues already
# fixed above. Dropping c++/obj-c++ avoids that entirely instead of chasing it further.
# build_gcc.sh has several spots that unconditionally assume C++ was built alongside C -
# each was tracked down by bisecting with checkpoint tracing, since most fail silently
# (either via an explicit "&>/dev/null" swallowing the real error, or by not printing one
# at all) and `set -e` aborts the whole script the instant any of them returns nonzero:
#  - line ~215: `pushd c++/${GCC_VERSION}*` (post-install libstdc++ header patch)
#  - line ~211: `pushd .../$ARCH-apple-$TARGET/include` (preamble to the above - same fate
#    once c++ is excluded, even though it looks unrelated to c++ specifically)
#  - line ~242: `mv $ARCH-apple-$TARGET-g++ $ARCH-apple-$TARGET-base-g++` (no g++ binary
#    exists to move - this was the actual final blocker, confirmed via checkpoint tracing:
#    execution reached the checkpoint right after the preceding gcc `mv`, never the one
#    right after this g++ `mv`)
#  - line ~248: `ln -sf $GCC_ARCH-apple-$TARGET-base-g++ ...` (same reason, symlinking a
#    base-g++ that was never created)
RUN sed -i 's/LANGS="c,c++,objc,obj-c++"/LANGS="c,objc"/' build_gcc.sh \
    && sed -i '/test_compiler \$GCC_ARCH_SHORT32-g++/d; /test_compiler \$GCC_ARCH_SHORT-g++/d' build_gcc.sh \
    && sed -i '/pushd c++\/\${GCC_VERSION}\* &>\/dev\/null/,/popd &>\/dev\/null/d' build_gcc.sh \
    && sed -i '/pushd \$OSXCROSS_TARGET_DIR\/\$ARCH_TO_BUILD-apple-\$OSXCROSS_TARGET\/include &>\/dev\/null/,+1d' build_gcc.sh \
    && sed -i '/mv \$ARCH_TO_BUILD-apple-\$OSXCROSS_TARGET-g++ /d' build_gcc.sh \
    && sed -i '/ln -sf \$GCC_ARCH-apple-\$OSXCROSS_TARGET-base-g++ /d' build_gcc.sh

COPY sdk/MacOSX10.5.sdk.tar.gz tarballs/

ENV UNATTENDED=1
ENV OSX_VERSION_MIN=10.4
ENV GCC_VERSION=6.1.0

RUN ./build.sh

ENV CFLAGS=-fpermissive
ENV CXXFLAGS=-fpermissive

# build_gcc.sh does NOT loop over architectures - it builds x86_64/i386 by default and
# only targets PowerPC (oppc32/oppc64, powerpc-apple-darwin9) when POWERPC=1 is set. An
# earlier attempt spent significant time building (and debugging) an x86_64 GCC by
# omission of this variable, which this project never needed - o32-clang/o64-clang from
# build.sh already cover x86_64/i386.
ENV POWERPC=1

RUN ./build_gcc.sh

# The built toolchain is NOT relocatable: build_gcc.sh configures GCC with hardcoded
# absolute paths (--with-as=/opt/osxcross/target/bin/../bin/powerpc64-apple-darwin9-as,
# same for --with-ld). GCC's sysroot/include search relocates fine relative to argv[0]
# (its standard prefix-relocation mechanism), but --with-as/--with-ld do not get the same
# treatment - confirmed by actually copying target/ out to a host path and hitting the
# assembler silently falling back to the host's plain "as" (GNU binutils, ELF-only, which
# misparses "-arch ppc" as its own "-a" listing option and dies with "invalid listing
# option `r'"). Copying target/ out of the container is a dead end; use docker/ppc-cc
# instead, which runs compiles through this image so /opt/osxcross/target always exists
# at the path GCC was actually configured with.
CMD ["bash"]
