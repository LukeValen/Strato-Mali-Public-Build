# 0.0.5 RC3 fix

## Startup fixes

- FM26 Touch (SDK 20.5.14): the submitted log ends in `CreateClientServiceByHipc` after a request on closed session 0xD024. CMIF CloneCurrentObject/Ex previously inserted the same KSession twice. Closing either endpoint therefore closed both. Clones now have independent session lifetimes and share the service object; already-domain clones also share the domain object table and ID allocator. Cloning before conversion preserves each session's protocol independently.
- Hollow Knight: Silksong 1.0.27776 (SDK 20.5.8): the submitted log requests IApplicationFunctions command 210, then fails with `vector (SVC: SvcWaitSynchronization)`. Command 210 now returns a copied handle to a persistent, initially unsignalled KEvent. Its signalling conditions are not known; this is basic event support, not a full implementation of its unknown semantics.
- WaitSynchronization now returns InvalidHandle for out-of-range or deleted handles instead of allowing a C++ exception to terminate emulation. Zero-handle waits no longer index an empty span while tracing.

Protocol references used during investigation:
- Atmosphere's independent session creation and shared service object cloning: https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/source/sf/hipc/sf_hipc_server_domain_session_manager.cpp
- Command 210 event ABI, from the Ryujinx implementation: https://git.h3cjp.net/H3cJP/ryujinx/compare/Canary-1.3.124...Canary-1.3.136

## Updating

Package: `com.lukevalen.stratomali`. Version code: 504 (RC3 was 503).
Release builds reuse the persistent RC3 signing key. Expected certificate SHA-256:
`b4e8e12d073e4ee9e53adab53f55090049a6dbba9d11d92cd66c2cb3fbcdd3e4`.
An APK with this identity can update that signed RC3 without uninstalling. Older APKs signed with a different ephemeral key cannot be updated by this key.

## Validation scope

Host ASan/UBSan regression tests exercise the production clone dispatch, close path, domain sharing, applet event constructor/response, and WaitSynchronization. The session regression fails with the previous handle-aliasing implementation. Full APK compilation and existing regression checks run in GitHub Actions. These tests do not establish in-game compatibility or rule out later startup blockers; both games still need an on-device retest.
