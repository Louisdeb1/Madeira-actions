#!/bin/bash
set -e

BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$BUILD_DIR/../.." && pwd)"
WINE_SRC="$REPO_ROOT/wine"
WINE_BUILD="$WINE_SRC/build-macos"
SDK_NAME="${MADEIRA_SDK_NAME:-iphoneos}"
SDK=$(xcrun --sdk "$SDK_NAME" --show-sdk-path)
if [ "$SDK_NAME" = "iphonesimulator" ]; then MIN_FLAG="-mios-simulator-version-min=17.0"; else MIN_FLAG="-miphoneos-version-min=17.0"; fi
OBJ_DIR="${MADEIRA_OBJ_DIR:-$BUILD_DIR/obj}"
APP_LIB="${MADEIRA_OUTPUT_LIB:-$REPO_ROOT/app/Madeira/libntdll_unix.a}"
EXTRA_CFLAGS="${MADEIRA_EXTRA_CFLAGS:-}"

mkdir -p "$OBJ_DIR"

SUCCEEDED=0
FAILED=0
FAILED_FILES=""

compile_one() {
    local src=$1
    local name=$2
    echo -n "  $name... "

    if xcrun -sdk "$SDK_NAME" clang \
        -arch arm64 -isysroot "$SDK" "$MIN_FLAG" $EXTRA_CFLAGS \
        -O2 -fPIC -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing \
        -Wno-implicit-function-declaration -Wno-int-conversion \
        -include "$WINE_BUILD/include/config.h" \
        -include "$BUILD_DIR/shims/wine_ios_exit.h" \
        -I"$BUILD_DIR" -I"$BUILD_DIR/shims" \
        -I"$WINE_BUILD/dlls/ntdll" -I"$WINE_SRC/dlls/ntdll" -I"$WINE_SRC/dlls/ntdll/unix" \
        -I"$WINE_BUILD/include" -I"$WINE_SRC/include" \
        -D__WINESRC__ -DLTC_NO_PROTOTYPES -DLTC_SOURCE -D_NTSYSTEM_ \
        -D_ACRTIMP= -DWINBASEAPI= \
        -DBINDIR=\"/usr/local/bin\" -DLIBDIR=\"/usr/local/lib\" \
        -DDATADIR=\"/usr/local/share\" -DSYSTEMDLLPATH=\"\" \
        -DWINE_UNIX_LIB -DWINE_IOS=1 \
        -Dget_thread_context=ntdll_get_thread_context \
        -Dset_thread_context=ntdll_set_thread_context \
        -c "$src" -o "$OBJ_DIR/$name.o" 2>"$OBJ_DIR/$name.err"; then
        echo "OK"
        SUCCEEDED=$((SUCCEEDED + 1))
    else
        echo "FAILED"
        FAILED=$((FAILED + 1))
        FAILED_FILES="$FAILED_FILES $name"
    fi
}

CRYPTO_DIR="$REPO_ROOT/build/crypto-unix"
GNUTLS_PREFIX="$REPO_ROOT/toolchains/gnutls-ios"
compile_unixlib() {
    local src=$1 name=$2 prefix=$3
    shift 3
    echo -n "  $name... "
    if xcrun -sdk "$SDK_NAME" clang \
        -arch arm64 -isysroot "$SDK" "$MIN_FLAG" $EXTRA_CFLAGS \
        -O2 -fPIC -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing \
        -Wno-implicit-function-declaration -Wno-int-conversion \
        -include "$WINE_BUILD/include/config.h" \
        -include "$BUILD_DIR/shims/wine_ios_exit.h" \
        -I"$BUILD_DIR" -I"$BUILD_DIR/shims" \
        -I"$WINE_BUILD/include" -I"$WINE_SRC/include" \
        -D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= \
        -DWINE_UNIX_LIB -DWINE_IOS=1 \
        -D__wine_unix_call_funcs=${prefix}_unix_call_funcs \
        -D__wine_unix_call_wow64_funcs=${prefix}_unix_call_wow64_funcs \
        "$@" \
        -c "$src" -o "$OBJ_DIR/$name.o" 2>"$OBJ_DIR/$name.err"; then
        echo "OK"
        SUCCEEDED=$((SUCCEEDED + 1))
    else
        echo "FAILED"
        FAILED=$((FAILED + 1))
        FAILED_FILES="$FAILED_FILES $name"
    fi
}

SMB2_REV="7c8eee4d5766da26aceca7e7aeb7549ce8af4e0c"
SMB2_SRC="$BUILD_DIR/_libsmb2-src"
SMB2_BUILD="$BUILD_DIR/_libsmb2-build"
rm -rf "$SMB2_SRC" "$SMB2_BUILD"
git init -q "$SMB2_SRC"
git -C "$SMB2_SRC" remote add origin https://github.com/sahlberg/libsmb2.git
git -C "$SMB2_SRC" fetch -q --depth 1 origin "$SMB2_REV"
git -C "$SMB2_SRC" checkout -q FETCH_HEAD
cmake -S "$SMB2_SRC" -B "$SMB2_BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_UTILS=OFF \
    -DENABLE_LIBKRB5=OFF \
    -DENABLE_GSSAPI=OFF \
    -DENABLE_LIBDCERPC=OFF
cmake --build "$SMB2_BUILD" --parallel "$(sysctl -n hw.ncpu)"
SMB2_LIB="$(find "$SMB2_BUILD" -name 'libsmb2.a' -print -quit)"
test -n "$SMB2_LIB" -a -f "$SMB2_LIB" || { echo "FATAL: libsmb2.a missing"; exit 1; }
echo "Direct SMB library: $SMB2_LIB ($(wc -c < "$SMB2_LIB" | tr -d ' ') bytes)"

python3 "$BUILD_DIR/smb_vfs_patch.py" \
    "$WINE_SRC/dlls/ntdll/unix/file.c" \
    "$BUILD_DIR/virtual_ios.c" \
    "$OBJ_DIR/file_smb_ios.c" \
    "$OBJ_DIR/virtual_smb_ios.c"

echo "=== Building ntdll unix (iOS) ==="
compile_one "$BUILD_DIR/audio_null_ios.c" "audio_null_ios"

echo "=== Building crypto/network unixlibs ==="
"$CRYPTO_DIR/gen_gnutls_symtab.sh" > /dev/null
compile_one "$CRYPTO_DIR/gnutls_symtab_ios.c" "gnutls_symtab_ios"
compile_unixlib "$WINE_SRC/dlls/ws2_32/unixlib.c" "ws2_32_unixlib" "ws2_32" \
    -I"$WINE_SRC/dlls/ws2_32" -include "$BUILD_DIR/shims/netfilter_ios.h"
compile_unixlib "$WINE_SRC/dlls/bcrypt/gnutls.c" "bcrypt_unixlib" "bcrypt" \
    -I"$WINE_SRC/dlls/bcrypt" -I"$GNUTLS_PREFIX/include" \
    -DHAVE_GNUTLS_CIPHER_INIT=1 -DSONAME_LIBGNUTLS=\"libgnutls.so.30\" \
    -include "$CRYPTO_DIR/ios_gnutls_shim.h"
compile_unixlib "$WINE_SRC/dlls/secur32/schannel_gnutls.c" "secur32_unixlib" "secur32" \
    -I"$WINE_SRC/dlls/secur32" -I"$GNUTLS_PREFIX/include" \
    -DSONAME_LIBGNUTLS=\"libgnutls.so.30\" \
    -include "$CRYPTO_DIR/ios_gnutls_shim.h"
compile_unixlib "$BUILD_DIR/dwrite_freetype_ios.c" "dwrite_unixlib" "dwrite" \
    -I"$WINE_SRC/dlls/dwrite" -I"$REPO_ROOT/research/freetype/include" \
    -I"$REPO_ROOT/wine/build-arm64ec/include"
compile_unixlib "$CRYPTO_DIR/crypt32_unixlib_ios.c" "crypt32_unixlib" "crypt32" \
    -I"$WINE_SRC/dlls/crypt32" -I"$GNUTLS_PREFIX/include" \
    -include "$CRYPTO_DIR/ios_gnutls_shim.h"
compile_one "$BUILD_DIR/nsi_unixlib_ios.c" "nsi_unixlib_ios"
compile_one "$BUILD_DIR/xinput_ios.c" "xinput_ios"

for src in $WINE_SRC/dlls/ntdll/unix/*.c; do
    name=$(basename "$src" .c)
    case "$name" in
        loader)
            compile_one "$BUILD_DIR/loader_ios.c" "loader"
            ;;
        process)
            compile_one "$BUILD_DIR/process_ios.c" "process"
            ;;
        server)
            compile_one "$BUILD_DIR/server_ios.c" "server"
            ;;
        env)
            compile_one "$BUILD_DIR/env_ios.c" "env"
            ;;
        cdrom)
            compile_one "$BUILD_DIR/cdrom_stub.c" "cdrom"
            ;;
        file)
            compile_one "$OBJ_DIR/file_smb_ios.c" "file"
            ;;
        virtual)
            compile_one "$OBJ_DIR/virtual_smb_ios.c" "virtual"
            ;;
        signal_arm64)
            compile_one "$BUILD_DIR/signal_arm64_ios.c" "signal_arm64"
            ;;
        thread)
            compile_one "$BUILD_DIR/thread_ios.c" "thread"
            ;;
        *)
            compile_one "$src" "$name"
            ;;
    esac
done

echo ""
echo "Results: $SUCCEEDED succeeded, $FAILED failed"
if [ -n "$FAILED_FILES" ]; then
    echo "Failed:$FAILED_FILES"
fi

test -f "$OBJ_DIR/file.o" -a -f "$OBJ_DIR/virtual.o" || { echo "FATAL: SMB-patched Wine objects missing"; exit 1; }

echo ""
echo "=== Building libntdll_unix + libsmb2 ==="
ar rcs "$OBJ_DIR/libntdll_base.a" \
    "$OBJ_DIR/audio_null_ios.o" "$OBJ_DIR/nsi_unixlib_ios.o" "$OBJ_DIR/xinput_ios.o" \
    "$OBJ_DIR/gnutls_symtab_ios.o" "$OBJ_DIR/ws2_32_unixlib.o" \
    "$OBJ_DIR/bcrypt_unixlib.o" "$OBJ_DIR/secur32_unixlib.o" "$OBJ_DIR/crypt32_unixlib.o" \
    "$OBJ_DIR/dwrite_unixlib.o" \
    "$OBJ_DIR/cdrom.o" "$OBJ_DIR/debug.o" "$OBJ_DIR/env.o" "$OBJ_DIR/file.o" \
    "$OBJ_DIR/loader.o" "$OBJ_DIR/loadorder.o" "$OBJ_DIR/process.o" "$OBJ_DIR/registry.o" \
    "$OBJ_DIR/security.o" "$OBJ_DIR/serial.o" "$OBJ_DIR/server.o" \
    "$OBJ_DIR/signal_arm.o" "$OBJ_DIR/signal_arm64.o" "$OBJ_DIR/signal_i386.o" "$OBJ_DIR/signal_x86_64.o" \
    "$OBJ_DIR/socket.o" "$OBJ_DIR/sync.o" "$OBJ_DIR/syscall.o" "$OBJ_DIR/system.o" \
    "$OBJ_DIR/tape.o" "$OBJ_DIR/thread.o" "$OBJ_DIR/virtual.o"

/usr/bin/libtool -static -o "$APP_LIB" "$OBJ_DIR/libntdll_base.a" "$SMB2_LIB"
ranlib "$APP_LIB"
echo "libntdll_unix+libsmb2.a: $(wc -c < "$APP_LIB" | tr -d ' ') bytes"
echo "Done!"
