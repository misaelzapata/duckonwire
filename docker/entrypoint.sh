#!/bin/sh
# Isolated local Docker demo: random accounts, private persistent keys, verified TLS.
set -eu
export LC_ALL=C
umask 077
root=${DUCKONWIRE_ROOT:-/var/lib/duckonwire}
install=${DUCKONWIRE_INSTALL_ROOT:-/opt/duckonwire}
fail() { printf '%s\n' "DuckOnWire Docker: $1" >&2; exit 1; }
[ -d "$root" ] && [ ! -L "$root" ] || fail 'the persistent root must be a regular writable directory'
mkdir -p "$root/data" "$root/.runtime"
chmod 700 "$root/.runtime"
exec 9>"$root/.runtime/docker.lock"
flock -n 9 || fail 'another container owns this volume'

store_password() {
    file=$1
    password=$2
    [ ! -L "$file" ] || fail 'password files must not be symlinks'
    if [ -z "$password" ]; then
        if [ -f "$file" ]; then password=$(cat "$file"); else password=$(openssl rand -hex 32); fi
    fi
    [ "${#password}" -ge 32 ] && [ "${#password}" -le 1024 ] || fail 'passwords need 32 to 1024 printable ASCII characters'
    case "$password" in *[!\ -\~]*) fail 'passwords need printable ASCII characters' ;; esac
    printf '%s\n' "$password" >"$file.tmp"
    chmod 600 "$file.tmp"
    mv "$file.tmp" "$file"
}
store_password "$root/.runtime/writer.password" "${DUCKONWIRE_PASSWORD:-}"
store_password "$root/.runtime/reader.password" "${DUCKONWIRE_READER_PASSWORD:-}"
writer=$("$install/bin/duckonwire-password" <"$root/.runtime/writer.password")
reader=$("$install/bin/duckonwire-password" <"$root/.runtime/reader.password")
printf 'database\tduckonwire\nuser\twriter\twrite\t%s\nuser\treader\tread\t%s\n' "$writer" "$reader" >"$root/.runtime/access.tsv.tmp"
chmod 600 "$root/.runtime/access.tsv.tmp"
mv "$root/.runtime/access.tsv.tmp" "$root/.runtime/access.tsv"
writer_pass=$(sed 's/\\/\\\\/g; s/:/\\:/g' "$root/.runtime/writer.password")
reader_pass=$(sed 's/\\/\\\\/g; s/:/\\:/g' "$root/.runtime/reader.password")
for host in 127.0.0.1 localhost; do
    printf '%s:%s:duckonwire:writer:%s\n' "$host" "${PGPORT:-9496}" "$writer_pass"
    printf '%s:%s:duckonwire:reader:%s\n' "$host" "${PGPORT:-9496}" "$reader_pass"
done >"$root/.runtime/pgpass.tmp"
chmod 600 "$root/.runtime/pgpass.tmp"
mv "$root/.runtime/pgpass.tmp" "$root/.runtime/pgpass"
unset writer_pass reader_pass
unset DUCKONWIRE_PASSWORD DUCKONWIRE_READER_PASSWORD writer reader password

tls="$root/.runtime/tls"
[ ! -L "$tls" ] || fail 'TLS directory must not be a symlink'
if [ ! -d "$tls" ]; then
    temp="$root/.runtime/tls-new-$$"
    mkdir "$temp"
    trap 'rm -rf "$temp"' EXIT HUP INT TERM
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -subj '/CN=DuckOnWire local Docker CA' -addext 'basicConstraints=critical,CA:TRUE' \
        -keyout "$temp/ca-key.pem" -out "$temp/ca.pem" >/dev/null 2>&1
    openssl req -new -newkey rsa:2048 -nodes -sha256 -subj /CN=localhost \
        -keyout "$temp/server.key" -out "$temp/server.csr" >/dev/null 2>&1
    printf '%s\n' 'basicConstraints=critical,CA:FALSE' \
        'keyUsage=critical,digitalSignature,keyEncipherment' 'extendedKeyUsage=serverAuth' \
        'subjectAltName=DNS:localhost,IP:127.0.0.1' >"$temp/server.ext"
    openssl x509 -req -sha256 -days 365 -in "$temp/server.csr" -CA "$temp/ca.pem" \
        -CAkey "$temp/ca-key.pem" -CAcreateserial -extfile "$temp/server.ext" \
        -out "$temp/server.crt" >/dev/null 2>&1
    rm "$temp/server.csr" "$temp/server.ext" "$temp/ca.srl"
    chmod 600 "$temp"/*
    mv "$temp" "$tls"
    trap - EXIT HUP INT TERM
fi
for file in ca.pem ca-key.pem server.crt server.key; do
    [ -f "$tls/$file" ] && [ ! -L "$tls/$file" ] || fail 'incomplete or invalid TLS files'
done
openssl verify -CAfile "$tls/ca.pem" -verify_hostname localhost "$tls/server.crt" >/dev/null 2>&1 \
    || fail 'the local TLS certificate is invalid or expired'

# The inherited lock prevents two entrypoints owning one volume. After a crash,
# only this entrypoint's old descriptor can be removed; the database is preserved.
state="$root/.runtime/native.json"
if [ -e "$state" ] || [ -L "$state" ]; then
    [ -f "$root/.runtime/docker-managed" ] && [ -f "$state" ] && [ ! -L "$state" ] \
        || fail 'an unmanaged runtime descriptor already exists'
    pid=$(sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p' "$state")
    if [ -n "$pid" ] && [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" = "$install/bin/duckonwire-server" ]; then
        fail 'a native owner is still running'
    fi
    rm "$state"
fi
printf '%s\n' 'DuckOnWire Docker volume' >"$root/.runtime/docker-managed"
exec "$install/bin/duckonwire-server" --root "$root" --pg-host 0.0.0.0 \
    --pg-port "${PGPORT:-9496}" --port "${DUCKONWIRE_QUACK_PORT:-9495}" \
    --access-file .runtime/access.tsv --tls-cert .runtime/tls/server.crt \
    --tls-key .runtime/tls/server.key "$@"
