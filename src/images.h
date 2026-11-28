#ifndef VT_IMAGES_H
#define VT_IMAGES_H

#include <vita2d.h>

/* Asynchronous image cache (stream thumbnails, avatars). A worker thread
 * downloads; textures are created on the main thread in img_frame(). */

void img_init(void);
void img_shutdown(void);

/* Call once per frame, BEFORE vita2d_start_drawing(): creates textures for
 * finished downloads and frees evicted ones safely. */
void img_frame(void);

/* Returns the texture if loaded, otherwise queues the download and returns
 * NULL. Most recently requested images are downloaded first. */
vita2d_texture *img_get(const char *url);

/* Same, at lower priority, for items just off screen (call every frame to keep
 * them cached). */
void img_prefetch(const char *url);

/* changes whenever a downloaded image becomes a texture (or fails) */
unsigned img_version(void);

/* Player mode selects its own smaller cache budget. All frees happen
 * at the pre-draw GPU fence; img_free_trash is a compatibility no-op. */
void img_set_deferred_free(int on);
void img_free_trash(void);
/* counters since the last call (for the log) */
void img_stats(int *decoded, int *evicted, int *failed, int *freed);

/* Forget everything (e.g. Explorar refreshed: thumbnails change over time). */
void img_clear(void);

#endif
