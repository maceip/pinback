#ifndef PIN_STATIC_ASSETS_H
#define PIN_STATIC_ASSETS_H

/* Embedded static UI assets.
 *
 * The build embeds web/index.html, web/app.js, and web/app.css as
 * read-only byte arrays via tools/gen-static-assets.sh + xxd. At
 * runtime, --web-dir DIR overrides the embedded copy so iteration
 * does not require a recompile. */

#include <stddef.h>

typedef struct {
    const char           *path;       /* "/", "/assets/app.js", ... */
    const char           *content_type;
    const unsigned char  *data;
    size_t                len;
} pin_static_file;

extern const pin_static_file pin_static_files[];
extern const size_t          pin_static_count;

/* Look up a request path. Returns NULL on miss. */
const pin_static_file *pin_static_lookup(const char *path);

#endif
