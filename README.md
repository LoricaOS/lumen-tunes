# lumen-tunes

The music player for **AspisOS**, a capability-based, no-ambient-authority
operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel.

lumen-tunes is a minimal WAV/MP3 player. It speaks the
[lumen](https://github.com/AspisOS/lumen) external-window protocol, is a
standalone component of the Lumen desktop, and is distributed as a
[herald](https://github.com/AspisOS/AspisOS) package installed as an `/apps`
bundle.

## Role in the system

- An ordinary Lumen client: it connects to the compositor and draws a single
  420x214 window (its descriptor's display name is **Tunes**) with a track
  title, a progress bar with elapsed/total time, and Play/Stop buttons.
- Audio playback goes through `/dev/audio` via the toolkit's **libaudio**, which
  streams with backpressure so a track of any length plays. The decode/produce
  side runs in a forked child (`audio_play_file_async`) so the window stays
  responsive; Stop sends `SIGKILL` to the child and halts the DMA immediately
  (`SYS_AUDIO_STOP`, no drain tail). A `SIGCHLD` signals natural end of track.
- It takes the file to play as `argv[1]`: the
  [file manager](https://github.com/AspisOS/lumen-filemanager) spawns it on a
  `.wav`/`.mp3` and it auto-plays; launched with no argument it shows a
  "No file" prompt.
- Keys: Space/Enter play, S/Esc stop, Q/close quit — or click the buttons.

## Capabilities

lumen-tunes's cap policy (`pkg/etc/aegis/caps.d/tunes`) is the baseline
desktop-app profile:

```
service
```

It carries no elevated capabilities of its own; audio output is reached through
the `/dev/audio` device under the `service` profile.

Because its herald package id (`lumen-tunes`) differs from the bundle/exec name
(`tunes`) and it installs a binary plus a cap policy and an app descriptor
across `/apps` and `/etc`, it is a `class=system` package: first-party and
signature-trusted, installed verbatim by herald.

## Building

lumen-tunes fetches a pinned [glyph](https://github.com/AspisOS/glyph) toolkit
artifact (the GUI libraries it links, including **libaudio** for playback) and
builds against it, then packs a signed herald package. The shared Makefile links
the full toolkit, so libaudio is pulled in automatically.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the toolkit release fetched by `tools/fetch-glyph.sh`.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption — point it
  at an Aegis-native `cc` to build on-device in the future).
- `HERALD_KEY` signs the `.hpkg`.

Output: `lumen-tunes.hpkg` (a `class=system` herald package) +
`lumen-tunes.hpkg.sig`.

## Package payload

```
/apps/tunes/tunes                    the app binary
/apps/tunes/app.ini                  the bundle descriptor
/etc/aegis/caps.d/tunes              its capability policy
```

## Repository layout

```
src/        tunes source
pkg/        install-tree skeleton shipped verbatim (app.ini + caps.d)
tools/      fetch-glyph.sh (toolkit fetch) + pack.sh (build the signed .hpkg)
Makefile    fetch toolkit -> build -> pack
VERSION         this component's version
GLYPH_VERSION   the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — lumen-tunes is a Lumen client, so installing it pulls
[lumen](https://github.com/AspisOS/lumen) (which in turn provides the desktop
fonts).
