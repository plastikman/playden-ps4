#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
root=$(cd -- "$script_dir/../.." && pwd -P)
sdk="$root/deps/orbis-sdk-v1/sdk"
compat="$root/deps/orbis-sdk-v1/orbis-compat"
toolchain="$compat/cmake/ps4-openorbis.cmake"
deps="$script_dir/.deps"
downloads="$deps/downloads"
sources="$deps/src"
build="$deps/build"
prefix="$deps/ps4"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

curl_version=8.22.0
curl_sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
curl_patch="$script_dir/patches/curl-ps4-native-nonblock.patch"
curl_patch_sha256=81e7dfc99e66110201a60199eadf5da403da43169fe8e439ec443ad3956f4fc5
mbedtls_version=3.6.6
mbedtls_sha256=8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
ca_version=2026-09-25
ca_sha256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

[[ -f "$sdk/link.x" && -f "$toolchain" ]] || {
    echo "paired PS4 SDK missing; expected $sdk and $toolchain" >&2
    exit 1
}
actual_patch_sha256=$(sha256sum "$curl_patch" | awk '{print $1}')
[[ "$actual_patch_sha256" == "$curl_patch_sha256" ]] || {
    echo "SHA-256 mismatch for $curl_patch: $actual_patch_sha256" >&2
    exit 1
}
export OO_PS4_TOOLCHAIN="$sdk" ORBIS_COMPAT_DIR="$compat"
mkdir -p "$downloads" "$sources" "$build" "$prefix"

fetch() {
    local url=$1 output=$2 expected=$3 actual
    if [[ ! -f "$output" ]]; then
        curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$output"
    fi
    actual=$(sha256sum "$output" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || {
        echo "SHA-256 mismatch for $output: $actual" >&2
        exit 1
    }
}

fetch "https://curl.se/download/curl-$curl_version.tar.xz" \
    "$downloads/curl-$curl_version.tar.xz" "$curl_sha256"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2" \
    "$downloads/mbedtls-$mbedtls_version.tar.bz2" "$mbedtls_sha256"
fetch "https://curl.se/ca/cacert-$ca_version.pem" "$downloads/cacert.pem" "$ca_sha256"

if [[ ! -d "$sources/mbedtls-$mbedtls_version" ]]; then
    tar -xjf "$downloads/mbedtls-$mbedtls_version.tar.bz2" -C "$sources"
fi
if [[ ! -d "$sources/curl-$curl_version" ]]; then
    tar -xJf "$downloads/curl-$curl_version.tar.xz" -C "$sources"
fi
if ! grep -q 'PLAYDEN_PS4_NATIVE_NONBLOCK' "$sources/curl-$curl_version/lib/curl_setup.h"; then
    patch --fuzz=0 -d "$sources/curl-$curl_version" -p1 < "$curl_patch"
fi

mbedtls_src="$sources/mbedtls-$mbedtls_version"
for option in MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD; do
    python3 "$mbedtls_src/scripts/config.py" --file "$mbedtls_src/include/mbedtls/mbedtls_config.h" set "$option"
done

cmake -S "$mbedtls_src" -B "$build/mbedtls-ps4" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
cmake --build "$build/mbedtls-ps4" --parallel "$jobs"
cmake --install "$build/mbedtls-ps4"

cmake -U CMAKE_C_FLAGS -U HAVE_SETSOCKOPT_SO_NONBLOCK \
    -S "$sources/curl-$curl_version" -B "$build/curl-ps4" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    '-DCMAKE_C_FLAGS_RELEASE=-DSOL_IP=IPPROTO_IP -DPLAYDEN_PS4_NATIVE_NONBLOCK=1' \
    -DHAVE_STROPTS_H:INTERNAL=0 \
    -DHAVE_GETADDRINFO:INTERNAL=1 \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
    -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF -DBUILD_SHARED_LIBS=OFF -DHTTP_ONLY=OFF \
    -DCURL_USE_MBEDTLS=ON -DCURL_USE_CMAKECONFIG=ON -DMbedTLS_DIR="$prefix/lib/cmake/MbedTLS" \
    -DENABLE_IPV6=OFF -DENABLE_THREADED_RESOLVER=OFF -DCURL_DISABLE_SOCKETPAIR=ON \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_ZLIB=OFF -DCURL_DISABLE_NETRC=ON \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON \
    -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON \
    -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_TELNET=ON \
    -DCURL_DISABLE_TFTP=ON -DCURL_ENABLE_SMB=OFF
cmake --build "$build/curl-ps4" --parallel "$jobs"
cmake --install "$build/curl-ps4"
install -m 0644 "$downloads/cacert.pem" "$prefix/cacert.pem"

echo "PS4 network dependency prefix: $prefix"
