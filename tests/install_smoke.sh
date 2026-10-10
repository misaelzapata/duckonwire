#!/bin/sh
# Small installer fixtures, plus an optional already-downloaded public release.
set -eu
umask 077
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd -P)
installer=$repo/scripts/install.sh
fixture_parent=${DUCKONWIRE_TEST_TMPDIR:-${TMPDIR:-/tmp}}
work=$(mktemp -d "$fixture_parent/duckonwire-install-tests.XXXXXX")
trap 'rm -rf "$work"' 0
trap 'exit 1' 1 2 15
checks=0
version=0.0.0-test
name=duckonwire-$version-linux-x86_64-v3
assets=$work/assets
mkdir "$assets"
if command -v sha256sum >/dev/null 2>&1; then
    digest() { sha256sum "$1" | awk '{print $1}'; }
else
    digest() { shasum -a 256 "$1" | awk '{print $1}'; }
fi
pass() { checks=$((checks+1)); }
checksums() {
    printf '%s  %s\n%s  %s\n' "$(digest "$assets/$name.tar.gz")" "$name.tar.gz" \
        "$(digest "$assets/$name.manifest.json")" "$name.manifest.json" > "$assets/$name.sha256"
}
package() {
    rm -rf "$work/payload"
    mkdir -p "$work/payload/$name/bin" "$work/payload/$name/lib"
    printf '#!/bin/sh\nprintf "fixture server\\n"\n' > "$work/payload/$name/bin/duckonwire-server"
    chmod 755 "$work/payload/$name/bin/duckonwire-server"
    printf 'fixture password utility\n' > "$work/payload/$name/bin/duckonwire-password"
    printf 'fixture library\n' > "$work/payload/$name/lib/libduckdb.so"
    printf '{"version":"%s","platform":"linux-x86_64-v3"}\n' "$version" > "$assets/$name.manifest.json"
    tar -czf "$assets/$name.tar.gz" -C "$work/payload" "$name"
    checksums
}
install() {
    sh "$installer" --version "$version" --platform linux-x86_64-v3 \
        --base-url "file://$assets" --prefix "$1" > "$work/stdout" 2> "$work/stderr"
}
reject() {
    target=$1
    if install "$target"; then printf 'Expected installer rejection\n' >&2; exit 1; fi
    [ ! -e "$target" ] && [ ! -L "$target" ]
    pass
}
sh -n "$installer"
sh "$installer" --help > "$work/help"
grep -q -- '--prefix' "$work/help"
pass
package
install "$work/install with spaces"
[ -x "$work/install with spaces/bin/duckonwire-server" ]
[ -f "$work/install with spaces/share/duckonwire/release.manifest.json" ]
cmp "$assets/$name.manifest.json" "$work/install with spaces/share/duckonwire/release.manifest.json"
pass
printf 'preserve\n' > "$work/install with spaces/sentinel"
if install "$work/install with spaces"; then exit 1; fi
[ "$(cat "$work/install with spaces/sentinel")" = preserve ]
pass
mkdir "$work/empty"
if install "$work/empty"; then exit 1; fi
[ -d "$work/empty" ] && [ -z "$(ls -A "$work/empty")" ]
pass
ln -s "$work/install with spaces" "$work/symlink"
if install "$work/symlink"; then exit 1; fi
[ -L "$work/symlink" ] && [ "$(cat "$work/install with spaces/sentinel")" = preserve ]
pass
printf corrupt >> "$assets/$name.tar.gz"
reject "$work/bad-archive-hash"
package
printf corrupt >> "$assets/$name.manifest.json"
reject "$work/bad-manifest-hash"
package
printf '%s  ../../outside\n' "$(digest "$assets/$name.tar.gz")" >> "$assets/$name.sha256"
reject "$work/bad-checksum-path"
package
cat "$assets/$name.sha256" >> "$work/duplicate"
cat "$work/duplicate" >> "$assets/$name.sha256"
reject "$work/duplicate-checksum"
package
ln -s /tmp "$work/payload/$name/escape"
tar -czf "$assets/$name.tar.gz" -C "$work/payload" "$name"
checksums
reject "$work/archive-link"
package
tar -czf "$assets/$name.tar.gz" -C "$work/payload" "$name" "$name/bin/duckonwire-server"
checksums
reject "$work/repeated-archive-path"
package
mv "$work/payload/$name" "$work/payload/unexpected-root"
tar -czf "$assets/$name.tar.gz" -C "$work/payload" unexpected-root
checksums
reject "$work/wrong-root"
package
rm "$work/payload/$name/lib/libduckdb.so"
tar -czf "$assets/$name.tar.gz" -C "$work/payload" "$name"
checksums
reject "$work/missing-library"
if sh "$installer" --platform arm64 --prefix "$work/unsupported" > "$work/stdout" 2> "$work/stderr"; then exit 1; fi
[ ! -e "$work/unsupported" ]; pass
if sh "$installer" --version '../bad' --prefix "$work/bad-version" > "$work/stdout" 2> "$work/stderr"; then exit 1; fi
[ ! -e "$work/bad-version" ]; pass
if sh "$installer" --unknown > "$work/stdout" 2> "$work/stderr"; then exit 1; fi
pass
if sh "$installer" --platform linux-x86_64-v3 --base-url "file://$work/missing-assets" \
   --prefix "$work/download-failure" > "$work/stdout" 2> "$work/stderr"; then exit 1; fi
[ ! -e "$work/download-failure" ]; pass
for leftover in "$work"/.duckonwire-install.*; do [ ! -e "$leftover" ]; done
pass
real=false
if [ "$#" -gt 0 ]; then
    [ "$#" -eq 2 ] && [ "$1" = --real-assets ] || { printf 'Usage: sh tests/install_smoke.sh [--real-assets DIR]\n' >&2; exit 1; }
    real_assets=$(CDPATH= cd "$2" && pwd -P)
    sh "$installer" --version 0.3.0 --platform linux-x86_64-v3 --base-url "file://$real_assets" \
        --prefix "$work/real-release" > "$work/real-stdout" 2> "$work/real-stderr"
    archive=$real_assets/duckonwire-0.3.0-linux-x86_64-v3.tar.gz
    tar -xOzf "$archive" duckonwire-0.3.0-linux-x86_64-v3/bin/duckonwire-server > "$work/expected-server"
    cmp "$work/expected-server" "$work/real-release/bin/duckonwire-server"
    cmp "$real_assets/duckonwire-0.3.0-linux-x86_64-v3.manifest.json" "$work/real-release/share/duckonwire/release.manifest.json"
    [ -f "$work/real-release/lib/libduckdb.so" ] && [ -f "$work/real-release/README.md" ]
    real=true
    pass
fi
printf '{"passed":true,"checks":%s,"real_native_bundle_prepared":%s,"native_Linux_execution_tested":false}\n' "$checks" "$real"
