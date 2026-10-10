#!/bin/sh
# Install an immutable public native bundle without root or PATH changes.
set -eu
umask 077

version=0.3.0
prefix=${HOME:?HOME must be set}/.local/share/duckonwire
base_url=
platform=
prepare_only=false
stage=
owned_prefix=false

usage() {
    cat <<'HELP'
Usage: sh install.sh [options]
  --prefix DIR       New installation directory (default: ~/.local/share/duckonwire)
  --version VERSION  Published release, with optional v prefix (default: 0.3.0)
  --base-url URL     Release asset directory; defaults to this version on GitHub
  --platform TARGET  Prepare linux-x86_64-v3 files without running them or checking this host
  --help             Show this help

Automatic installation supports Linux x86-64-v3 with glibc 2.39+,
libcurl 4 and OpenSSL 3, as used by the Ubuntu 24.04 release bundle.
For other hosts, use Docker or build from source. Existing paths are preserved.
HELP
}
fail() { printf 'DuckOnWire install: %s\n' "$*" >&2; exit 1; }
cleanup() {
    if [ "$owned_prefix" = true ] && [ ! -L "$prefix" ]; then
        if [ -f "$prefix/.duckonwire-install-owner" ] &&
           [ "$(cat "$prefix/.duckonwire-install-owner")" = "$stage" ]; then
            rm -rf "$prefix"
        else
            # A failed marker write may leave the directory we just reserved.
            # Remove it only if empty, preserving anything another actor added.
            rmdir "$prefix" 2>/dev/null || :
        fi
    fi
    if [ -n "$stage" ]; then rm -rf "$stage"; fi
}
trap cleanup 0
trap 'exit 1' 1 2 15

while [ "$#" -gt 0 ]; do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --prefix|--version|--base-url|--platform)
            [ "$#" -ge 2 ] || fail "Missing value for $1"
            case $1 in
                --prefix) prefix=$2 ;;
                --version) version=$2 ;;
                --base-url) base_url=$2 ;;
                --platform) platform=$2; prepare_only=true ;;
            esac
            shift 2 ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
case $version in v*) version=${version#v} ;; esac
case $version in ''|*[!0-9A-Za-z.+-]*) fail 'Invalid release version' ;; esac
printf '%s\n' "$version" | awk '/^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+-]+)?$/ { good=1 } END { exit !good }' ||
    fail 'Use a release version such as 0.3.0'

for command in curl tar awk mktemp; do
    command -v "$command" >/dev/null 2>&1 || fail "Required command is missing: $command"
done
if command -v sha256sum >/dev/null 2>&1; then
    digest() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    digest() { shasum -a 256 "$1" | awk '{print $1}'; }
elif command -v openssl >/dev/null 2>&1; then
    digest() { openssl dgst -sha256 "$1" | awk '{print $NF}'; }
else
    fail 'Install sha256sum, shasum or openssl to verify the release'
fi

if [ "$prepare_only" = false ]; then
    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] ||
        fail 'No native bundle for this host. Use Docker or build from source.'
    platform=linux-x86_64-v3
    command -v getconf >/dev/null 2>&1 || fail 'getconf is needed to check glibc compatibility'
    libc=$(getconf GNU_LIBC_VERSION 2>/dev/null) || fail 'The bundle requires glibc 2.39+; use Docker or source'
    printf '%s\n' "$libc" | awk '$1=="glibc" { split($2,v,"."); good=(v[1]>2 || (v[1]==2 && v[2]>=39)) } END { exit !good }' ||
        fail 'The bundle requires glibc 2.39+; use Docker or build from source'
    # Linux uses pni for SSE3 and may use abm for LZCNT. Its AVX flags also
    # reflect the OS support needed to use the extended register state.
    awk '
      /^flags[[:space:]]*:/ {
        for (i=3;i<=NF;i++) flags[$i]=1
        good=(flags["cx16"] && flags["lahf_lm"] && flags["popcnt"] &&
          (flags["sse3"] || flags["pni"]) && flags["ssse3"] &&
          flags["sse4_1"] && flags["sse4_2"] && flags["avx"] && flags["avx2"] &&
          flags["bmi1"] && flags["bmi2"] && flags["f16c"] && flags["fma"] &&
          (flags["lzcnt"] || flags["abm"]) && flags["movbe"] && flags["xsave"])
        found=1; exit
      }
      END { exit !(found && good) }
    ' /proc/cpuinfo 2>/dev/null || fail 'The native bundle requires an x86-64-v3 CPU; use Docker or source'
fi
[ "$platform" = linux-x86_64-v3 ] || fail 'Only the linux-x86_64-v3 native bundle is published'
case $prefix in ''|/) fail 'Choose a new installation directory' ;; esac
while [ "${prefix%/}" != "$prefix" ]; do prefix=${prefix%/}; done
case $prefix in /*) ;; *) prefix=$(pwd)/$prefix ;; esac
parent=${prefix%/*}
leaf=${prefix##*/}
case $leaf in ''|.|..) fail 'Choose a new installation directory' ;; esac
[ ! -e "$prefix" ] && [ ! -L "$prefix" ] || fail "Installation path already exists; preserved: $prefix"
mkdir -p "$parent"
parent=$(CDPATH= cd "$parent" && pwd -P)
prefix=$parent/$leaf
[ ! -e "$prefix" ] && [ ! -L "$prefix" ] || fail "Installation path already exists; preserved: $prefix"
stage=$(mktemp -d "$parent/.duckonwire-install.XXXXXX") || fail 'Cannot create private staging directory'
name=duckonwire-$version-$platform
archive=$name.tar.gz
manifest=$name.manifest.json
[ -n "$base_url" ] || base_url=https://github.com/misaelzapata/duckonwire/releases/download/v$version
base_url=${base_url%/}
case $base_url in *'@'*|*'?'*|*'#'*) fail 'Use a public asset directory URL without credentials, query or fragment' ;; esac
case $base_url in
    https://*) protocol=https ;;
    http://*) protocol=http ;;
    file://*) protocol=file ;;
    *) fail 'Asset URL must use https, http or file' ;;
esac
download() {
    curl --fail --silent --show-error --location --retry 2 --connect-timeout 15 --max-time 600 \
        --proto "=$protocol" --proto-redir "=$protocol" --output "$stage/$1" "$base_url/$1" ||
        fail "Could not download $1"
}
download "$name.sha256"
download "$manifest"
download "$archive"
# Never pass untrusted checksum paths to sha256sum --check.
awk -v a="$archive" -v m="$manifest" '
  NF!=2 || length($1)!=64 || $1~/[^0-9a-fA-F]/ { bad=1; next }
  { n=$2; sub(/^\*/,"",n); if(n==a) ac++; else if(n==m) mc++; else bad=1 }
  END { exit (bad || ac!=1 || mc!=1) }
' "$stage/$name.sha256" || fail 'The release checksum file is invalid'
for file in "$archive" "$manifest"; do
    expected=$(awk -v f="$file" '{ n=$2; sub(/^\*/,"",n); if(n==f) print tolower($1) }' "$stage/$name.sha256")
    [ "$(digest "$stage/$file")" = "$expected" ] || fail "SHA256 verification failed: $file"
done

tar -tzf "$stage/$archive" > "$stage/entries" || fail 'Cannot list the native archive'
awk -v root="$name" '
  {
    path=$0; sub(/\/$/,"",path)
    if(path !~ /^[A-Za-z0-9_.\/+\-]+$/ || path ~ /\/\// ||
       path ~ /(^|\/)\.\.?($|\/)/ || (path!=root && index(path,root"/")!=1) || seen[path]++) bad=1
    count++
  }
  END { exit (bad || !count) }
' "$stage/entries" || fail 'Archive has an unsafe, repeated or unexpected path'
tar -tvzf "$stage/$archive" > "$stage/types" || fail 'Cannot inspect native archive entries'
awk 'substr($1,1,1)!="-" && substr($1,1,1)!="d" { bad=1 } $1~/[sStT]/ { bad=1 } END { exit bad }' "$stage/types" ||
    fail 'Archive links, devices or special file permissions are not supported'
mkdir "$stage/unpack"
tar -xzf "$stage/$archive" -C "$stage/unpack" || fail 'Cannot extract the verified archive'
bundle=$stage/unpack/$name
for file in bin/duckonwire-server bin/duckonwire-password lib/libduckdb.so; do
    [ -f "$bundle/$file" ] && [ ! -L "$bundle/$file" ] || fail "Native package is missing $file"
done
[ -x "$bundle/bin/duckonwire-server" ] || fail 'The native server is not executable'
if [ "$prepare_only" = false ]; then
    "$bundle/bin/duckonwire-server" --help > "$stage/runtime-help" 2>&1 ||
        fail 'The bundle cannot run here. It needs Ubuntu 24.04-compatible libcurl 4/OpenSSL 3; use Docker or source.'
fi
mkdir -p "$bundle/share/duckonwire"
for file in release.manifest.json release.sha256; do
    [ ! -e "$bundle/share/duckonwire/$file" ] || fail 'Archive contains a conflicting installation receipt'
done
cp "$stage/$manifest" "$bundle/share/duckonwire/release.manifest.json"
cp "$stage/$name.sha256" "$bundle/share/duckonwire/release.sha256"
# mkdir reserves the destination atomically: even a concurrent installer cannot
# make mv put our bundle inside a pre-existing user directory.
mkdir "$prefix" || fail "Installation path appeared during download; preserved: $prefix"
owned_prefix=true
printf '%s\n' "$stage" > "$prefix/.duckonwire-install-owner"
for file in "$bundle"/.[!.]* "$bundle"/..?* "$bundle"/*; do
    [ -e "$file" ] || continue
    mv "$file" "$prefix/"
done
rm "$prefix/.duckonwire-install-owner"
owned_prefix=false
printf 'DuckOnWire %s installed in %s\n' "$version" "$prefix"
if [ "$prepare_only" = true ]; then
    printf 'Linux bundle prepared; host compatibility and execution were not tested.\n'
fi
printf 'Start it on a compatible Linux host:\n  "%s/bin/duckonwire-server" --root "%s"\n' "$prefix" "$prefix"
