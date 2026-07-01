# lumen-tunes

The music player for **LoricaOS**, a capability-based, no-ambient-authority
operating system built on the from-scratch
[Aegis](https://github.com/LoricaOS/Aegis) kernel.

lumen-tunes is a minimal WAV/MP3 player that streams audio to `/dev/audio`. It
is an external client of the [lumen](https://github.com/LoricaOS/lumen)
compositor, distributed as a [herald](https://github.com/LoricaOS/LoricaOS)
package and installed as an `/apps` bundle. Its descriptor's display name is
**Tunes**.

## The LoricaOS ecosystem

LoricaOS is decomposed into independent repositories; lumen-tunes is one
graphical leaf of that tree:

| Repo | Role |
|------|------|
| [`LoricaOS/Aegis`](https://github.com/LoricaOS/Aegis) | The kernel. Provides the capability model, the `/dev/audio` device, the streaming-with-backpressure write path, and the `SYS_AUDIO_STOP` syscall that halts DMA. |
| [`LoricaOS/lumen`](https://github.com/LoricaOS/lumen) | The compositor / display server. lumen-tunes connects to its socket for a window and input events. |
| [`LoricaOS/glyph`](https://github.com/LoricaOS/glyph) | The GUI toolkit. Supplies the renderer, the theme, the client side of lumen's window protocol (`lumen_client.h`), **and `libaudio`** — the decode/stream library that drives playback. |
| [`LoricaOS/LoricaOS`](https://github.com/LoricaOS/LoricaOS) | The OS: userland, rootfs, ISO/installer, and the herald package manager that installs this `.hpkg`. |

## What it does

Grounded in `src/main.c`:

- **One window.** Connects to lumen and draws a single 460×250 player card: a
  vinyl-record album-art tile on the left, and on the right the track title,
  format, a seek bar with elapsed/total time, and round transport buttons
  (restart + a play/stop toggle).
- **Playback via libaudio.** Audio goes through `/dev/audio` using the toolkit's
  **libaudio**, which streams with backpressure so a track of any length plays.
  The decode/produce side runs in a forked child (`audio_play_file_async`) so
  the window stays responsive; the progress bar advances against the track
  duration (`audio_duration_ms`).
- **Stop and end-of-track.** Stop sends `SIGKILL` to the playback child and
  issues `SYS_AUDIO_STOP` (syscall 504) to halt the DMA immediately, with no
  drain tail. A `SIGCHLD` distinguishes a user stop from a track that finished
  on its own, updating the status line ("Stopped" vs "Finished"). On quit it
  stops playback so audio never outlives the window.
- **Restart.** libaudio has no seek, so the restart button (and `R`) replays
  from the top — it stops any running playback and starts a fresh child from 0.
- **Launch on a file.** The file to play is `argv[1]`: the
  [file manager](https://github.com/LoricaOS/lumen-filemanager) spawns Tunes on a
  `.wav`/`.mp3` and it auto-plays. Launched with no argument it shows a "No
  track loaded" prompt.
- **Keys.** Space/Enter play-pause toggle, S/Esc stop, R restart, Q/close quit —
  or click the round transport buttons.

## Capabilities

LoricaOS has no ambient authority: a process can do nothing except through
capabilities granted at exec time. lumen-tunes's policy
(`pkg/etc/aegis/caps.d/tunes`) is the baseline desktop-app profile:

```
service
```

It carries no elevated capabilities of its own; audio output is reached through
the `/dev/audio` device under the `service` profile granted to a Lumen client —
no special audio capability is required.

Because its herald package id (`lumen-tunes`) differs from the bundle/exec name
(`tunes`) and it installs a binary plus a cap policy and an app descriptor across
`/apps` and `/etc`, it is a `class=system` package: first-party and
signature-trusted, installed verbatim by herald.

## Building

lumen-tunes fetches a pinned [glyph](https://github.com/LoricaOS/glyph) toolkit
artifact (the GUI libraries it links, including **libaudio** for playback) and
builds against it, then packs a signed herald package. The shared Makefile links
the full toolkit (`-lcitadel -laudio -lauth -lglyph`); a static archive only
contributes the objects actually referenced, so linking libaudio everywhere is
harmless, and here it is the library that matters.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the toolkit release fetched by `tools/fetch-glyph.sh`.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption — point it
  at an Aegis-native `cc` to build on-device in the future).
- `HERALD_KEY` signs the `.hpkg` (ECDSA P-256).

Output: `lumen-tunes.hpkg` (a `class=system` herald package) +
`lumen-tunes.hpkg.sig`.

## Package payload

The `.hpkg` is a manifest-first, uncompressed POSIX `ustar` archive with a
detached signature. Its payload tree:

```
/apps/tunes/tunes                    the app binary (stripped)
/apps/tunes/app.ini                  the bundle descriptor (name=Tunes, exec=tunes)
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
[lumen](https://github.com/LoricaOS/lumen) (which in turn ships the desktop fonts
every dependent inherits). The audio decode/stream library, **libaudio**, comes
from the pinned glyph toolkit fetched at build time, not as a runtime herald
dependency.
