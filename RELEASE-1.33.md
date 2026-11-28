## English
- Redesigned classic Twitch interface, larger readable fonts, native PlayStation button symbols and front-touch navigation.
- Personalized LiveArea, Twitch app name and original custom icon, background and startup artwork.
- Change stream quality without returning to the home screen; viewer count and decoded video diagnostics in the player.
- Read and send live chat messages using the native PS Vita keyboard, Twitch Helix and TLS Fixer.
- Improved OAuth validation, fixed false token expiration, and clearer moderation and network error feedback.
- 19 interface languages, broadcast-language filters, playback profiles, reconnection and buffering improvements.
- Hardware-tested final build; automated regression tests and VitaSDK compilation passed.

**Chat sending requirement:** [TLS Fixer 0.2.3+](https://github.com/devnoname120/vita_tls_fixer/releases/latest), Full or System + updated certificates. Reboot after installation, then sign out/in to authorize `user:write:chat` if needed. Do not combine with iTLS-Enso or vitals. Native HTTPS verifies certificates; TLS 1.2/1.3 is negotiated, not forced. Legacy playback/anonymous chat still use the previous transport.

## Português (Brasil)
- Interface clássica redesenhada, fontes maiores, símbolos dos botões PlayStation e navegação por toque.
- LiveArea personalizada com ícone, logotipo e fundo aprovados.
- Troca de qualidade dentro do player, contador de espectadores e diagnóstico da resolução decodificada.
- Leitura e envio de mensagens no chat com teclado nativo, Twitch Helix e TLS Fixer.
- Correção do falso token expirado e mensagens de erro mais precisas.
- 19 idiomas, filtros por idioma das lives, perfis de reprodução, reconexão e melhorias de buffering.
- Build final testada no PS Vita e aprovada nos testes automatizados.

**Requisito para enviar mensagens:** [TLS Fixer 0.2.3+](https://github.com/devnoname120/vita_tls_fixer/releases/latest) (Full ou System + certificados atualizados). Reinicie o Vita e refaça o login se for necessário autorizar `user:write:chat`. Não combine com iTLS-Enso ou vitals.

## Español
- Interfaz clásica rediseñada, fuentes más grandes, símbolos PlayStation y navegación táctil.
- LiveArea personalizada, cambio de calidad dentro del reproductor y contador de espectadores.
- Lectura y envío de mensajes con teclado nativo, Twitch Helix y TLS Fixer.
- Corrección de expiración falsa del token y diagnósticos más claros.
- 19 idiomas, filtros de directos, perfiles de reproducción y mejoras de reconexión y búfer.
- Compilación final probada en PS Vita y pruebas automatizadas superadas.

**Requisito para enviar mensajes:** [TLS Fixer 0.2.3+](https://github.com/devnoname120/vita_tls_fixer/releases/latest) (Full o System + certificados actualizados). Reinicia y vuelve a iniciar sesión si necesitas autorizar `user:write:chat`. No combines con iTLS-Enso o vitals.

Thanks to [Dexlei](https://github.com/Dexlei/VitaTwitch) for the original application and [devnoname120](https://github.com/devnoname120/vita_tls_fixer) for TLS Fixer.

**1.33 visual revision:** The sidebar header now displays only the Twitch symbol, centered and enlarged; both text labels were removed. No chat, player, settings or LiveArea changes. SFO version 01.33 and title ID VTWI00001 remain unchanged. The revised VPK is rebuilt and CI-validated; physical-console validation of this visual revision is pending.

**PT-BR:** O cabeçalho agora exibe apenas o símbolo da Twitch, centralizado e ampliado. Sem alteração no chat, player ou LiveArea.

**ES:** La cabecera muestra únicamente el símbolo de Twitch, centrado y ampliado. Sin cambios en chat, reproductor o LiveArea.
