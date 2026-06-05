#ifndef PINBACK_URL_H
#define PINBACK_URL_H

#include <stddef.h>

#define PINBACK_URL_MAX 2048

/* Default loopback cockpit URL (desktop self-host). */
const char *pinback_url_default(void);

/* $PINBACK_URL if set, else NULL. */
const char *pinback_url_from_env(void);

/* Load persisted URL into out (no trailing slash). Returns 1 if a value was stored. */
int pinback_url_load_saved(char *out, size_t cap);

/* Persist URL (normalized). Returns 0 on success. */
int pinback_url_save(const char *url);

/* env → saved → fallback_default. Normalizes into out. Returns 0 on success. */
int pinback_url_resolve(char *out, size_t cap, const char *fallback_default);

/* GET /healthz on the given http(s) base URL. */
int pinback_health_ok(const char *url);

/* file:// URI to bundled setup.html (executable dir, bundle Resources, etc.). */
int pinback_setup_file_uri(char *out, size_t cap);

#endif
