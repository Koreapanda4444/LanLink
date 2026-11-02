set -eu

openssl_binary="$1"
test_binary="$2"
fixture_dir="$(mktemp -d /tmp/lanlink-network-client.XXXXXX)"
case "$fixture_dir" in
    /tmp/lanlink-network-client.*) trap 'rm -rf -- "$fixture_dir"' EXIT ;;
    *) exit 1 ;;
esac

"$openssl_binary" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
    -nodes -days 1 -subj /CN=127.0.0.1 \
    -addext subjectAltName=IP:127.0.0.1 \
    -keyout "$fixture_dir/key.pem" -out "$fixture_dir/cert.pem" >/dev/null 2>&1
printf 'lanlink-local-integration-token-2026\n' > "$fixture_dir/auth.token"
SSL_CERT_FILE="$fixture_dir/cert.pem" "$test_binary" "$fixture_dir"
