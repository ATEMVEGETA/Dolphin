# Experimental Wii–DS LAN transport

In **Settings → Wii**, enable **Connect to melonDS** and enter the IPv4
address of the phone or PC running a compatible melonDS build in
**melonDS device IPv4 address**. Stop emulation before editing. Leaving the
field saves the setting; it applies on the next game launch.

Leave the address empty (or use `127.0.0.1`) for the existing same-PC behavior.
Only numeric IPv4 unicast addresses are supported. Invalid input leaves the
previous address saved and displays an error.

The address is stored as `PeerIPv4` in the `[WiiDSBridge]` section of
`WiiDSBridge.ini` in Dolphin's configuration directory. If that key does not
exist, `WII_DS_PEER_IPV4` provides a fallback for older experimental launchers.
A saved empty value takes precedence over that environment variable.

For a remote peer, Dolphin binds its UDP socket to all IPv4 interfaces and
accepts bridge packets only from the configured peer address and existing
bridge port range (7065–7080). Same-PC mode retains a loopback-only bind.
The bridge protocol, emulated wireless behavior, and timing are unchanged.

LAN use requires companion melonDS transport changes: its bridge must accept
and reply to a remote Dolphin peer rather than restricting traffic to
loopback. Enter the melonDS device's address in Dolphin, not Dolphin's own
address. Allow the bridge traffic through the local firewall as needed.

This is an experimental transport, without authentication or encryption;
use it on a trusted local network and keep backup game saves. Compatibility
and wireless timing still require testing across physical devices.
