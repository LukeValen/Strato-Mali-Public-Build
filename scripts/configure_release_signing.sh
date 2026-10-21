#!/usr/bin/env bash
set -euo pipefail
umask 077

repo="LukeValen/Strato-Mali-Public-Build"
backup_dir="${HOME}/strato-mali-signing"
key_file="${backup_dir}/strato-release.p12"
password_file="${backup_dir}/password"
alias="strato-mali"

for tool in gh keytool python3 base64; do
  command -v "$tool" >/dev/null || { echo "Missing: $tool" >&2; exit 1; }
done
gh auth status >/dev/null
mkdir -p "$backup_dir"
if [[ -e "$key_file" && ! -s "$password_file" ]] || [[ -e "$password_file" && ! -s "$key_file" ]]; then
  echo "Incomplete signing backup in $backup_dir; stopping to avoid changing the APK identity." >&2
  exit 1
fi

if [[ ! -e "$key_file" ]]; then
  python3 - "$password_file" <<'PY'
import pathlib, secrets, sys
pathlib.Path(sys.argv[1]).write_text(secrets.token_hex(32))
PY
  export STRATO_KEY_PASSWORD="$(cat "$password_file")"
  keytool -genkeypair -noprompt -keystore "$key_file" -storetype PKCS12 \
    -storepass:env STRATO_KEY_PASSWORD -keypass:env STRATO_KEY_PASSWORD \
    -alias "$alias" -keyalg RSA -keysize 3072 -validity 10000 \
    -dname 'CN=Strato Mali, O=LukeValen'
  echo "New persistent signing key created. Back up $backup_dir securely."
else
  export STRATO_KEY_PASSWORD="$(cat "$password_file")"
fi

keytool -list -keystore "$key_file" -storepass:env STRATO_KEY_PASSWORD -alias "$alias" >/dev/null
base64 "$key_file" | tr -d '\n' | gh secret set STRATO_SIGNING_KEYSTORE_BASE64 -R "$repo"
printf '%s' "$STRATO_KEY_PASSWORD" | gh secret set STRATO_SIGNING_STORE_PASSWORD -R "$repo"
printf '%s' "$alias" | gh secret set STRATO_SIGNING_KEY_ALIAS -R "$repo"
printf '%s' "$STRATO_KEY_PASSWORD" | gh secret set STRATO_SIGNING_KEY_PASSWORD -R "$repo"
unset STRATO_KEY_PASSWORD
echo "GitHub Actions signing configured for $repo. Key backup: $backup_dir"
