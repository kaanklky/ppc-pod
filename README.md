<p align="center"><img src="resources/icon_sources/icon_128.png" width="128" height="128" alt="PowerPC Pod icon"></p>

# PowerPC Pod

### Disclaimer: This project built entirely through AI-assisted development!<br>Sorry, I can't call it 'slop'.

An AirPlay 1 receiver with a native Cocoa GUI, built to run on a real PowerPC Mac (developed and tested against a 700MHz iMac G4 running Mac OS X 10.5 Leopard). Advertises itself over Bonjour (`_raop._tcp`), accepts a real AirPlay 1 RTSP/RTP session (RSA-OAEP session key recovery, AES-CBC decrypt, ALAC decode), and plays audio via CoreAudio. Now-playing metadata (title/artist/album, cover art) and connection status are shown in a small window; the device name shown there and advertised over Bonjour defaults to the Mac's own hostname on first launch and can be edited and saved.

<p align="center"><img src="resources/screenshot.png" alt="PowerPC Pod screenshot"></p>

## Requirements to build

- Docker, running on a normal x86_64/arm64 Linux or Mac host (the actual cross-compiler runs inside a container - `powerpc-apple-darwin9` GCC cannot run natively on this era's host hardware).
- The Mac OS X 10.5 SDK (see "Getting the SDK" below).
- `vendor/mbedtls-src` and `vendor/mbedtls/lib-ppc` (see "Getting the vendored dependencies" below).
- A real (or virtual) PowerPC Mac running 10.4+ to actually run the built app on - this project cannot execute PowerPC code on the build host at all, only compile and link it.

## Getting the SDK

`sdk/` is gitignored - it's Apple's proprietary SDK, and its license does not permit redistributing it outside Apple's own channels, so it's never committed to this repo. To get it, you need access to a Mac with Xcode 3.1.4 installed (the last Xcode version with full PowerPC/Mac OS X 10.5 SDK support) - either a real Leopard-era Mac or a compatible VM:

```
# On the Xcode 3.1.4 machine:
cd /Developer/SDKs
tar czf MacOSX10.5.sdk.tar.gz MacOSX10.5.sdk

# Copy the resulting tarball into this repo:
scp MacOSX10.5.sdk.tar.gz your-dev-machine:/path/to/ppc-pod/sdk/
```

`docker/Dockerfile` expects exactly `sdk/MacOSX10.5.sdk.tar.gz` and extracts it during the image build below.

## Getting the vendored dependencies

`vendor/` is gitignored too (third-party source + prebuilt binaries, kept out of this repo so it stays to this project's own code):

```
git clone --branch v2.28.8 https://github.com/Mbed-TLS/mbedtls.git vendor/mbedtls-src
```

(Check the actual tag on GitHub if `v2.28.8` doesn't exist.)

## Building the cross-compiler image

```
./docker/build.sh
```

Builds a Docker image containing a real `powerpc-apple-darwin9` GCC 6.1.0 toolchain with the 10.5 SDK baked in. `docker/ppc-cc` is a thin wrapper that runs any compile through this image.

## Rebuilding mbedtls for PowerPC

Needed once, before the first app build:

```
docker run --rm -v "$(pwd)":/src ppc-pod-toolchain bash -c '
  cd /src/vendor/mbedtls-src/library && \
  make CC=/opt/osxcross/target/bin/powerpc-apple-darwin9-gcc \
       AR=/opt/osxcross/target/bin/powerpc-apple-darwin9-ar \
       RL=/opt/osxcross/target/bin/powerpc-apple-darwin9-ranlib \
       APPLE_BUILD=1
'
```

Then copy the resulting `.a` files into `vendor/mbedtls/lib-ppc/`.

## Building and deploying the app

`-mlong-double-64` is required, not optional: without it, GCC links every variadic
libc call (`fprintf`, `snprintf`, etc.) against the `$LDBL128`-suffixed symbol
variant that only exists starting with the later `long double` ABI - Mac OS X
10.2 Jaguar's libSystem doesn't have it and refuses to even launch the binary.
This flag makes GCC emit the plain legacy symbol names instead, which exist on
every target OS version, confirmed via a real dyld error before adding it.

```
./docker/ppc-cc -Os -mlong-double-64 -ffunction-sections -fdata-sections -Wl,-dead_strip \
  -I vendor/mbedtls-src/include -I src \
  src/ppc_pod_gui_main.m src/cocoa_ui.m \
  src/airplay_main.c \
  src/airplay_dmap.c src/airplay_rtsp.c src/airplay_rtp.c src/airplay_rsa.c src/airplay_session.c src/alac.c \
  src/app_state.c src/app_settings.c \
  src/mdns.c src/coreaudio_output.c \
  vendor/mbedtls/lib-ppc/libmbedtls.a vendor/mbedtls/lib-ppc/libmbedx509.a vendor/mbedtls/lib-ppc/libmbedcrypto.a \
  -framework Cocoa -framework Foundation -framework CoreServices -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
  -o ppc_pod_ppc

./docker/make_app_bundle.sh ppc_pod_ppc .

file "PowerPC Pod.app/Contents/MacOS/PowerPCPod"   # confirm: Mach-O ppc executable

scp -r "PowerPC Pod.app" your-ppc-mac:/Applications/
```

Once copied to `/Applications`, it's a normal double-click-to-run app - no separate install or pairing step. It does not register itself as a Login Item automatically - if you want it to start on login, add it yourself via System Preferences.

## Building for Intel (x86_64, Mac OS X 10.9 Mavericks+)

The same `ppc-pod-toolchain` Docker image already contains a full `x86_64-apple-darwin9` toolchain against the same 10.5 SDK (osxcross builds every architecture the SDK supports by default) - no separate SDK or Docker image needed. A few real differences from the PowerPC build, each hit and fixed during actual Mavericks-under-QEMU testing rather than guessed at:

- **Toolchain is Clang, not GCC.** Apple's real PowerPC toolchain was GCC-based, but osxcross only builds a GCC front end for the `powerpc*` targets; `x86_64-apple-darwin9` only has Clang (`x86_64-apple-darwin9-clang`). Link with the `o64-clang` wrapper, not the plain target-triple binary directly - plain `x86_64-apple-darwin9-clang` picks up the host's native GNU `ld` instead of the bundled Mach-O `ld`, producing `unrecognised emulation mode: acosx_version_min` errors. Even `o64-clang` alone wasn't enough in practice; pass `-B/opt/osxcross/target/bin -fuse-ld=/opt/osxcross/target/bin/x86_64-apple-darwin9-ld` explicitly to force the correct linker.
- **No `-mlong-double-64` needed or valid.** That flag exists specifically for PowerPC's historical `long double` ABI split (see above) - x86_64 doesn't have an equivalent split, so omit it entirely for this target.
- **mbedtls needs `MBEDTLS_NO_UDBL_DIVISION`.** Without it, `bignum.c`'s 128-bit division fast path (`mbedtls_t_udbl` via `__attribute__((mode(TI)))`) references `__udivti3`, a compiler-runtime builtin this toolchain doesn't ship for this target (no `libclang_rt`/`compiler-rt` built for `x86_64-apple-darwin9` here, unlike the real GCC-built `libgcc.a` the PowerPC target has). Defining this macro falls back to mbedtls's portable division implementation instead, avoiding the missing symbol entirely.
- **`vendor/mbedtls-src` needs a `3rdparty/Makefile.inc` stub.** This checkout's `library/Makefile` unconditionally `include`s `../3rdparty/Makefile.inc` (used for optional alt-crypto backends like Everest), but that directory was never present in this checkout at all, for either architecture - create it yourself with just `THIRDPARTY_INCLUDES=` and `THIRDPARTY_CRYPTO_OBJECTS=` (both empty) before building for any target. Also build only the `libmbedcrypto.a 
libmbedx509.a libmbedtls.a` targets directly (not the default `static` target, nor `make clean` first) - both of those additionally try `cd ../tests && echo ... > seedfile`, which fails the same way since `../tests` doesn't exist either; the real `.a` files are already correct prerequisites of `static` and build fine on their own.

```
# One-time: rebuild mbedtls for x86_64 (same vendor/mbedtls-src checkout as PowerPC, different target)
mkdir -p vendor/mbedtls/lib-x64
docker run --rm -v "$(pwd)":/src ppc-pod-toolchain bash -c '
  cd /src/vendor/mbedtls-src/library && \
  make libmbedcrypto.a libmbedx509.a libmbedtls.a \
       CC=/opt/osxcross/target/bin/x86_64-apple-darwin9-clang \
       AR=/opt/osxcross/target/bin/x86_64-apple-darwin9-ar \
       RL=/opt/osxcross/target/bin/x86_64-apple-darwin9-ranlib \
       CFLAGS="-DMBEDTLS_NO_UDBL_DIVISION" \
       APPLE_BUILD=1
'
cp vendor/mbedtls-src/library/libmbedcrypto.a vendor/mbedtls-src/library/libmbedx509.a vendor/mbedtls-src/library/libmbedtls.a vendor/mbedtls/lib-x64/

# App build - same source files as the PowerPC build, different compiler/libs
docker run --rm -v "$(pwd)":/src -w /src ppc-pod-toolchain bash -c '
  /opt/osxcross/target/bin/o64-clang -Os -B/opt/osxcross/target/bin \
    -fuse-ld=/opt/osxcross/target/bin/x86_64-apple-darwin9-ld \
    -ffunction-sections -fdata-sections -Wl,-dead_strip \
    -I vendor/mbedtls-src/include -I src \
    src/ppc_pod_gui_main.m src/cocoa_ui.m \
    src/airplay_main.c \
    src/airplay_dmap.c src/airplay_rtsp.c src/airplay_rtp.c src/airplay_rsa.c src/airplay_session.c src/alac.c \
    src/app_state.c src/app_settings.c \
    src/mdns.c src/coreaudio_output.c \
    vendor/mbedtls/lib-x64/libmbedtls.a vendor/mbedtls/lib-x64/libmbedx509.a vendor/mbedtls/lib-x64/libmbedcrypto.a \
    -framework Cocoa -framework Foundation -framework CoreServices -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
    -o ppc_pod_x64
'

./docker/make_app_bundle.sh ppc_pod_x64 .

file "PowerPC Pod.app/Contents/MacOS/PowerPCPod"   # confirm: Mach-O 64-bit x86_64 executable
```

Tested end to end under QEMU/KVM (`q35` machine, OpenCore 0.6.7 bootloader, `-cpu Penryn`) against a real Mac OS X 10.9.5 Mavericks install: app launches, AirPlay advertises and accepts connections, and networking works over a bridged NIC. Audio output was not yet confirmed working in this VM setup at time of writing (`AppleALC`/`Lilu` load and patch correctly, but the specific codec layout-id QEMU's emulated HDA codec needs wasn't pinned down) - untested whether this is a QEMU-emulated-codec-specific gap or would also affect real Intel Mac hardware.

## Changing the app icon

`resources/AppIcon.icns` is already built and included. To use a different icon, replace the PNGs in `resources/icon_sources/` (16/32/48/128/256/512px) and run:

```
./docker/make_icns.sh
```

## Project layout

```
src/                            all source (Objective-C + C)
  ppc_pod_gui_main.m            process entry point (main()), spawns the AirPlay
                                backend thread, runs the Cocoa UI
  cocoa_ui.m/.h                 the window: device name field, now-playing display
  airplay_main.c                AirPlay TCP listener / connection lifecycle
  airplay_rtsp.c/.h             RTSP handshake, SET_PARAMETER, DMAP metadata push
  airplay_rtp.c/.h              RTP audio packet receive + decrypt + decode + output
  airplay_rsa.c/.h              RSA-OAEP session key recovery
  airplay_session.c/.h          per-connection session state
  airplay_dmap.c/.h             DMAP (iTunes metadata format) parser
  alac.c/.h                     Apple Lossless decoder
  coreaudio_output.c/.h         CoreAudio playback
  app_state.c/.h                shared state between the backend thread and the UI
  app_settings.c/.h             device name persistence (settings.txt) + hostname default
  mdns.c/.h                     Bonjour/mDNS responder
docker/                         cross-compile toolchain (Dockerfile, ppc-cc wrapper,
                                make_app_bundle.sh, make_icns.sh)
resources/icon_sources/         icon artwork at every needed size
resources/AppIcon.icns          app icon
resources/Info.plist            app bundle metadata, copied verbatim by make_app_bundle.sh
resources/screenshot.png        README screenshot
sdk/                            the Mac OS X 10.5 SDK - gitignored, see "Getting the SDK"
vendor/                         mbedtls (source + prebuilt libs per architecture) - gitignored,
                                see "Getting the vendored dependencies"; lib-ppc/ for PowerPC,
                                lib-x64/ for Intel (see "Building for Intel")
```
