# Strato Mali APK updates

Android accepts an APK as an update only when the package name and signing certificate match the installed app and its versionCode does not decrease. The mainline package remains `com.lukevalen.stratomali`; RC3 uses versionCode 503. RC3 fix uses versionCode 504 and versionName `0.0.5 RC3 fix`, with the same persistent signing certificate.

Previously, CI signed each release with its runner's temporary debug keystore. Those old APKs cannot be updated with a newly generated certificate. An RC3 installation may therefore require one removal and reinstall if the installed certificate differs. Back up saves first. Subsequent releases can install over RC3 by reusing the same signing key and increasing versionCode.

On a trusted machine with `gh` authenticated, run `bash scripts/configure_release_signing.sh` once. This creates a persistent key in `~/strato-mali-signing`, stores its credentials as GitHub Actions repository secrets, and reuses it on later runs. Keep an offline backup of this directory: losing the private key means future APKs cannot update installs signed with it. Never commit or share the key or password. The public CI build refuses to produce its mainline APK without these secrets.
