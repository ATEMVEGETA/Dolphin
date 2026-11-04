# Wii-DS Link Windows build, v0.2.0-beta.3

Upstream base: Dolphin 2609a, 409881c34357d0f105fd473167d15ab0bd9c1628.
Source tag: `wii-ds-link-v0.2.0-beta.3`. Restore pinned submodules:

```sh
git submodule update --init --recursive
```

Windows x64 Release uses Visual Studio 2026 Build Tools, MSVC 19.51.36260,
Windows SDK 10.0.26100.0, CMake 4.3.3 and upstream-pinned Qt 6.8.3, SFML3,
SDL3 and other dependencies. No dependency code was changed by this fix.

```sh
cmake -S . -B build-wiids -G "Visual Studio 18 2026" -A x64 \
  -DENABLE_TESTS=OFF -DDISTRIBUTOR=WiiDSLinkUnofficial \
  -DENABLE_AUTOUPDATE=OFF -DDOLPHIN_DEFAULT_UPDATE_TRACK=
cmake --build build-wiids --config Release --target dolphin-emu --parallel 2
```

Distribute Dolphin.exe with the matching Qt DLLs/plugins, qt.conf, Data/Sys,
portable.txt and upstream/dependency notices. Never include a configured User
directory, games, dumped firmware/NAND, saves or diagnostics.

The exact retail-tested EXE is preserved. It was built from fork base
22060863a04f086bd0c51a6fdf7500e27470d35d plus this tag's functional changes,
before final publication documentation. Embedded older Git/test labels do not
identify a different payload; use the release build record and EXE SHA256:
39d004a14c0a82bb472fdd3a0af01da1600b1b41a7d6efd005f96cf46d466fb8.
Rebuilds need not be bit-for-bit identical. No Playnite customization is included.

The fix keeps WD and NCD on the normal configured identity and atomically
advertises it to P1 companions. Both emulators must be updated. Older clients
that cannot represent a different parent identity are refused, not silently
treated as compatible. No saved WirelessMac rewrite is needed.

Mystery Gift receipt with Pearl and a non-default parent was confirmed on
this exact pair and repeated successfully. Seventeen production identity/
beacon tests, multiplayer/helper tests and private startup/dependency checks
passed. Prior Ranch and four-player Battle Revolution retail results remain
historical beta.1 coverage; those complete flows were not rerun for this fix.
macOS binaries containing this fix are not published yet.
