# Twitch 1.34 — PS Vita

Unofficial Twitch live-stream client for PS Vita, based on [VitaTwitch by Dexlei](https://github.com/Dexlei/VitaTwitch). MIT license. This fork is not affiliated with Twitch or Sony.

**[Download Twitch 1.34](https://github.com/joaopedr00000/TwitchonVita/releases/latest)**

## Requirements / Requisitos

- PS Vita with HENkaku/Ensō and VitaShell; working internet connection.
- **To send chat messages / Para enviar mensagens / Para enviar mensajes:** install [vita_tls_fixer 0.2.3 or newer](https://github.com/devnoname120/vita_tls_fixer/releases/latest) by devnoname120, using Full or System + updated certificates. Reboot the console. Browser-only is insufficient. Do not combine it with iTLS-Enso or vitals.
- Sign in to Twitch. Sign out and sign in again if the existing account token does not grant `user:write:chat`. Never share `ux0:data/VitaTwitch/auth.txt`.

## Installation / Instalação / Instalación

1. Download `Twitch-1.34.vpk` from Releases.
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

A versão 1.34 inclui as melhorias da Test 33, com compilação e testes automatizados aprovados; a validação completa no PS Vita ainda está pendente. A LiveArea personalizada está incluída no repositório. Para enviar mensagens no chat, instale o **TLS Fixer** na modalidade Full ou System + certificados atualizados, reinicie o console e, se necessário, saia da conta Twitch e entre novamente para autorizar `user:write:chat`. Pressione R, toque em Escrever ou use SELECT durante uma live.

## Español

La versión 1.34 incluye las mejoras de Test 33, con compilación y pruebas automatizadas aprobadas; aún requiere validación completa en PS Vita. Para enviar mensajes en el chat instala **TLS Fixer** con Full o System + certificados actualizados, reinicia la consola y vuelve a iniciar sesión si necesitas autorizar `user:write:chat`. Pulsa R, toca Escribir o usa SELECT durante un directo.

## Security and limitations

The chat send path uses Sony's native HTTPS APIs, verifies the certificate and hostname, validates the issued OAuth token/client/user/scope, and checks Twitch's actual send result. TLS 1.2/1.3 is negotiated by the system provider; no specific negotiated version was measured on the tested console. Anonymous IRC reading and other legacy network paths retain their prior certificate-verification limitations. This is not a claim of complete transport security. OAuth tokens are stored locally in the original auth file. Do not share that file or crash dumps publicly. Some streams may have moderation restrictions, and 720p60 depends on decoding performance. Recorded HLS and experimental direct MP4 clips are supported; clip playback and publication still require hardware validation.

## Build

VitaSDK, CMake, libvita2d, libpng, libjpeg-turbo, libcurl, OpenSSL 1.1.1, zlib and zstd. Supply the Mozilla CA bundle at `assets/cacert.pem` (see [curl CA extract](https://curl.se/ca/)); the CI workflow obtains it over verified HTTPS.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The original decoder architecture is preserved. The custom LiveArea files are versioned with the source. GitHub Actions runs automated regression tests and validates the VPK. The 1.34 release promotes the exact Test 33 CI package. See `RELEASE-1.34.md` and the release checksum. Use the PS system button to leave the app.

## Diagnostics

Logs: `ux0:data/VitaTwitch/log.txt` and `log_prev.txt`. Share relevant logs only, never auth.txt.

## Credits and license

Original project: [Dexlei/VitaTwitch](https://github.com/Dexlei/VitaTwitch). TLS Fixer: [devnoname120/vita_tls_fixer](https://github.com/devnoname120/vita_tls_fixer). MIT — see [LICENSE](LICENSE).


## What's new in 1.34

- Handheld and Smart TV layouts, eight Explore cards, category covers and local favorites.
- Channel/account profiles with videos, clips, About and biography header; paginated media catalogs and touch swipes.
- Recorded HLS playback, resume history, seeking, quality switching and frozen last frame while seeking.
- Experimental direct MP4 clips with bounded HTTP Range playback, source VOD fallback and clip editor with explicit publication and confirmation.
- Independent authenticated chat sender, token/scope validation and refresh, verified native HTTPS fallback for supported media and clip API operations.
- Bounded thumbnail decoding, GPU-fenced texture reuse and memory diagnostics.


See [release notes](RELEASE-1.34.md) for limitations and multilingual details.
