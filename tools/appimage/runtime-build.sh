#!/bin/bash
set -euo pipefail
export SOURCE_DATE_EPOCH=1780000000 LC_ALL=C TZ=UTC
mkdir /build
cd /build
tar xf /proof/inputs/type2-runtime.tar.gz
mv type2-runtime-* runtime-source
tar xf /proof/inputs/fuse-3.15.0.tar.xz
cd fuse-3.15.0
patch -p1 < /build/runtime-source/patches/libfuse/mount.c.diff
meson setup build --prefix=/usr -Ddefault_library=static -Dexamples=false -Dtests=false
ninja -C build -j2 install
cd /build
tar xf /proof/inputs/squashfuse-0.5.2.tar.gz
cd squashfuse-0.5.2
export CFLAGS='-ffunction-sections -fdata-sections -Os -ffile-prefix-map=/build=.'
./autogen.sh
./configure LDFLAGS=-static
make -j2
make install
mkdir -p /usr/local/include/squashfuse
cp ./*.h /usr/local/include/squashfuse/
cd /build/runtime-source/src/runtime
printf '%s' 8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa > version
make -j2 CFLAGS='-std=gnu99 -Os -D_FILE_OFFSET_BITS=64 -DGIT_COMMIT=\"8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa\" -T data_sections.ld -ffunction-sections -fdata-sections -Wl,--gc-sections -static -Wall -Werror -static-pie -Wl,--build-id=none,-Map,/build/runtime.map -ffile-prefix-map=/build=.' runtime
strip --strip-debug --strip-unneeded runtime
printf 'AI\002' | dd of=runtime bs=1 count=3 seek=8 conv=notrunc
mkdir -p /proof/$RUN_NAME
cp runtime /proof/$RUN_NAME/runtime-x86_64
cp /build/runtime.map /proof/$RUN_NAME/runtime.map
cp /lib/apk/db/installed /proof/$RUN_NAME/apk-installed-db.txt
sha256sum runtime
wc -c runtime
readelf -d runtime
