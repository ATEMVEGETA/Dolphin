# Wii-DS Link Windows build, v0.2.0-beta.4

Upstream base: Dolphin 2609a, 409881c34357d0f105fd473167d15ab0bd9c1628.
Source tag: `wii-ds-link-v0.2.0-beta.4`. Restore pinned submodules:

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

The public EXE is rebuilt with release branding from the same functional code
as the candidate on which the tester confirmed Driver: San Francisco (PAL)
connectivity. RELEASE-BUILD.json records exact source revisions and EXE hashes;
SHA256SUMS.txt records archive hashes. Automated runtime checks apply to the
publication build; a metadata-only rebuild is not a separate retail test.
Rebuilds need not be bit-for-bit identical. No Playnite customization is included.

The fix keeps WD and NCD on the normal configured identity and atomically
advertises it to P1 companions. Both emulators must be updated. Older clients
that cannot represent a different parent identity are refused, not silently
treated as compatible. No saved WirelessMac rewrite is needed.

Beta.4 backports upstream commit
`f4b9fae9f53db163a36b39fde1837421016d6a5f` (PR #14983), removing Driver's USA
and PAL frame-rate speed-hack GameINIs and the corresponding obsolete approvals.
Verify their absence from packaged Sys data, not only the source checkout.
The matching melonDS corrects a generic post-Download-Play GameInfo-length
mismatch. No title-specific wireless workaround or shutdown change is included.

Mystery Gift receipt/repeat with Pearl and a non-default parent was confirmed
on beta.3. Production identity/beacon, multiplayer/helper, patch-allowlist and
startup/dependency checks passed. Prior Ranch and four-player Battle Revolution
retail results remain historical beta.1 coverage; those complete flows were
not rerun for beta.4. The separate beta.4-macos.1 tester packages include these
Driver changes and passed native automated checks, but are not Mac gameplay
certification. Their build records identify the exact source pins independently
of later documentation-only commits.
