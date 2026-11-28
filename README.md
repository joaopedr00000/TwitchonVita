# VitaTwitch

Cliente de directos de Twitch para PS Vita (VitaSDK). Reproduce con los decodificadores
por hardware (H.264 + AAC), permite iniciar sesión con tu cuenta sin escribir la contraseña
en la consola y navegar por tus canales seguidos, los más vistos y la búsqueda.

## TLS Fixer — requisito para enviar mensagens no chat / chat sending requirement

**[Download TLS Fixer (vita_tls_fixer) by devnoname120](https://github.com/devnoname120/vita_tls_fixer/releases/latest)**

> **Português (Brasil):** Para utilizar o envio de mensagens no chat na versão Classic com suporte ao TLS Fixer, é necessário instalar o plugin **vita_tls_fixer 0.2.3 ou superior**, na opção **Full** ou **System + certificados atualizados**, e reiniciar o PS Vita. A instalação somente para o navegador não é suficiente. Não combine o TLS Fixer com iTLS-Enso ou vitals. Saia da conta Twitch e entre novamente para autorizar a permissão `user:write:chat`. O envio utiliza HTTPS nativo do PS Vita com verificação de certificados, permitindo TLS 1.2/1.3 conforme a negociação com o servidor.

> **English:** To send chat messages in the TLS Fixer-enabled Classic build, install **vita_tls_fixer 0.2.3 or later**, using **Full** or **System + updated certificates**, then reboot the PS Vita. Browser-only installation is insufficient. Do not combine TLS Fixer with iTLS-Enso or vitals. Sign out of Twitch and sign in again to grant `user:write:chat`. Sending uses Sony's native HTTPS APIs with certificate verification and TLS 1.2/1.3 negotiation.

> **Español:** Para enviar mensajes en el chat en la versión Classic compatible con TLS Fixer, instala **vita_tls_fixer 0.2.3 o posterior**, con **Full** o **System + certificados actualizados**, y reinicia la PS Vita. La instalación solo para el navegador no es suficiente. No combines TLS Fixer con iTLS-Enso ni vitals. Cierra sesión en Twitch y vuelve a iniciarla para autorizar `user:write:chat`. El envío utiliza HTTPS nativo con verificación de certificados y negociación TLS 1.2/1.3.

**Development status / Estado:** Native HTTPS chat sending is currently experimental in the [Classic development branch](https://github.com/joaopedr00000/TwitchonVita/tree/visual/twitch-classic), pending physical-console validation. It is not yet part of a public stable release. The plugin is required for the supported modern TLS sending configuration, not for legacy read-only chat or video playback. TLS 1.3 is negotiated, not forced. Thanks to [devnoname120](https://github.com/devnoname120) for TLS Fixer.

## Descargar e instalar

1. Descarga `VitaTwitch.vpk` de la última versión en
   [Releases del fork](https://github.com/joaopedr00000/TwitchonVita/releases/latest).
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
gcc -DVT_NO_NET -Isrc tests/host_test.c src/ts_demux.c src/m3u8.c src/twitch.c src/json.c src/log.c -o host_test.exe
gcc -DVT_NO_NET -Isrc tests/api_test.c src/json.c src/twitch.c src/auth.c src/m3u8.c src/log.c -o api_test.exe
gcc -DVT_NO_NET -Isrc tests/chat_test.c src/chat.c src/json.c src/text.c src/log.c -o chat_test.exe
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
- TLS sin verificación de certificado (la Vita no tiene CAs actuales; NetStream hace lo mismo).
- 720p60 depende de que el decodificador aguante 60 fps; si se descartan muchos frames, usa 720p.
- Chat solo de lectura; sin VODs ni clips todavía.

## Licencia

MIT. Consulta [LICENSE](LICENSE).
