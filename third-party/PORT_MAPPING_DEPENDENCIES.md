# Port-mapping dependencies

- MiniUPnPc 2.3.3: https://github.com/miniupnp/miniupnp at
  `bf4215a7574f88aa55859db9db00e3ae58cf42d6` (`miniupnpc/` subtree).
- libnatpmp: https://github.com/miniupnp/libnatpmp at
  `134fc89e2781e154e40042641f4d8bcbe42579f1`.

Both libraries use the BSD 3-Clause license; their upstream license files are retained in their
vendored directories. Local libnatpmp changes limit its CMake build to a static library, cap request
retries at three to retain OpenGOAL's existing timing, expose readable error strings, and use the
native Windows `SOCKET` type on 64-bit builds.
