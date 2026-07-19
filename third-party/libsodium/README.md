# libsodium 1.0.22

Pinned Windows x64 headers, import library, and runtime DLL from the official
`libsodium-1.0.22-msvc.zip` release. The upstream archive is published at
https://download.libsodium.org/libsodium/releases/.

The bridge links the release DLL for both local build configurations. The
library keeps its upstream ISC license in `LICENSE`.

Linux builds use the distribution-provided `libsodium-dev` package, and macOS
builds use the Homebrew `libsodium` formula. Static release presets select the
package-provided static archive so release artifacts do not require a separate
libsodium shared library at runtime.
