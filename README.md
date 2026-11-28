# Twitch 1.33 — PS Vita

Unofficial Twitch live-stream client for PS Vita, based on [VitaTwitch by Dexlei](https://github.com/Dexlei/VitaTwitch). MIT license. This fork is not affiliated with Twitch or Sony.

**[Download Twitch 1.33](https://github.com/joaopedr00000/TwitchonVita/releases/latest)**

## Requirements / Requisitos

- PS Vita with HENkaku/Ensō and VitaShell; working internet connection.
- **To send chat messages / Para enviar mensagens / Para enviar mensajes:** install [vita_tls_fixer 0.2.3 or newer](https://github.com/devnoname120/vita_tls_fixer/releases/latest) by devnoname120, using Full or System + updated certificates. Reboot the console. Browser-only is insufficient. Do not combine it with iTLS-Enso or vitals.
- Sign in to Twitch. Sign out and sign in again if the existing account token does not grant `user:write:chat`. Never share `ux0:data/VitaTwitch/auth.txt`.

## Installation / Instalação / Instalación

1. Download `Twitch-1.33.vpk` from Releases.
2. Transfer via VitaShell USB/FTP and install with X. The title ID remains `VTWI00001`; it can update an existing compatible installation.
3. Open the app, sign in via the Twitch activation code if necessary, and select a live channel.

## Features

- Classic dark purple Twitch interface with larger typography, PlayStation button symbols and touch controls.
- Custom LiveArea icon, background and startup artwork stored directly in `sce_sys/`.
- Followed channels, Explore, games, search, and broadcast-language filters.
- Hardware H.264/AAC playback, TS/fMP4, playback profiles, quality switching from the player, viewer count and diagnostics.
- Twitch/7TV emotes, badges and read/write chat. R or touch Write opens the native keyboard. SELECT opens player actions.
- 19 interface languages with automatic system detection.
- Improved buffering/reconnection, safe queue lifecycle and OAuth/chat error diagnostics.

## Português (Brasil)

A versão 1.33 é a build aprovada em teste no PS Vita. A LiveArea personalizada está incluída no repositório. Para enviar mensagens no chat, instale o **TLS Fixer** na modalidade Full ou System + certificados atualizados, reinicie o console e, se necessário, saia da conta Twitch e entre novamente para autorizar `user:write:chat`. Pressione R, toque em Escrever ou use SELECT durante uma live.

## Español

La versión 1.33 corresponde a la compilación aprobada en una PS Vita. Para enviar mensajes en el chat instala **TLS Fixer** con Full o System + certificados actualizados, reinicia la consola y vuelve a iniciar sesión si necesitas autorizar `user:write:chat`. Pulsa R, toca Escribir o usa SELECT durante un directo.

## Security and limitations

The chat send path uses Sony's native HTTPS APIs, verifies the certificate and hostname, validates the issued OAuth token/client/user/scope, and checks Twitch's actual send result. TLS 1.2/1.3 is negotiated by the system provider; no specific negotiated version was measured on the tested console. Anonymous IRC reading and other legacy network paths retain their prior certificate-verification limitations. This is not a claim of complete transport security. OAuth tokens are stored locally in the original auth file. Do not share that file or crash dumps publicly. Some streams may have moderation restrictions, and 720p60 depends on decoding performance. VODs/clips are not supported.

## Build

VitaSDK, CMake, libvita2d, libpng, libjpeg-turbo, libcurl, OpenSSL 1.1.1, zlib and zstd. Supply the Mozilla CA bundle at `assets/cacert.pem` (see [curl CA extract](https://curl.se/ca/)); the CI workflow obtains it over verified HTTPS.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The original decoder architecture is preserved. The custom LiveArea files are versioned with the source. GitHub Actions runs automated regression tests and validates the VPK. The 1.33 release includes a logo-only sidebar header revision and removes the START shortcut for closing the app, built from the hardware-approved code with no playback or chat changes. Use the PS system button to leave the app. See `RELEASE-1.33.md` and the release checksum.

## Diagnostics

Logs: `ux0:data/VitaTwitch/log.txt` and `log_prev.txt`. Share relevant logs only, never auth.txt.

## Credits and license

Original project: [Dexlei/VitaTwitch](https://github.com/Dexlei/VitaTwitch). TLS Fixer: [devnoname120/vita_tls_fixer](https://github.com/devnoname120/vita_tls_fixer). MIT — see [LICENSE](LICENSE).


### Experimental 1.34 discovery updates
Category cover browsing, local category favorites, alternate TV grid, offline follows, profile metadata and static BTTV/FFZ emotes were inspired by [LiveVita by Cecil2121](https://github.com/Cecil2121/livevita) (MIT). These are independent adaptations. The live player remains unchanged; VOD/clip playback is not yet implemented. Native Twitch follow remains experimental and may be rejected by Twitch.


### Smart TV layout and device favorites
The alternate TV interface now has top navigation, a featured preview and horizontal shelves. Save on Vita is a separate persistent local bookmark, not a Twitch account follow. The Following view cycles live account follows, all account follows and local saved channels. Twitch account mutations remain experimental and may be refused; the app does not report a successful account follow unless independently confirmed.


### Local Follow 9
Account follows are read-only. The profile shows existing Twitch follows and offers to save new channels on the Vita, with an explicit notice that Twitch account following is unavailable. Local save is idempotent and removal affects the local list only. No follow or unfollow mutations are sent.


### Experimental 1.34 Recordings Test 11

- Read-only Twitch video catalog (archives, highlights, uploads and clips), plus local watch-later and resume metadata (64 entries).
- Separate recorded HLS playback mode, pause, seek by segment, quality restart at current position, and small RAM-only packet queues. No media files are saved to disk. Completed unencrypted HLS playlists only; ordinary MP4 clips are listed but are not playable in this build.
- Clip creation via official Helix endpoints, using the existing account after reauthorization for the additional scopes. L in live playback opens a duration/confirmation dialog and reconnects afterward; Square in VOD playback opens the clip dialog and resumes afterward. Clip requests are not retried automatically; success is shown only after Get Clips confirms the ID. VOD creation requires broadcaster/editor permission. No clip is created automatically.
- Experimental only; hardware validation and API compatibility are not yet established.

### Experimental Recordings Test 12

Fix misleading sign-in error when Helix channel lookup fails. Public video/clip catalog has a read-only anonymous GraphQL fallback with verified TLS, without sending account credentials. Distinguish HTTP rejection from transport/TLS and catalog errors; Triangle retries. No logout or token changes. Fix Helix percent-prefixed thumbnail placeholders. GraphQL availability remains experimental.

### Experimental Clip Editor Test 13

Adds a native duration/end-position editor, title keyboard, preview with audio using existing recorded HLS player, optional repeat, explicit publish/cancel. Live preview uses a bounded in-memory snapshot of recent segment URLs only (no stored media); CDN URLs can expire. Live publishing is server-current, not guaranteed to match the preview, and requires a separate explicit confirmation. VOD preview starts at a segment boundary and stops near the selected end; the API uses the selected exact offset. Direct ordinary MP4 clip playback is still unsupported. Hardware testing pending.

Published clips with an available source VOD and known offset can be previewed through that source's HLS. Direct MP4 and clips without an accessible source are still unsupported. Preview does not need login, but publication does.

### Experimental Profile/TLS Test 14

Batch changes from Test 13 feedback: explicit packaged CA file on Vita (strict peer/hostname verification retained), video library from live profile with live reconnect on return, own profile with Live/Videos/Clips/Saved/Continue/About/Session tabs, top-right sign-in in handheld mode, grouped settings, logo-only TV header, and direct playback without a pre-play quality chooser. Hardware TLS and catalog behavior still need validation.

### Experimental Catalog/TLS Test 15

Uses a memory-backed CAINFO_BLOB on modern libcurl and verified native Sony HTTPS read-only fallback for Twitch API requests (shared native SSL lifecycle with chat). Does not disable TLS verification. Adds detailed transport diagnostics, complete profile access from live overlay by name/avatar, extended About with public metadata and social links when returned, fork credits, separate Explore and Categories navigation in both layouts (handheld order Explore, Categories, Settings). Hardware tests pending.
