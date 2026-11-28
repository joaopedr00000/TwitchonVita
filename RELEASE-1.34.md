# Twitch 1.34 for PS Vita

Based on the successfully built Media Playback Test 33. Original VitaTwitch by Dexlei; fork maintained by joaopedr00000. MIT licensed.

## English
- Handheld and Smart TV layouts, eight Explore cards, category covers and local favorites.
- Channel/account profiles with videos, clips, About and biography header; paginated media catalogs and touch swipes.
- Recorded HLS playback, resume history, seeking, quality switching and frozen last frame while seeking.
- Experimental direct MP4 clips with bounded HTTP Range playback, source VOD fallback and clip editor with explicit publication and confirmation.
- Independent authenticated chat sender, token/scope validation and refresh, verified native HTTPS fallback for supported media and clip API operations.
- Bounded thumbnail decoding, GPU-fenced texture reuse and memory diagnostics.

## Português (Brasil)
- Dois layouts, portátil e TV, Explorar com oito canais, categorias e favoritos locais.
- Perfis completos com bio no cabeçalho, vídeos, clipes, paginação e rolagem por toque.
- Player de gravações com histórico, retomada, avanço/retrocesso e último quadro congelado durante a busca.
- Reprodução MP4 de clipes e editor/publicação experimentais; confirmação da Twitch antes de indicar sucesso.
- Envio de chat independente, renovação de sessão e rotas HTTPS nativas verificadas.
- Cache gráfico limitado, validação de miniaturas e reutilização segura de texturas.

## Español
- Interfaces portátil y TV, ocho canales en Explorar, categorías y favoritos locales.
- Perfiles con biografía, vídeos, clips, paginación y gestos táctiles.
- Reproductor de grabaciones con historial, reanudación y último fotograma congelado durante la búsqueda.
- Reproducción MP4 y editor/publicación de clips experimentales.
- Envío de chat independiente, renovación de sesión y HTTPS nativo verificado en las rutas compatibles.
- Caché gráfico limitado y reutilización de texturas sincronizada con la GPU.

## Installation / Instalação / Instalación
Install Twitch-1.34.vpk via VitaShell over the compatible existing installation (title ID VTWI00001). For authenticated chat and clip operations, use vita_tls_fixer with Full or System + updated certificates, reboot and reauthorize Twitch if scopes are missing. Never share auth.txt.

## Validation and limitations
The exact VPK from [Test 33 CI](https://github.com/joaopedr00000/TwitchonVita/actions/runs/46118799238) passed automated tests and the VitaSDK package checks. The latest changes have not yet been fully verified on physical hardware. Chat sending, direct clips, clip publishing, TLS recovery and GPU crash mitigation still need device validation. VOD clip publishing requires broadcaster/editor permission; account following is read-only and new bookmarks are local. Some legacy paths retain their previous TLS limitations. No claim of complete transport security or universal ad blocking.

Credits: [Dexlei/VitaTwitch](https://github.com/Dexlei/VitaTwitch), [Cecil2121/LiveVita](https://github.com/Cecil2121/livevita) (design inspiration), [devnoname120/vita_tls_fixer](https://github.com/devnoname120/vita_tls_fixer). This unofficial fork is not affiliated with Twitch or Sony.
