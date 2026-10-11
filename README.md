# as_hide (Plugify)

Hide mode for admins: 'mm_hide' (console) or '!hide' (chat, needs @admin/hide) moves the caller off every team (spectator, then none) so they disappear from the game while staying connected; using it again, or trying to join a team, puts them back. While a hidden admin tries to join a team the join is stopped and the hide mode is switched off. A port of the Metamod:Source module 'Hide' of the Pisex cs2-admin_system plugin.

Plugify port (fork) of **Hide** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_hide-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_hide`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

