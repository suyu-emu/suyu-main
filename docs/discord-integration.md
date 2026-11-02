# Discord presence and NetPlay integration

## Current implementation

Desktop builds with `USE_DISCORD_PRESENCE=ON` publish the active title through
Suyu's existing Discord application. The Qt frontend keeps its user setting
(`enable_discord_presence`). `suyu-cmd` publishes the title after a successful
load, including when an exported Windows executable or its Steam non-Steam
shortcut starts that executable. All of those export launch methods run the
same `suyu-cmd` code. An active NetPlay room name replaces the activity state
while connected. No address, port, room password, or join token is published.
Room names are visible to people who can see the player's Discord activity
while presence is enabled.

This uses the repository's existing `discord-rpc` dependency. The integration
does not create a separate Discord application for each exported title, so
Discord identifies the activity as Suyu and displays the individual game in
the presence text. Linux and macOS export packaging currently produces source
artifacts, not a playable launcher; a future compiled launcher can use the
same `suyu-cmd` presence path.

### Cover art

Discord accepts an `https` image URL as the activity's large image and fetches
it through its own media proxy. Suyu uses the lead image of the game's English
Wikipedia article, with the Suyu logo as the small image. When no cover is
known, the Suyu logo is the large image, as before.

The lookup uses Wikipedia's public REST page summary
(`https://en.wikipedia.org/api/rest_v1/page/summary/<page>`), trying
"`<title> (video game)`" and then "`<title>`". A page is used only when its
short description says "video game" and not "series", "franchise" or
"character". The request carries only the game's title in the page name and a
`User-Agent` identifying Suyu; nothing else about the player, the ROM or the
system is sent. The page's `thumbnail` image (a few hundred pixels wide) is
preferred, then its `originalimage`. Discord then downloads that image from
Wikimedia when it shows the activity.

- **Qt frontend.** While `enable_discord_presence` is on, starting a game
  publishes the presence immediately with the Suyu logo and looks the title up
  on Wikipedia on a worker thread (at most 6 s), so the GUI never waits on the
  network. When a cover is found and the same game is still running, the
  presence is updated with it. Results, including misses, are cached per title
  for the session. Earlier builds sent the title to `suyu.dev` for the same
  purpose whenever presence was on; that site no longer serves box art. The
  setting remains the only switch: turning it off sends nothing to Discord or
  Wikipedia.
- **Exported games.** The export dialog's "Show this game in Discord (cover
  art from Wikipedia)" option (on by default, for Windows packages with a
  launcher) looks the cover up once, at export time, and reuses that lookup for
  the Steam artwork when both are chosen. `suyu-cmd` itself never goes to the
  network for presence.

### `discord.ini` in exported packages

The export writes `discord.ini` next to the package's executable:

```ini
enabled=1
cover_url=https://upload.wikimedia.org/...
```

With the option unchecked it writes `enabled=0` and an empty `cover_url`. The
file can be edited later; it is the off switch for an exported game and for the
Steam shortcut that starts it.

`suyu-cmd` reads the file from its own directory at start:

- `enabled=0` (also `false`, `no`, `off`): Discord is never initialized, so
  no IPC connection is made.
- `cover_url`: used as the large image, with the Suyu logo as the small image,
  when it is an `https://` URL of at most 256 bytes with no spaces or control
  characters. Anything else is ignored and the Suyu logo is used.
- Keys are case-insensitive, whitespace is trimmed, CRLF and a UTF-8 BOM are
  accepted, unknown keys and comment lines are ignored, and only the first
  4 KiB is read.
- With no file, as with plain `suyu-cmd` or packages exported before this
  change, presence behaves as before: enabled, with the Suyu logo.

## Join from Discord

Discord's current [Social SDK activity documentation](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Activity.html)
requires a stable party ID, room capacity with an available slot, and a join
secret before an activity can be joinable. Suyu currently has a direct-connect
room flow, but no launch-time handler that can accept and validate a Discord
join secret. A join button should be added only after that flow exists.

Proposed sequence:

1. Define an opaque, expiring room invite token. Resolve it through the room
   service to a reachable room, and reject expired/full/private rooms. Never put
   a raw room password or private network address in the activity.
2. Add a launch argument or registered protocol handler that receives the
   token, shows the room and requested game, and asks the player to join.
3. Reuse the existing NetPlay direct-connect and local-game lookup paths to
   complete the join. Handle the case where the requested game is absent.
4. Publish party ID, current/max player count, and join secret only for rooms
   that opt into invites. Clear those fields immediately on leave or room close.
5. Register a launch command for installed Suyu. For Steam, distinguish a real
   Steam application ID from Suyu's generated non-Steam shortcut IDs; the
   [Social SDK launch API](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Client.html)
   provides separate command and Steam application registration methods.

Discord [recommends the Social SDK](https://support-dev.discord.com/hc/en-us/articles/30125671534359-Migrating-from-the-Legacy-Game-SDK-to-the-Discord-Social-SDK)
for new integrations. Migration should be evaluated before implementing the
join flow because the existing `discord-rpc` library is legacy and the Social
SDK has its own distribution and app setup requirements.

## Achievements

There is no achievement unlock or publish API in the public Social SDK class
index. Discord's [Game Stats Widget](https://discord.com/developer-newsletter/may-2026)
can show game progress, but is currently available to select developers. A
Suyu achievement system would need per-title definitions and reliable,
consented progress signals from the game or emulator. Arbitrary Switch titles
cannot be assigned correct achievements merely by reading their title or save
files. That work should be a separate design and opt-in feature, with Discord
profile publishing considered only if the platform API becomes available.
