#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int status;
    uint8_t *body;
    size_t len;
} HttpResponse;

/* Blocking HTTP/1.1 GET or POST over plain LAN TCP (works on ARMv7 Linux and macOS).
 * Caller must call http_response_free(&resp).
 * Supports http://host:port/path, Content-Length and chunked transfer encoding. */
bool http_request(const char *method,
                  const char *url,
                  const char *req_body,
                  const char *content_type,
                  int timeout_ms,
                  size_t max_bytes,
                  HttpResponse *out);

static inline bool http_get(const char *url, int timeout_ms, size_t max_bytes, HttpResponse *out) {
    return http_request("GET", url, NULL, NULL, timeout_ms, max_bytes, out);
}

static inline bool http_post_json(const char *url, const char *json_body, int timeout_ms, HttpResponse *out) {
    return http_request("POST", url, json_body, "application/json", timeout_ms, 262144, out);
}

void http_response_free(HttpResponse *r);

/* URL-encode query value into dst (NUL-terminated). */
void http_url_encode(const char *src, char *dst, size_t dst_sz);
