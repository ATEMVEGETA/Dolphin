# Wii-DS Link (unofficial fork)

This addition connects Dolphin's emulated `/dev/net/wd/command` device to a
compatible melonDS Wii-DS Link build through UDP on loopback port 7065. It does
not connect to a real Wii or DS and does not use a Wi-Fi adapter or an external
relay program. This is a downstream fork, not an official Dolphin release.

## Use

Before starting a game, enable **Connect to melonDS** under
**Configuration -> Wii -> Misc Settings**. In the matching melonDS build,
enable **Connect to Dolphin**. Both options default to off and persist between
launches. The emulators may be opened in either order; the link is established
only when the games request it through their own connection menus. Only one
Dolphin and one melonDS link participant on the same PC are supported.

The setting applies to the next game launch. It is disabled in the settings UI
while emulation is active. Ordinary WD behavior is retained when it is off.
The separate diagnostic option is also off by default. When enabled, small
scalar connection records are appended to `User/Logs/wii-ds-bridge.log`, capped
at 8 MiB. The bridge diagnostic logger does not dump game payloads, cartridge
data, saves, or guest CPU memory. General Dolphin logging is configured
separately.

## Implementation

The bridge forwards the guest's Nintendo beacon data and WD multiplayer
application bytes. A type-7 `WIIDSBRIDGE/1` message carries the unmodified
16-byte guest MP configuration separately from application data. The melonDS
virtual radio reproduces Nintendo MP timing and delivers the game's real
reply frames. The returned WD envelope exposes those replies to the Wii guest.
The Wii title's actual Download Play client is transferred by its normal
protocol and runs from DS RAM; this backend does not replace it with a cached
ROM, patch game CPUs, or force application-level success.

Discovery uses a heartbeat, peer-session identity and expiry. A late or
restarted melonDS peer receives the current cached beacon. Stopping or closing
the WD session sends a STOP control message and clears incoming radio state;
savestate restore also discards the live radio peer. The dedicated socket is
nonblocking and bound only to localhost. Packet work and incoming radio queues
are bounded.

The single-parent virtual radio uses BSSID `00:17:ab:4d:b9:e0`. When enabled,
the WD device reports this address through `WD_GetInfo`, consistently with the
melonDS beacons, management responses and disconnect validation. This scoped
virtual-adapter address does not rewrite Dolphin's ordinary networking MAC or
the user's `WirelessMac` configuration. Disabled WD behavior is unchanged.

## Compatibility and provenance

This is a clean port onto official Dolphin **2609a**, commit
`409881c34357d0f105fd473167d15ab0bd9c1628`. It adds only the Wii-DS backend and
settings, not unrelated custom Playnite features. The implementation is
derived from an earlier working Wii-DS experiment and adapts its
transport to SFML 3. The source remains GPL-2.0-or-later, like Dolphin.

The earlier experimental pair was tested with My Pokemon Ranch + Diamond/Pearl and
Pokemon Battle Revolution + properly paired DS saves. Those results establish
the protocol implementation's history. The public beta uses the same protocol
implementation and passed isolated transport/startup validation, but still
needs a fresh retail transfer test on its exact binaries. Other Wii-DS titles,
multi-client links and every ROM/save
combination are not certified. Games can impose their own cartridge/save
identity rules independently of transport compatibility.

No games, BIOS, firmware, NAND or save data belong in a distributable package.
Build downstream packages with a unique distributor identifier and with
`DOLPHIN_DEFAULT_UPDATE_TRACK` empty so official automatic updates cannot
replace this fork's link backend.
