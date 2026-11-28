# VitaTwitch

## Classic — TLS Fixer requirement for chat sending (experimental)

**Install [vita_tls_fixer by devnoname120](https://github.com/devnoname120/vita_tls_fixer/releases/latest) 0.2.3 or newer with the Full option (or System plugin + updated certificates), then reboot.** Browser-only installation is insufficient. Do not combine TLS Fixer with iTLS-Enso or vitals; the TLS Fixer installer documents their incompatibility. The plugin is a required dependency for the supported modern TLS chat-sending configuration, not bundled with this VPK.

Sign out and sign in again to grant the new `user:write:chat` OAuth scope. Press R, touch Write, or select Send message in the player. Messages are posted to Twitch's official Helix Send Chat Message endpoint via Sony `sceHttp`/`sceHttps` with certificate and hostname verification enabled, allowing the system TLS Fixer provider to negotiate TLS 1.2/1.3. The app does not force TLS 1.3 or claim to detect the plugin; negotiation depends on provider and server. Channel ID is resolved via verified Helix before sending. No credentials are sent through the legacy IRC reader. An HTTP 401 means check login, token scope and client ID; HTTP 403 means Twitch rejected permission; NET errors indicate system networking/TLS failure. No automatic POST retry. Existing account registration/client-ID scope support and on-device sending still need physical validation. Anonymous reading and playback keep their previous transport; legacy anonymous IRC does not verify certificates, and other legacy HTTPS paths have independent security limitations. Older notes below describe previous builds.

### Português (Brasil)

Para **enviar mensagens no chat**, instale o [TLS Fixer](https://github.com/devnoname120/vita_tls_fixer/releases/latest) na opção Full ou System + certificados atualizados e reinicie o PS Vita. Saia da conta Twitch e entre novamente para autorizar `user:write:chat`. Não use iTLS-Enso ou vitals junto com TLS Fixer. A leitura do chat e o vídeo continuam no transporte anterior. O envio usa HTTPS nativo com verificação de certificados; TLS 1.3 depende da negociação. Função experimental, ainda requer teste no console.

### Español

Para **enviar mensajes**, instala [TLS Fixer](https://github.com/devnoname120/vita_tls_fixer/releases/latest) con Full o System + certificados, reinicia la consola y vuelve a iniciar sesión para autorizar `user:write:chat`. No combines TLS Fixer con iTLS-Enso ni vitals. Envío experimental por HTTPS nativo con verificación de certificados; pendiente de pruebas físicas.

### PlayFix 5 — legacy chat restoration

This experimental build keeps the PlayFix 4 video/audio/HLS pipeline and restores the original 1.32 anonymous IRC reader, including badges and 7TV. The chat send composer is disabled; no account token is passed to IRC. The legacy chat TLS compatibility mode does not verify the server certificate, so chat authenticity is not guaranteed. The worker uses pthread creation and join for safe lifecycle management. Earlier descriptions of writable chat below refer to previous builds.

### Experimental playback profiles

Four configurable profiles (Economy, Balanced, Low latency, Quality), optional automatic initial quality and controlled downward switching via fresh master playlist, two automatic reconnect attempts, adaptive audio prebuffer, download throughput and FPS diagnostics, volume via D-pad Left/Right, optional session-only sign-in, periodic account validation and split image cache quotas. Low latency uses fewer full HLS segments, not LL-HLS partial segments. Switches restart the decoder and may cause a brief interruption; no seamless upward ABR yet. CPU scaling is conservative and retains the existing hardware codec path. The default keeps manual quality selection. Real Vita testing is required.


### Experimental localization and stability patch

All 19 languages now have 30 core and 97 additional interface, playback, chat and network messages. The extended catalogs are checked for count and matching format placeholders. Native speaker review and on-device font/layout verification remain necessary. Server-provided messages and debug logs are not translated. UTF-8 keyboard titles are converted to UTF-16. HTTPS and IRC now require the bundled CA store; missing or invalid certificates stop connections. HTTP error bodies are no longer logged. HLS shutdown waits for the worker, and queue byte limits are enforced. Hardware and CI verification required; this is not a release. OAuth storage, ABR, VODs, clips, audio-only and the remaining feature roadmap are not implemented in this patch.


## Experimental 1.33 branch: 19 languages, SELECT extras, touch and writable chat

This independent feature branch does **not** publish a new release yet. The
existing source is based on [VitaTwitch by Dexlei](https://github.com/Dexlei/VitaTwitch).
The original hardware H.264/AAC playback, Twitch authentication, TS/fMP4 and
7TV chat rendering are preserved.

- SELECT opens a **native extras menu**: Account, Language, Touch on/off,
  Chat on/off, clear thumbnail cache, About and Close.
- **19 main UI/menu languages**: English, Spanish, Brazilian Portuguese, French,
  Italian, German, Russian, Indonesian, Filipino, Cebuano, Dutch, Polish,
  Turkish, Ukrainian, Swedish, Japanese, Korean, simplified Chinese and
  traditional Chinese. The system language is detected at startup and can be
  overridden in SELECT > Language; stored as `lang=<code>` in settings.txt.
  All 30 core and 97 extended UI messages are translated in all 19 catalogs.
  PS Vita default PGF fonts may lack glyphs for CJK/Indic scripts; UI text may
  need alternate fonts. This is a limitation, not a claim of complete glyph
  rendering.
- **Front touchscreen**: select tabs, open streams, scroll cards, tap search,
  select stream quality and navigate SELECT menus. Controller navigation is
  preserved. Touch input can be disabled in the menu; saved as `touch=0/1`.
- **In-player SELECT menu**: toggle chat visibility, compose a message, toggle
  information overlay, close menu or return to the home screen. Tap the live
  chat's Write button or press R to open the composer directly.
- **Write to Twitch chat**: only an authenticated Twitch OAuth token with
  `chat:read` and `chat:edit` privileges can send; anonymous users remain
  read-only. Single-threaded IRC send queue, input sanitation to prevent IRC
  command injection, a 450-byte payload limit and a conservative 1.6-second
  message throttle. The Vita IME remains nonblocking, preserving HLS playback.
- **Security**: writing requires verified TLS to Twitch IRC using the bundled
  Mozilla CA certificate store (`app0:ca/cacert.pem`). If absent, expired, or
  validation fails, the client falls back to anonymous read-only IRC and never
  sends the OAuth token on an unverified socket. The preexisting app's other
  network code still has insecure TLS defaults; this is a known security
  limitation to address separately before claiming full transport security.
  Twitch access tokens are stored in `ux0:data/VitaTwitch/auth.txt`, as in
  the upstream app. Do not publish this file or crash dumps.

**Build instruction**: supply Mozilla CA certificate bundle at
`assets/cacert.pem`, from [curl's Mozilla CA extract](https://curl.se/ca/).
The experimental GitHub Actions workflow downloads it over verified HTTPS.
CMake packages the cert only if it exists; otherwise authenticated chat cannot
connect.

The Android apps [Xtra](https://github.com/AndreyAsadchy/Xtra) and
[ThystTV](https://github.com/tzii/ThystTV) were used as inspiration for feature
selection (chat controls, quality panels, themes). Their Android/AGPL source was
**not copied** into this MIT-licensed PS Vita project. VODs, clips, audio-only,
BTTV/FFZ and chat appearance settings remain possible future separate additions.

**Testing**: the GitHub Actions workflow builds and validates an experimental
VPK and exercises host tests for translation catalogs, UTF-8/UTF-16 conversion
and chat message sanitization. Real PS Vita tests are still required for OAuth,
IME overlay, touch input and decoder stability.


Cliente de directos de Twitch para PS Vita (VitaSDK). Reproduce con los decodificadores
por hardware (H.264 + AAC), permite iniciar sesión con tu cuenta sin escribir la contraseña
en la consola y navegar por tus canales seguidos, los más vistos y la búsqueda.

## Descargar e instalar

1. Descarga `VitaTwitch.vpk` de la última versión en
   [Releases](https://github.com/Dexlei/VitaTwitch/releases/latest).
2. Cópialo a la Vita (VitaShell: USB o FTP) e instálalo pulsando X sobre el archivo.
   La consola necesita HENkaku/Ensō.
3. Abre VitaTwitch con la Vita conectada a una red Wi-Fi.

Funciones: directos con decodificación por hardware y salto de anuncios, inicio de sesión con
código (sin escribir la contraseña), pestañas Siguiendo / Explorar / Buscar con miniaturas, y
chat en directo con emotes de Twitch y 7TV e insignias (moderador, VIP, suscriptor...).

## Pantallas

| Pestaña | Qué muestra |
|---|---|
| **Siguiendo** | Tus canales seguidos que están en directo (requiere sesión). |
| **Explorar** | Los 30 directos con más espectadores. |
| **Buscar** | Búsqueda de canales (en directo y desconectados). □ abre el nombre exacto. |

Cada directo aparece como tarjeta con miniatura, espectadores, avatar, título y juego.
Al elegir uno se abre el selector de calidad (480p / 720p / 720p60); la última elegida
se recuerda.

## Controles

| Botón | Acción |
|---|---|
| L / R | Cambiar de pestaña |
| Cruceta o stick | Moverse por las tarjetas |
| X / O (según región) | Ver directo / aceptar |
| Triángulo | Actualizar la lista |
| SELECT | Cuenta: iniciar o cerrar sesión |
| START | Salir |
| Durante el vídeo: Triángulo | Mostrar u ocultar la barra de información |
| Durante el vídeo: Cuadrado | Mostrar u ocultar el chat |
| Durante el vídeo: Atrás | Volver al inicio |

## Inicio de sesión

Usa el *device code flow* de OAuth de Twitch con el client-id de la app de Twitch para TV:

1. SELECT → la Vita pide un código a `id.twitch.tv/oauth2/device`.
2. En el móvil o el PC abres **twitch.tv/activate** e introduces el código.
3. La Vita consulta `oauth2/token` cada pocos segundos hasta recibir el token.

El token y el refresh token se guardan en `ux0:data/VitaTwitch/auth.txt` (texto plano, como
cualquier homebrew). Con sesión iniciada, el token se envía también al pedir el stream, así
que si estás suscrito o tienes Turbo recibes el directo sin anuncios.

## Registro de errores (log)

Cada vez que se abre, la app escribe `ux0:data/VitaTwitch/log.txt`; el de la ejecución anterior
queda como `log_prev.txt`. Cada línea se guarda al momento, así que si la app se cierra de golpe la
última línea indica lo último que hacía. Incluye:

- Cabecera: versión, fecha y firmware; resultado de iniciar red, decodificadores y fuente.
- Cada mensaje de error o aviso que aparece en pantalla.
- Peticiones de red fallidas (código curl o HTTP y el inicio de la respuesta de Twitch) o lentas (> 4 s).
- Listas cargadas, comprobación de sesión y pasos del inicio de sesión.
- Al abrir un canal: calidades recibidas, la elegida, inicio del decodificador, primer frame, audio,
  anuncios saltados, segmentos perdidos, una línea de estado por minuto y resumen al salir.

Nunca se escriben tokens: las URLs se guardan sin la parte `?…`. Máximo 1 MB por ejecución.
Para sacarlo: VitaShell → `ux0:data/VitaTwitch/` → copiar por USB o FTP (SELECT en VitaShell).
Si la consola muestra un error del sistema (C2-12828-1…), el volcado está en `ux0:data/*.psp2dmp`.

## Cómo funciona la reproducción

1. **Token** → `POST https://gql.twitch.tv/gql` (`streamPlaybackAccessToken`, como streamlink).
2. **Master playlist** → `usher.ttvnw.net/api/channel/hls/<canal>.m3u8?sig=…&token=…`.
3. `hls.c`: refresca la media playlist, entra a 3 segmentos del borde en vivo y **salta los
   anuncios insertados** (segmentos cuyo título no es `live`).
4. `ts_demux.c`: demuxer MPEG-TS; reensambla los access units H.264 por NAL AUD (Twitch no
   alinea PES con frames) y mantiene los timestamps continuos tras discontinuidades.
   `mp4_demux.c`: los canales con *Enhanced Broadcasting* usan fMP4/CMAF (`#EXT-X-MAP` +
   segmentos `.mp4`); se lee la cabecera (avcC/esds) y cada `moof`/`trun`, y se entrega lo
   mismo que el demuxer TS (H.264 Annex B con SPS/PPS en cada keyframe y AAC con cabecera ADTS).
5. `player.c`: H.264 → `sceAvcdecDecode` directo a una textura de vita2d; AAC → `sceAudiodecDecode`
   → `sceAudioOut`. El audio es el reloj maestro; los frames tardíos se descartan.

NetStream (GrapheneCt) usa el módulo `SceAvPlayer` de WebMAF, no disponible en VitaSDK; se
sigue su estructura (hilo de audio, superficie en el hilo principal, libcurl sin verificación
TLS) llamando directamente a los decodificadores.

## Código

| Archivo | Función |
|---|---|
| `main.c` | Interfaz: pestañas, tarjetas, modales, teclado, cuenta |
| `ui.c` | Dibujo (texto recortado, limpieza de emojis, spinner, hints), entrada con autorepetición |
| `images.c` | Caché de miniaturas/avatares: descarga en un hilo, texturas en el hilo principal |
| `task.c` | Peticiones de red en segundo plano (la interfaz no se congela y se pueden cancelar) |
| `twitch.c` | GQL: token de reproducción, Siguiendo, Explorar, Búsqueda; usher |
| `auth.c` | Login por código de dispositivo, validación y refresco del token |
| `json.c` | Parser JSON pequeño |
| `log.c` | Registro de diagnóstico en `log.txt` |
| `chat.c` | Chat IRC (hilo), parser, emotes de Twitch y 7TV |
| `chat_view.c` | Dibujo del panel de chat |
| `text.c` | Limpieza de texto (emojis que la fuente no dibuja) |
| `hls.c`, `ts_demux.c`, `mp4_demux.c`, `m3u8.c`, `pktqueue.c`, `player.c` | Reproductor |

## Compilar

Requisitos (ya instalados en este PC): VitaSDK en `C:\vitasdk` con `curl`, `openssl-1.1.1`,
`zlib`, `libvita2d`, `libpng`, `libjpeg-turbo` (vdpm, canal `nightly`), CMake y Ninja.

```powershell
.\build.ps1
```

Genera `VitaTwitch.vpk`. Instálalo con VitaShell (la consola necesita HENkaku/Ensō).

## Pruebas en PC

```powershell
gcc -DVT_NO_NET -Isrc tests/host_test.c src/ts_demux.c src/m3u8.c src/twitch.c src/json.c src/log.c src/i18n.c -o host_test.exe
gcc -DVT_NO_NET -Isrc tests/api_test.c src/json.c src/twitch.c src/auth.c src/m3u8.c src/log.c src/i18n.c -o api_test.exe
gcc -DVT_NO_NET -Isrc tests/chat_test.c src/chat.c src/json.c src/text.c src/log.c src/i18n.c -o chat_test.exe
gcc -DVT_NO_NET -Isrc tests/mp4_test.c src/mp4_demux.c -o mp4_test.exe
```

- `mp4_test init.mp4 seg1.mp4 ...`: demuxer fMP4 con la cabecera y segmentos reales de un canal
  con Enhanced Broadcasting.

- `chat_test global.json user.json irc.txt`: parser IRC, emotes de Twitch por posición, sets de
  7TV reales y una captura real de chat.

- `host_test`: parser M3U8, demuxer TS, ADTS y token de reproducción con datos reales.
- `api_test bodies <dir>` escribe las peticiones GQL que genera el código C;
  `api_test parse top.json search.json followed.json device.json poll.json` comprueba el
  parser JSON, las listas y el login con las respuestas reales de Twitch.

## Chat con emotes de 7TV

Durante el directo, **□** muestra u oculta el chat (panel de 320 px a la derecha; el vídeo se
reduce a 640 px de ancho). La preferencia se guarda (`chat=1` en `settings.txt`).

- Solo lectura, anónimo: IRC por TLS a `irc.chat.twitch.tv:6697` como `justinfanNNNNN`
  (curl en modo `CONNECT_ONLY`), con `CAP REQ :twitch.tv/tags twitch.tv/commands`.
- Emotes de Twitch: por posición desde la etiqueta `emotes` →
  `static-cdn.jtvnw.net/emoticons/v2/<id>/static/dark/1.0` (PNG).
- 7TV: al recibir `ROOMSTATE` (trae `room-id`) se descargan `7tv.io/v3/emote-sets/global` y
  `7tv.io/v3/users/twitch/<room-id>` (~2 MB; ~17 MB de memoria temporal al parsear). Imagen:
  `cdn.7tv.app/emote/<id>/1x.png` (estáticos) o `1x_static.png` (primer frame de los animados;
  la Vita no tiene decodificador WebP/GIF, así que los animados se ven quietos).
- Insignias (moderador, VIP, streamer, suscriptor con sus meses, Prime, bits...): hasta 3
  junto al nombre, desde la etiqueta `badges` de cada mensaje. Las imágenes se piden por GQL
  (`badges` globales y `user(id).broadcastBadges` del canal, PNG 18x18).
- Muestra suscripciones/anuncios del canal (USERNOTICE) y mensajes `/me`. Los emojis y nombres
  CJK se quitan o se sustituyen por el login, porque la fuente de la Vita no los dibuja.
- BTTV y FFZ no están incluidos (se podrían añadir igual que 7TV).

## Limitaciones conocidas

- Nada de esta versión se ha probado aún en una Vita real (solo en PC: red, parsers, demuxer).
- TLS requiere el paquete de certificados CA incluido en el VPK experimental.
- 720p60 depende de que el decodificador aguante 60 fps; si se descartan muchos frames, usa 720p.
- Chat solo de lectura; sin VODs ni clips todavía.

## Licencia

MIT. Consulta [LICENSE](LICENSE).
