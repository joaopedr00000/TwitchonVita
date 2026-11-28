#include "i18n.h"
#include <string.h>
/* Nineteen complete navigation/menu and extended interface catalogs.
 * UTF-8 labels are data: font glyph availability depends on Vita firmware. */
typedef struct {const char *code, *name; const char *text[VT_I_COUNT];} lang_t;
static const lang_t langs[VT_LANG_COUNT] = {
  {"en", "English", {"Following", "Explore", "Search", "LIVE", "Offline", "Menu", "Account", "Language", "Touch", "Chat", "Write", "Log in to send messages", "On", "Off", "Refresh", "Close", "Play", "Quality", "Loading", "Back", "Exit", "Settings", "Clear thumbnails", "About", "Log in", "Log out", "Tab", "Send message", "Show info", "Hide info"}},
  {"es", "Español", {"Siguiendo", "Explorar", "Buscar", "DIRECTO", "Desconectado", "Menú", "Cuenta", "Idioma", "Táctil", "Chat", "Escribir", "Inicia sesión para escribir", "Activado", "Desactivado", "Actualizar", "Cerrar", "Reproducir", "Calidad", "Cargando", "Volver", "Salir", "Ajustes", "Limpiar miniaturas", "Acerca de", "Iniciar sesión", "Cerrar sesión", "Pestaña", "Enviar mensaje", "Mostrar info", "Ocultar info"}},
  {"pt-BR", "Português (Brasil)", {"Seguindo", "Explorar", "Buscar", "AO VIVO", "Offline", "Menu", "Conta", "Idioma", "Controle por toque", "Chat", "Escrever", "Entre na conta para enviar mensagens", "Ativado", "Desativado", "Atualizar", "Fechar", "Reproduzir", "Qualidade", "Carregando", "Voltar", "Sair", "Configurações", "Limpar cache", "Sobre", "Entrar na conta", "Sair da conta", "Aba", "Enviar mensagem", "Mostrar informações", "Ocultar informações"}},
  {"fr", "Français", {"Abonnements", "Explorer", "Rechercher", "EN DIRECT", "Hors ligne", "Menu", "Compte", "Langue", "Tactile", "Chat", "Écrire", "Connectez-vous pour écrire", "Activé", "Désactivé", "Actualiser", "Fermer", "Lire", "Qualité", "Chargement", "Retour", "Quitter", "Paramètres", "Vider les vignettes", "À propos", "Connexion", "Déconnexion", "Onglet", "Envoyer un message", "Afficher infos", "Masquer infos"}},
  {"it", "Italiano", {"Seguiti", "Esplora", "Cerca", "IN DIRETTA", "Offline", "Menu", "Account", "Lingua", "Touch", "Chat", "Scrivi", "Accedi per scrivere", "Attivo", "Disattivo", "Aggiorna", "Chiudi", "Riproduci", "Qualità", "Caricamento", "Indietro", "Esci", "Impostazioni", "Pulisci miniature", "Info", "Accedi", "Esci dall'account", "Scheda", "Invia messaggio", "Mostra info", "Nascondi info"}},
  {"de", "Deutsch", {"Gefolgt", "Entdecken", "Suchen", "LIVE", "Offline", "Menü", "Konto", "Sprache", "Touch", "Chat", "Schreiben", "Zum Chatten anmelden", "Ein", "Aus", "Aktualisieren", "Schließen", "Abspielen", "Qualität", "Laden", "Zurück", "Beenden", "Einstellungen", "Vorschaubilder leeren", "Info", "Anmelden", "Abmelden", "Register", "Nachricht senden", "Info anzeigen", "Info ausblenden"}},
  {"ru", "Русский", {"Подписки", "Обзор", "Поиск", "ЭФИР", "Не в сети", "Меню", "Аккаунт", "Язык", "Касание", "Чат", "Написать", "Войдите, чтобы писать", "Вкл", "Выкл", "Обновить", "Закрыть", "Смотреть", "Качество", "Загрузка", "Назад", "Выход", "Настройки", "Очистить превью", "О программе", "Войти", "Выйти", "Вкладка", "Отправить сообщение", "Показать инфо", "Скрыть инфо"}},
  {"id", "Bahasa Indonesia", {"Mengikuti", "Jelajahi", "Cari", "LIVE", "Offline", "Menu", "Akun", "Bahasa", "Sentuh", "Obrolan", "Tulis", "Masuk untuk mengirim pesan", "Aktif", "Nonaktif", "Segarkan", "Tutup", "Putar", "Kualitas", "Memuat", "Kembali", "Keluar", "Pengaturan", "Hapus thumbnail", "Tentang", "Masuk", "Keluar akun", "Tab", "Kirim pesan", "Tampilkan info", "Sembunyikan info"}},
  {"fil", "Filipino", {"Sinusundan", "Tuklasin", "Hanapin", "LIVE", "Offline", "Menu", "Account", "Wika", "Pindot", "Chat", "Magsulat", "Mag-login para magpadala", "Bukas", "Patay", "I-refresh", "Isara", "I-play", "Kalidad", "Naglo-load", "Bumalik", "Lumabas", "Settings", "Linisin ang larawan", "Tungkol", "Mag-login", "Mag-logout", "Tab", "Magpadala ng mensahe", "Ipakita ang info", "Itago ang info"}},
  {"ceb", "Cebuano", {"Gisundan", "Susiha", "Pangitaa", "LIVE", "Offline", "Menu", "Account", "Pinulongan", "Hikap", "Chat", "Pagsulat", "Pag-login aron makapadala", "On", "Off", "I-refresh", "Sirad-i", "I-play", "Kalidad", "Nag-load", "Balik", "Gawas", "Settings", "Hawani ang hulagway", "Mahitungod", "Pag-login", "Pag-logout", "Tab", "Padala og mensahe", "Ipakita ang info", "Tagoa ang info"}},
  {"nl", "Nederlands", {"Volgend", "Ontdekken", "Zoeken", "LIVE", "Offline", "Menu", "Account", "Taal", "Aanraken", "Chat", "Schrijven", "Log in om te chatten", "Aan", "Uit", "Vernieuwen", "Sluiten", "Afspelen", "Kwaliteit", "Laden", "Terug", "Afsluiten", "Instellingen", "Miniaturen wissen", "Over", "Inloggen", "Uitloggen", "Tab", "Bericht sturen", "Info tonen", "Info verbergen"}},
  {"pl", "Polski", {"Obserwowani", "Odkrywaj", "Szukaj", "NA ŻYWO", "Offline", "Menu", "Konto", "Język", "Dotyk", "Czat", "Napisz", "Zaloguj się, aby pisać", "Wł.", "Wył.", "Odśwież", "Zamknij", "Odtwórz", "Jakość", "Ładowanie", "Wstecz", "Wyjdź", "Ustawienia", "Wyczyść miniatury", "O aplikacji", "Zaloguj", "Wyloguj", "Karta", "Wyślij wiadomość", "Pokaż informacje", "Ukryj informacje"}},
  {"tr", "Türkçe", {"Takip edilenler", "Keşfet", "Ara", "CANLI", "Çevrimdışı", "Menü", "Hesap", "Dil", "Dokunmatik", "Sohbet", "Yaz", "Yazmak için giriş yapın", "Açık", "Kapalı", "Yenile", "Kapat", "Oynat", "Kalite", "Yükleniyor", "Geri", "Çıkış", "Ayarlar", "Küçük resimleri temizle", "Hakkında", "Giriş yap", "Çıkış yap", "Sekme", "Mesaj gönder", "Bilgiyi göster", "Bilgiyi gizle"}},
  {"uk", "Українська", {"Підписки", "Огляд", "Пошук", "НАЖИВО", "Не в мережі", "Меню", "Обліковий запис", "Мова", "Дотик", "Чат", "Написати", "Увійдіть, щоб писати", "Увімк.", "Вимк.", "Оновити", "Закрити", "Дивитися", "Якість", "Завантаження", "Назад", "Вийти", "Налаштування", "Очистити прев'ю", "Про програму", "Увійти", "Вийти з акаунта", "Вкладка", "Надіслати повідомлення", "Показати інфо", "Сховати інфо"}},
  {"sv", "Svenska", {"Följer", "Utforska", "Sök", "LIVE", "Offline", "Meny", "Konto", "Språk", "Pekskärm", "Chatt", "Skriv", "Logga in för att skriva", "På", "Av", "Uppdatera", "Stäng", "Spela", "Kvalitet", "Laddar", "Tillbaka", "Avsluta", "Inställningar", "Rensa miniatyrer", "Om", "Logga in", "Logga ut", "Flik", "Skicka meddelande", "Visa info", "Dölj info"}},
  {"ja", "Japanese (日本語)", {"フォロー中", "探す", "検索", "ライブ", "オフライン", "メニュー", "アカウント", "言語", "タッチ", "チャット", "入力", "送信にはログインが必要", "オン", "オフ", "更新", "閉じる", "再生", "画質", "読込中", "戻る", "終了", "設定", "画像キャッシュ削除", "情報", "ログイン", "ログアウト", "タブ", "メッセージ送信", "情報を表示", "情報を隠す"}},
  {"ko", "Korean (한국어)", {"팔로잉", "탐색", "검색", "생방송", "오프라인", "메뉴", "계정", "언어", "터치", "채팅", "입력", "메시지를 보내려면 로그인", "켜기", "끄기", "새로고침", "닫기", "재생", "화질", "로딩 중", "뒤로", "종료", "설정", "썸네일 삭제", "정보", "로그인", "로그아웃", "탭", "메시지 보내기", "정보 보기", "정보 숨기기"}},
  {"zh-CN", "Chinese Simplified (简体中文)", {"关注", "发现", "搜索", "直播", "离线", "菜单", "账户", "语言", "触摸", "聊天", "输入", "登录后才能发送", "开启", "关闭", "刷新", "关闭", "播放", "画质", "加载中", "返回", "退出", "设置", "清理缩略图", "关于", "登录", "退出登录", "标签", "发送消息", "显示信息", "隐藏信息"}},
  {"zh-TW", "Chinese Traditional (繁體中文)", {"追蹤", "探索", "搜尋", "直播", "離線", "選單", "帳戶", "語言", "觸控", "聊天", "輸入", "登入後才能傳送", "開啟", "關閉", "重新整理", "關閉", "播放", "畫質", "載入中", "返回", "退出", "設定", "清除縮圖", "關於", "登入", "登出", "分頁", "傳送訊息", "顯示資訊", "隱藏資訊"}},
};
static int active = 0; /* English until system locale is selected */
int vt_lang_current(void) {return active;}
int vt_lang_set(int index) {if(index<0||index>=VT_LANG_COUNT)return -1;active=index;return 0;}
int vt_lang_set_code(const char *code) {
  if(!code)return -1;
  for(int i=0;i<VT_LANG_COUNT;i++)if(strcmp(code,langs[i].code)==0)return vt_lang_set(i);
  return -1;
}
const char *vt_lang_code(int i) {return i>=0&&i<VT_LANG_COUNT?langs[i].code:"en";}
const char *vt_lang_name(int i) {return i>=0&&i<VT_LANG_COUNT?langs[i].name:"English";}
const char *vt_tr(vt_text_id id) {
  if(id<0||id>=VT_I_COUNT)return "";
  const char *v=langs[active].text[id];
  return v&&*v?v:langs[0].text[id];
}
int vt_lang_auto(int sys) {
  /* SceSystemParamLang values; this is pure C to retain host testability.
   * Order: ja en fr es de it nl pt-PT ru ko zh-TW zh-CN fi sv da no pl pt-BR en-GB tr. */
  const int mapping[]={15,0,3,1,5,4,10,2,6,16,18,17,0,14,0,0,11,2,0,12};
  if(sys<0||sys>=(int)(sizeof(mapping)/sizeof(mapping[0])))return -1;
  return vt_lang_set(mapping[sys]);
}

/* Legacy source keys are stable; never use Spanish as an implicit fallback. */
static const struct {const char *source,*en,*pt;} messages[] = {
  {"Sin conexion a Internet. Conectate a una red Wi-Fi.","No internet connection. Connect to Wi-Fi.","Sem conexão com a internet. Conecte-se a uma rede Wi-Fi."},
  {"%s espectadores","%s viewers","%s espectadores"},
  {"Ir al canal exacto","Open exact channel","Abrir canal pelo nome"},
  {"Inicia sesion para ver los canales que sigues","Log in to see followed channels","Entre na sua conta para ver os canais que você segue"},
  {"Pulsa SELECT y sigue las instrucciones (no hace falta escribir la contrasena en la Vita)","Press SELECT and follow the instructions. No password needed on Vita.","Pressione SELECT e siga as instruções. Não é preciso digitar a senha no Vita."},
  {"No se pudo cargar","Unable to load","Não foi possível carregar"},
  {"Pulsa %s para buscar un canal por nombre","Press %s to search by channel name","Pressione %s para buscar um canal pelo nome"},
  {"Busca canales de Twitch","Search Twitch channels","Buscar canais da Twitch"},
  {"Ninguno de tus canales esta en directo","None of your followed channels are live","Nenhum canal que você segue está ao vivo"},
  {"Sin resultados","No results","Nenhum resultado"},
  {"No hay directos","No live streams","Nenhuma transmissão ao vivo"},
  {"Pulsa Triangulo para actualizar","Press Triangle to refresh","Pressione Triângulo para atualizar"},
  {"Aceptar","OK","Confirmar"},
  {"Cancelar","Cancel","Cancelar"},
  {"Este canal no ofrece 480p/720p: se muestran las disponibles.","480p/720p unavailable; showing available qualities.","Este canal não oferece 480p/720p. Exibindo as qualidades disponíveis."},
  {"Sin conexion","No connection","Sem conexão"},
  {"Conecta la Vita a una red Wi-Fi e intentalo de nuevo.","Connect to Wi-Fi and try again.","Conecte o Vita a uma rede Wi-Fi e tente novamente."},
  {"Abriendo %s...","Opening %s...","Abrindo %s..."},
  {"No se pudo abrir el canal","Unable to open channel","Não foi possível abrir o canal"},
  {"Sin video","No video","Sem vídeo"},
  {"El stream no tiene calidades de video compatibles.","No compatible video qualities.","Não há qualidades de vídeo compatíveis."},
  {"Reproduccion detenida","Playback stopped","Reprodução interrompida"},
  {"Has cerrado sesion","Logged out","Você saiu da conta"},
  {"Sesion iniciada: Siguiendo y anuncios segun tu cuenta.","Signed in. Follows and ads depend on your account.","Sessão iniciada. Canais seguidos e anúncios conforme sua conta."},
  {"Cerrar sesion","Log out","Sair da conta"},
  {"Volver","Back","Voltar"},
  {"Conecta la Vita a una red Wi-Fi para iniciar sesion.","Connect to Wi-Fi to log in.","Conecte o Vita ao Wi-Fi para entrar na conta."},
  {"Pidiendo un codigo a Twitch...","Requesting Twitch code...","Solicitando código à Twitch..."},
  {"No se pudo iniciar sesion","Unable to log in","Não foi possível entrar na conta"},
  {"Hola, %s","Hello, %s","Olá, %s"},
  {"Codigo caducado","Code expired","Código expirado"},
  {"El codigo ha caducado. Pulsa SELECT para pedir otro.","Code expired. Press SELECT for another.","O código expirou. Pressione SELECT para solicitar outro."},
  {"Iniciar sesion en Twitch","Log in to Twitch","Entrar na Twitch"},
  {"1. En tu movil o PC abre:","1. On your phone or PC, open:","1. No celular ou computador, acesse:"},
  {"2. Inicia sesion e introduce este codigo:","2. Log in and enter this code:","2. Entre na sua conta e digite este código:"},
  {"Esperando...","Waiting...","Aguardando..."},
  {"El codigo caduca en %llu:%02llu","Code expires in %llu:%02llu","O código expira em %llu:%02llu"},
  {"Up/Down  |  X  |  Back","Up/Down  |  X  |  Back","Cima/Baixo  |  X  |  Voltar"},
  {"Thumbnail cache cleared","Thumbnail cache cleared","Cache de miniaturas limpo"},
  {"19 languages, touchscreen, Twitch chat, hardware video. Independent VitaTwitch fork.","19 languages, touch, Twitch chat and hardware video. Independent fork.","19 idiomas, controles por toque, chat da Twitch e vídeo por hardware. Fork independente."},
  {"Buscar canal","Search channel","Buscar canal"},
  {"Ese nombre no es un canal valido","Invalid channel name","Nome de canal inválido"},
  {"No se pudo iniciar el decodificador H.264 (0x%08X)","Unable to start H.264 decoder (0x%08X)","Não foi possível iniciar o decodificador H.264 (0x%08X)"},
  {"Error del decodificador H.264 (0x%08X)","H.264 decoder error (0x%08X)","Erro no decodificador H.264 (0x%08X)"},
  {"Sin memoria para las texturas de video","Not enough video texture memory","Memória insuficiente para as texturas de vídeo"},
  {"No se pudo crear el hilo de video","Unable to start video thread","Não foi possível iniciar o processamento de vídeo"},
  {"DIRECTO","LIVE","AO VIVO"},
  {"%s   buffer %.1f s   descartados %d","%s   buffer %.1f s   dropped %d","%s   buffer %.1f s   descartados %d"},
  {"O: salir    Triangulo: info    Cuadrado: chat","O: exit    Triangle: info    Square: chat","O: sair    Triângulo: info    Quadrado: chat"},
  {"X: salir    Triangulo: info    Cuadrado: chat","X: exit    Triangle: info    Square: chat","X: sair    Triângulo: info    Quadrado: chat"},
  {"No se pudo crear el hilo de descarga","Unable to start download thread","Não foi possível iniciar o download"},
  {"Message queued (server confirmation pending)","Message queued (server confirmation pending)","Mensagem na fila. Aguardando confirmação do servidor."},
  {"Please slow down (chat rate limit)","Please slow down (chat rate limit)","Aguarde antes de enviar outra mensagem."},
  {"Message empty or too long","Message empty or too long","Mensagem vazia ou longa demais."},
  {"Cannot open PS Vita keyboard","Cannot open PS Vita keyboard","Não foi possível abrir o teclado do PS Vita."},
  {"El stream ha terminado","The stream has ended","A transmissão terminou"},
  {"Anuncio de Twitch: saltando...","Twitch ad: waiting for stream...","Anúncio da Twitch: aguardando a transmissão..."},
  {"Cargando...","Loading...","Carregando..."},
  {"Twitch rechazo el inicio de sesion: %s","Twitch rejected login: %s","A Twitch recusou o login: %s"},
  {"respuesta no valida","invalid response","resposta inválida"},
  {"El codigo ha caducado. Vuelve a intentarlo.","The code expired. Try again.","O código expirou. Tente novamente."},
  {"Has rechazado el acceso en Twitch.","You denied access on Twitch.","Você negou o acesso na Twitch."},
  {"Error de Twitch: %s","Twitch error: %s","Erro da Twitch: %s"},
  {"Error de red al contactar id.twitch.tv","Network error contacting id.twitch.tv","Erro de rede ao acessar id.twitch.tv"},
  {"La sesion ha caducado. Vuelve a iniciar sesion.","Session expired. Log in again.","A sessão expirou. Entre na conta novamente."},
  {"No se pudo comprobar la sesion (sin conexion?)","Unable to verify session. Check connection.","Não foi possível verificar a sessão. Confira a conexão."},
  {"Respuesta de Twitch no valida","Invalid Twitch response","Resposta inválida da Twitch"},
  {"Sesion caducada: vuelve a iniciar sesion","Session expired: log in again","Sessão expirada: entre novamente"},
  {"Twitch: %s","Twitch: %s","Twitch: %s"},
  {"respuesta sin lista","response without list","resposta sem lista de canais"},
  {"No se pudo iniciar curl","Unable to initialize network client","Não foi possível iniciar a conexão de rede"},
  {"Error de red al contactar con Twitch","Network error contacting Twitch","Erro de rede ao acessar a Twitch"},
  {"gql.twitch.tv respondio HTTP %ld","gql.twitch.tv returned HTTP %ld","gql.twitch.tv retornou HTTP %ld"},
  {"Inicia sesion para ver tus canales seguidos","Log in to view followed channels","Entre na conta para ver os canais que você segue"},
  {"fallo antes en esta sesion","Earlier failure in this session","Falha anterior nesta sessão"},
  {"Busqueda demasiado larga","Search too long","Busca longa demais"},
  {"Nombre de canal no valido","Invalid channel name","Nome de canal inválido"},
  {"El canal '%s' no existe","Channel '%s' does not exist","O canal '%s' não existe"},
  {"Error de red al contactar usher.ttvnw.net","Network error contacting usher.ttvnw.net","Erro de rede ao acessar usher.ttvnw.net"},
  {"'%s' no esta en directo","'%s' is not live","'%s' não está ao vivo"},
  {"Twitch no permite ver este canal (bloqueo regional o solo suscriptores)","Twitch restricts this channel (region or subscribers only)","A Twitch restringe este canal (região ou apenas inscritos)"},
  {"usher respondio HTTP %ld","usher returned HTTP %ld","usher retornou HTTP %ld"},
  {"No se encontraron calidades en la playlist","No qualities found in playlist","Nenhuma qualidade encontrada na lista de reprodução"},
  {"Stream terminado (HTTP %ld)","Stream unavailable (HTTP %ld)","Transmissão indisponível (HTTP %ld)"},
  {"El directo ha terminado","The live stream has ended","A transmissão ao vivo terminou"},
  {"Chat de %s","%s chat","Chat de %s"},
  {"Cargando insignias...","Loading badges...","Carregando emblemas..."},
  {"Cargando emotes de 7TV...","Loading 7TV emotes...","Carregando emotes do 7TV..."},
  {"Conectado - 7TV: %d emotes del canal","Connected - 7TV: %d channel emotes","Conectado - 7TV: %d emotes do canal"},
  {"Conectado - 7TV: solo globales","Connected - 7TV: global only","Conectado - 7TV: apenas emotes globais"},
  {"Conectado","Connected","Conectado"},
  {"Conectando al chat...","Connecting to chat...","Conectando ao chat..."},
  {"Secure chat TLS unavailable; anonymous read-only","Secure chat unavailable; read-only","Conexão segura indisponível; chat somente para leitura"},
  {"Failed to send chat message","Failed to send chat message","Falha ao enviar mensagem"},
  {"Conectado al chat","Connected to chat","Conectado ao chat"},
  {"El chat ha sido borrado por un moderador","Chat cleared by a moderator","O chat foi limpo por um moderador"},
  {"Chat desconectado, reintentando en %d s...","Chat disconnected, retrying in %d s...","Chat desconectado. Reconectando em %d s..."},
};
static const char *const ext_0[] = {
#include "locales/es.inc"
};
_Static_assert(sizeof(ext_0)/sizeof(ext_0[0]) == sizeof(messages)/sizeof(messages[0]), "locale es count");
static const char *const ext_1[] = {
#include "locales/fr.inc"
};
_Static_assert(sizeof(ext_1)/sizeof(ext_1[0]) == sizeof(messages)/sizeof(messages[0]), "locale fr count");
static const char *const ext_2[] = {
#include "locales/it.inc"
};
_Static_assert(sizeof(ext_2)/sizeof(ext_2[0]) == sizeof(messages)/sizeof(messages[0]), "locale it count");
static const char *const ext_3[] = {
#include "locales/de.inc"
};
_Static_assert(sizeof(ext_3)/sizeof(ext_3[0]) == sizeof(messages)/sizeof(messages[0]), "locale de count");
static const char *const ext_4[] = {
#include "locales/ru.inc"
};
_Static_assert(sizeof(ext_4)/sizeof(ext_4[0]) == sizeof(messages)/sizeof(messages[0]), "locale ru count");
static const char *const ext_5[] = {
#include "locales/id.inc"
};
_Static_assert(sizeof(ext_5)/sizeof(ext_5[0]) == sizeof(messages)/sizeof(messages[0]), "locale id count");
static const char *const ext_6[] = {
#include "locales/fil.inc"
};
_Static_assert(sizeof(ext_6)/sizeof(ext_6[0]) == sizeof(messages)/sizeof(messages[0]), "locale fil count");
static const char *const ext_7[] = {
#include "locales/ceb.inc"
};
_Static_assert(sizeof(ext_7)/sizeof(ext_7[0]) == sizeof(messages)/sizeof(messages[0]), "locale ceb count");
static const char *const ext_8[] = {
#include "locales/nl.inc"
};
_Static_assert(sizeof(ext_8)/sizeof(ext_8[0]) == sizeof(messages)/sizeof(messages[0]), "locale nl count");
static const char *const ext_9[] = {
#include "locales/pl.inc"
};
_Static_assert(sizeof(ext_9)/sizeof(ext_9[0]) == sizeof(messages)/sizeof(messages[0]), "locale pl count");
static const char *const ext_10[] = {
#include "locales/tr.inc"
};
_Static_assert(sizeof(ext_10)/sizeof(ext_10[0]) == sizeof(messages)/sizeof(messages[0]), "locale tr count");
static const char *const ext_11[] = {
#include "locales/uk.inc"
};
_Static_assert(sizeof(ext_11)/sizeof(ext_11[0]) == sizeof(messages)/sizeof(messages[0]), "locale uk count");
static const char *const ext_12[] = {
#include "locales/sv.inc"
};
_Static_assert(sizeof(ext_12)/sizeof(ext_12[0]) == sizeof(messages)/sizeof(messages[0]), "locale sv count");
static const char *const ext_13[] = {
#include "locales/ja.inc"
};
_Static_assert(sizeof(ext_13)/sizeof(ext_13[0]) == sizeof(messages)/sizeof(messages[0]), "locale ja count");
static const char *const ext_14[] = {
#include "locales/ko.inc"
};
_Static_assert(sizeof(ext_14)/sizeof(ext_14[0]) == sizeof(messages)/sizeof(messages[0]), "locale ko count");
static const char *const ext_15[] = {
#include "locales/zh-CN.inc"
};
_Static_assert(sizeof(ext_15)/sizeof(ext_15[0]) == sizeof(messages)/sizeof(messages[0]), "locale zh-CN count");
static const char *const ext_16[] = {
#include "locales/zh-TW.inc"
};
_Static_assert(sizeof(ext_16)/sizeof(ext_16[0]) == sizeof(messages)/sizeof(messages[0]), "locale zh-TW count");
static const char *const *const extended[VT_LANG_COUNT] = {
  [1]=ext_0,
  [3]=ext_1,
  [4]=ext_2,
  [5]=ext_3,
  [6]=ext_4,
  [7]=ext_5,
  [8]=ext_6,
  [9]=ext_7,
  [10]=ext_8,
  [11]=ext_9,
  [12]=ext_10,
  [13]=ext_11,
  [14]=ext_12,
  [15]=ext_13,
  [16]=ext_14,
  [17]=ext_15,
  [18]=ext_16,
};
size_t vt_msg_count(void) { return sizeof(messages)/sizeof(messages[0]); }
const char *vt_msg_key(size_t i) { return i<vt_msg_count()?messages[i].source:NULL; }
const char *vt_msg(const char *source) {
  if(!source)return "";
  for(size_t i=0;i<sizeof(messages)/sizeof(messages[0]);i++){
    if(!strcmp(source,messages[i].source)){
      if(active==2)return messages[i].pt;
      if(extended[active])return extended[active][i];
      return messages[i].en;
    }
  }
  return source;
}

static const char *const options[VT_LANG_COUNT][VT_O_COUNT] = {
{"Playback profile","Economy","Balanced","Low latency","Quality","Auto quality","Diagnostics","Volume","Reconnecting...","Lowering quality...","Remember account","FPS","Throughput","CPU","Rebuffers","Buffer","Dropped frames"},
{"Perfil de reproducción","Ahorro","Equilibrado","Baja latencia","Calidad","Calidad automática","Diagnóstico","Volumen","Reconectando...","Reduciendo calidad...","Recordar cuenta","FPS","Caudal","CPU","Pausas","Búfer","Fotogramas descartados"},
{"Perfil de reprodução","Econômico","Equilibrado","Baixa latência","Qualidade","Qualidade automática","Diagnóstico","Volume","Reconectando...","Reduzindo a qualidade...","Lembrar conta","FPS","Taxa de download","CPU","Pausas","Buffer","Frames descartados"},
{"Profil de lecture","Économie","Équilibré","Faible latence","Qualité","Qualité auto","Diagnostic","Volume","Reconnexion...","Réduction de qualité...","Mémoriser le compte","IPS","Débit","CPU","Pauses","Tampon","Images perdues"},
{"Profilo riproduzione","Risparmio","Bilanciato","Bassa latenza","Qualità","Qualità automatica","Diagnostica","Volume","Riconnessione...","Riduzione qualità...","Ricorda account","FPS","Velocità","CPU","Pause","Buffer","Frame scartati"},
{"Wiedergabeprofil","Sparmodus","Ausgewogen","Niedrige Latenz","Qualität","Auto-Qualität","Diagnose","Lautstärke","Verbindung wird erneuert...","Qualität wird reduziert...","Konto merken","FPS","Durchsatz","CPU","Pausen","Puffer","Verworfene Bilder"},
{"Профиль воспроизведения","Экономия","Баланс","Низкая задержка","Качество","Автокачество","Диагностика","Громкость","Переподключение...","Снижение качества...","Запомнить аккаунт","FPS","Скорость","CPU","Паузы","Буфер","Пропущенные кадры"},
{"Profil pemutaran","Hemat","Seimbang","Latensi rendah","Kualitas","Kualitas otomatis","Diagnostik","Volume","Menghubungkan ulang...","Menurunkan kualitas...","Ingat akun","FPS","Kecepatan","CPU","Jeda","Buffer","Frame terbuang"},
{"Profile ng playback","Matipid","Balanse","Mababang latency","Kalidad","Auto quality","Diagnostics","Volume","Kumokonekta muli...","Ibinababa ang kalidad...","Tandaan ang account","FPS","Bilis","CPU","Mga paghinto","Buffer","Nalaktawang frame"},
{"Profile sa playback","Tipid","Balanse","Ubos nga latency","Kalidad","Auto quality","Diagnostics","Volume","Nagkonektar pag-usab...","Gipaubos ang kalidad...","Hinumdomi ang account","FPS","Katulin","CPU","Mga paghunong","Buffer","Gilaktawang frame"},
{"Afspeelprofiel","Zuinig","Gebalanceerd","Lage latentie","Kwaliteit","Auto-kwaliteit","Diagnose","Volume","Opnieuw verbinden...","Kwaliteit verlagen...","Account onthouden","FPS","Doorvoer","CPU","Pauzes","Buffer","Gemiste frames"},
{"Profil odtwarzania","Oszczędny","Zrównoważony","Niskie opóźnienie","Jakość","Auto jakość","Diagnostyka","Głośność","Ponowne łączenie...","Obniżanie jakości...","Zapamiętaj konto","FPS","Przepływność","CPU","Przerwy","Bufor","Pominięte klatki"},
{"Oynatma profili","Tasarruf","Dengeli","Düşük gecikme","Kalite","Otomatik kalite","Tanılama","Ses düzeyi","Yeniden bağlanıyor...","Kalite düşürülüyor...","Hesabı hatırla","FPS","Aktarım hızı","CPU","Duraklama","Tampon","Atlanan kare"},
{"Профіль відтворення","Економія","Баланс","Низька затримка","Якість","Автоякість","Діагностика","Гучність","Перепідключення...","Зниження якості...","Запам’ятати акаунт","FPS","Швидкість","CPU","Паузи","Буфер","Пропущені кадри"},
{"Uppspelningsprofil","Sparläge","Balanserat","Låg latens","Kvalitet","Automatisk kvalitet","Diagnostik","Volym","Återansluter...","Sänker kvalitet...","Kom ihåg konto","FPS","Hastighet","CPU","Pauser","Buffert","Tappade bildrutor"},
{"再生プロファイル","省電力","バランス","低遅延","高画質","自動画質","診断","音量","再接続中...","画質を下げています...","アカウントを保存","FPS","転送速度","CPU","停止回数","バッファ","破棄フレーム"},
{"재생 프로필","절전","균형","낮은 지연","고화질","자동 화질","진단","음량","재연결 중...","화질 낮추는 중...","계정 기억","FPS","전송 속도","CPU","버퍼링 횟수","버퍼","누락 프레임"},
{"播放模式","省电","均衡","低延迟","画质","自动画质","诊断","音量","正在重连...","正在降低画质...","记住账号","FPS","下载速率","CPU","缓冲次数","缓冲","丢帧"},
{"播放模式","省電","平衡","低延遲","畫質","自動畫質","診斷","音量","正在重新連線...","正在降低畫質...","記住帳戶","FPS","下載速率","CPU","緩衝次數","緩衝","捨棄影格"},
};
const char *vt_opt(vt_option_id key) { return key>=0 && key<VT_O_COUNT ? options[active][key] : ""; }
