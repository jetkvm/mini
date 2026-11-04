#!/usr/bin/env bash
# Regenerates the fixtures of the signature host test (test_ota_signature.c)
# with GnuPG and throwaway keys:
#
#   root.asc             a release key as a keyserver serves it: a primary key
#                        that certifies, signing subkey 1 without an expiry and
#                        signing subkey 2 that expires after one year
#   root-revoked.asc     the same key after the primary key revoked subkey 1
#   root-key-revoked.asc the same key after the primary key revoked itself
#   root-rogue.asc       one armored block with another key before the release key
#   other.asc            the other key alone
#   root-extended.asc    the key with two subkeys and signing subkey 3, which
#                        expired after one year until the primary key
#                        extended that to three years. A keyserver keeps both
#                        binding signatures; the older one comes first
#   root-extended-newest-first.asc  the same with the newer binding first
#   root.h               the fingerprints, the subkeys' times, and for subkeys
#                        1 and 2 the place in root.asc of a base64 character
#                        that lies within the subkey's Ed25519 point
#   image.bin            the signed data
#   image.bin.sig        by subkey 1 (GnuPG's default hash)
#   image.bin.sha256.sig by subkey 1 over SHA-256
#   image.bin.subkey2.sig by subkey 2
#   image.bin.subkey3.sig by subkey 3
#   image.bin.other.sig  by the other key
#
# real/ holds what the real services serve, fetched once: the JetKVM release
# key from keys.openpgp.org and from keyserver.ubuntu.com, and the signature
# of the JetKVM app 0.5.9 from update.jetkvm.com. jetkvm_app-0.5.9.digest is
# the hash that this signature signs: SHA-512 of the app (31 MB, not kept
# here) followed by the signature's trailer.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export GNUPGHOME="$(mktemp -d)"
work="$(mktemp -d)"
trap 'gpgconf --kill all; rm -rf "$GNUPGHOME" "$work"' EXIT
gpg_batch=(gpg --batch --quiet --pinentry-mode loopback --passphrase "")
fingerprints() { gpg --list-keys --with-colons "$1" | awk -F: '/^fpr:/ { print $10 }'; }

"${gpg_batch[@]}" --quick-gen-key "OTA test release root" ed25519 cert never
root="$(fingerprints "OTA test release root" | sed -n 1p)"
"${gpg_batch[@]}" --quick-add-key "$root" ed25519 sign never
"${gpg_batch[@]}" --quick-add-key "$root" ed25519 sign 1y
subkey1="$(fingerprints "$root" | sed -n 2p)"
subkey2="$(fingerprints "$root" | sed -n 3p)"
"${gpg_batch[@]}" --quick-gen-key "OTA test other key" ed25519 sign never
other="$(fingerprints "OTA test other key" | sed -n 1p)"

python3 -c 'import sys; sys.stdout.write("".join(f"firmware image line {i:05d}\n" for i in range(300)))' > "$here/image.bin"
sign() { "${gpg_batch[@]}" --yes --local-user "$1!" "${@:3}" --output "$here/$2" --detach-sign "$here/image.bin"; }
sign "$subkey1" image.bin.sig
sign "$subkey1" image.bin.sha256.sig --digest-algo SHA256
sign "$subkey2" image.bin.subkey2.sig
sign "$other" image.bin.other.sig

gpg --armor --export "$root" > "$here/root.asc"
gpg --armor --export "$other" > "$here/other.asc"
gpg --armor --export "$other" "$root" > "$here/root-rogue.asc"

created() { gpg --list-keys --with-colons "$root" | awk -F: -v n="$1" '/^sub:/ { if (++i == n) print $6 }'; }

# Subkey 3, in a copy of the keyring, so that the files above and the
# revocations below are about the key with two subkeys. GnuPG replaces a
# binding signature when it changes the expiry; the key with both bindings
# comes from importing the state with the older one and then the state with
# the newer one, and newest_first.py writes the other order. The two bindings
# need different creation times, which have a resolution of one second.
(
  cp -r "$GNUPGHOME" "$work/home"
  export GNUPGHOME="$work/home"
  trap 'gpgconf --kill all' EXIT
  "${gpg_batch[@]}" --quick-add-key "$root" ed25519 sign 1y
  fingerprints "$root" | sed -n 4p > "$work/subkey3"
  created 3 > "$work/subkey3-created"
  sign "$(cat "$work/subkey3")" image.bin.subkey3.sig
  gpg --export "$root" > "$work/one-year.gpg"
  sleep 2
  "${gpg_batch[@]}" --quick-set-expire "$root" 3y "$(cat "$work/subkey3")"
  gpg --export "$root" > "$work/three-years.gpg"
)
mkdir -m 700 "$work/merged"
gpg --homedir "$work/merged" --batch --quiet --import "$work/one-year.gpg"
gpg --homedir "$work/merged" --batch --quiet --import "$work/three-years.gpg"
gpg --homedir "$work/merged" --armor --export "$root" > "$here/root-extended.asc"
python3 "$here/newest_first.py" "$here/root-extended.asc" > "$here/root-extended-newest-first.asc"
# One binding for subkeys 1 and 2 each, and two for subkey 3.
for file in root-extended.asc root-extended-newest-first.asc; do
  [ "$(gpg --list-packets "$here/$file" | grep -c 'sigclass 0x18')" = 4 ] ||
    { echo "$file does not have both bindings of subkey 3" >&2; exit 1; }
done
subkey3="$(cat "$work/subkey3")"

cat > "$here/root.h" <<EOF
/* Generated by make_fixtures.sh. */
#define FIXTURE_ROOT_FINGERPRINT "$root"
#define FIXTURE_OTHER_FINGERPRINT "$other"
#define FIXTURE_SUBKEY1_FINGERPRINT "$subkey1"
#define FIXTURE_SUBKEY2_FINGERPRINT "$subkey2"
#define FIXTURE_SUBKEY3_FINGERPRINT "$subkey3"
#define FIXTURE_SUBKEY2_CREATED $(created 2)U
#define FIXTURE_SUBKEY3_CREATED $(cat "$work/subkey3-created")U
#define FIXTURE_SUBKEY1_POINT_AT $(python3 "$here/point_at.py" "$here/root.asc" 1)U
#define FIXTURE_SUBKEY2_POINT_AT $(python3 "$here/point_at.py" "$here/root.asc" 2)U
EOF

# The primary key revokes subkey 1: "key 1" selects it, revkey asks for a
# confirmation, a reason (0), a description (an empty line) and a second
# confirmation.
printf 'key 1\nrevkey\ny\n0\n\ny\nsave\n' |
  gpg --batch --quiet --pinentry-mode loopback --passphrase "" --command-fd 0 --status-fd 2 --edit-key "$root" 2>/dev/null
gpg --armor --export "$root" > "$here/root-revoked.asc"
gpg --list-keys --with-colons "$root" | grep -q '^sub:r:' || { echo "subkey 1 is not revoked" >&2; exit 1; }

# The primary key revokes itself: revkey without a selected subkey, with the
# same answers.
printf 'revkey\ny\n0\n\ny\nsave\n' |
  gpg --batch --quiet --pinentry-mode loopback --passphrase "" --command-fd 0 --status-fd 2 --edit-key "$root" 2>/dev/null
gpg --armor --export "$root" > "$here/root-key-revoked.asc"
gpg --list-keys --with-colons "$root" | grep -q '^pub:r:' || { echo "the primary key is not revoked" >&2; exit 1; }

mkdir -p "$here/real"
jetkvm=AF5A36A993D828FEFE7C18C2D1B9856C26A79E95
curl -fsS -o "$here/real/keys.openpgp.org.asc" "https://keys.openpgp.org/vks/v1/by-fingerprint/$jetkvm"
curl -fsS -o "$here/real/keyserver.ubuntu.com.asc" "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$jetkvm"
curl -fsS -o "$here/real/jetkvm_app-0.5.9.sig" "https://update.jetkvm.com/app/0.5.9/skus/jetkvm-v2/jetkvm_app.sig"
curl -fsS -o "$work/jetkvm_app" "https://update.jetkvm.com/app/0.5.9/skus/jetkvm-v2/jetkvm_app"
python3 "$here/signed_digest.py" "$work/jetkvm_app" "$here/real/jetkvm_app-0.5.9.sig" > "$here/real/jetkvm_app-0.5.9.digest"
