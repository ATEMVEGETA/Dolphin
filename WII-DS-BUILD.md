# Wii-DS Link Windows build, v0.2.0-beta.1

Upstream base: Dolphin 2609a, commit
`409881c34357d0f105fd473167d15ab0bd9c1628`. This fork's modified source is
tagged `wii-ds-link-v0.2.0-beta.1`. Restore all upstream-pinned submodules:

```sh
git submodule update --init --recursive
```

The Windows x64 Release build uses Visual Studio 2026 C++ Build Tools, compiler
19.51.36260, Windows SDK 10.0.26100.0, CMake 4.3.3, and upstream-pinned Qt 6.8.3,
SFML 3 and SDL 3. Configuration:

```sh
cmake -S . -B build-wiids -G "Visual Studio 18 2026" -A x64 \
  -DENABLE_TESTS=OFF -DDISTRIBUTOR=WiiDSLinkMultiDSTest \
  -DENABLE_AUTOUPDATE=OFF -DDOLPHIN_DEFAULT_UPDATE_TRACK=
cmake --build build-wiids --config Release --target dolphin-emu --parallel 2
```

Distribute Dolphin.exe with the built Qt DLLs/plugins, qt.conf, Data/Sys runtime
data, upstream license files and notices. A portable.txt marker selects a
package-local User directory. No preconfigured game/NAND/save profile belongs
in the release. See upstream build instructions for tool installation.

The release preserves the exact binary used in the successful multi-DS tests,
compiled from the working tree based on fork commit
`678f03db3189054bc3afa1f093bb1c44ce96e1cb`. Its embedded revision can therefore
refer to that older base; the release tag contains the complete modified code
plus publication documentation. SHA-256:
`68b93fbf0512f51442136e633da2837a33626b77b2e9e9d8f69daac17cdc998c`.
Rebuilds need not be bit-for-bit identical.

The bridge capacity is 15 DS associations. Automated checks covered eight
clients, assigned AIDs, real reply aggregation, stale-session rejection,
timeouts and per-client disconnects. Four-player Battle Revolution and normal
Ranch flows were reported successful in retail tests. Other game combinations
and eight-player retail gameplay are not certified by these results.
