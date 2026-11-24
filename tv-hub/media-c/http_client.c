#include "http_client.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
    r->status = 0;
}

void http_url_encode(const char *src, char *dst, size_t dst_sz) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; src && *src && o + 4 < dst_sz; src++) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = HEX[c >> 4];
            dst[o++] = HEX[c & 0x0F];
        }
    }
    if (dst_sz > 0) dst[o] = '\0';
}

static bool parse_url(const char *url, char *host, size_t host_sz, int *port, const char **path_out) {
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : (p + strlen(p));
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl == 0 || hl >= host_sz) return false;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hl = (size_t)(host_end - p);
        if (hl == 0 || hl >= host_sz) return false;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = 80;
    }
    *path_out = slash ? slash : "/";
    return *port > 0 && *port <= 65535;
}

static bool send_all(int fd, const uint8_t *buf, size_t len, int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return false;
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static uint8_t *decode_chunked(const uint8_t *src, size_t src_len, size_t max_bytes, size_t *out_len) {
    uint8_t *dst = malloc(src_len + 1);
    if (!dst) return NULL;
    size_t pos = 0, out = 0;
    while (pos < src_len) {
        const uint8_t *crlf = memchr(src + pos, '\n', src_len - pos);
        if (!crlf) break;
        size_t line_len = (size_t)(crlf - (src + pos));
        char hexbuf[32];
        size_t copy_n = line_len < sizeof(hexbuf) - 1 ? line_len : sizeof(hexbuf) - 1;
        memcpy(hexbuf, src + pos, copy_n);
        hexbuf[copy_n] = '\0';
        unsigned long chunk_sz = strtoul(hexbuf, NULL, 16);
        pos = (size_t)(crlf - src) + 1;
        if (chunk_sz == 0) break;
        if (pos + chunk_sz > src_len) chunk_sz = src_len - pos;
        if (out + chunk_sz > max_bytes) {
            free(dst);
            return NULL;
        }
        memcpy(dst + out, src + pos, chunk_sz);
        out += chunk_sz;
        pos += chunk_sz;
        if (pos < src_len && src[pos] == '\r') pos++;
        if (pos < src_len && src[pos] == '\n') pos++;
    }
    dst[out] = '\0';
    *out_len = out;
    return dst;
}

bool http_request(const char *method,
                  const char *url,
                  const char *req_body,
                  const char *content_type,
                  int timeout_ms,
                  size_t max_bytes,
                  HttpResponse *out) {
    memset(out, 0, sizeof(*out));
    char host[128];
    int port = 80;
    const char *path = "/";
    if (!parse_url(url, host, sizeof(host), &port, &path)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) { close(fd); return false; }
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return false;
    }

    size_t body_len = req_body ? strlen(req_body) : 0;
    char hdr[1536];
    int hlen;
    if (body_len > 0) {
        hlen = snprintf(hdr, sizeof(hdr),
                        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
                        "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                        method, path, host, port,
                        content_type ? content_type : "application/json", body_len);
    } else {
        hlen = snprintf(hdr, sizeof(hdr),
                        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                        method, path, host, port);
    }
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) { close(fd); return false; }
    if (!send_all(fd, (const uint8_t *)hdr, (size_t)hlen, timeout_ms)) { close(fd); return false; }
    if (body_len > 0 && !send_all(fd, (const uint8_t *)req_body, body_len, timeout_ms)) { close(fd); return false; }

    size_t cap = 65536;
    size_t total = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); return false; }

    size_t hard_cap = max_bytes + 8192;
    for (;;) {
        if (total + 16384 > cap) {
            size_t ncap = cap * 2;
            if (ncap > hard_cap) ncap = hard_cap;
            if (total >= ncap) break;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return false; }
            buf = nb;
            cap = ncap;
        }
        ssize_t n = recv(fd, buf + total, cap - total - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return false;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';

    /* Split header / body */
    const char *sep = strstr((const char *)buf, "\r\n\r\n");
    if (!sep) { free(buf); return false; }
    size_t hdr_bytes = (size_t)(sep - (const char *)buf);
    size_t raw_body_off = hdr_bytes + 4;
    size_t raw_body_len = total > raw_body_off ? total - raw_body_off : 0;

    int status = 0;
    sscanf((const char *)buf, "HTTP/%*d.%*d %d", &status);
    out->status = status;

    /* Check Transfer-Encoding: chunked (case-insensitive within header only) */
    bool chunked = false;
    char *hdr_copy = malloc(hdr_bytes + 1);
    if (hdr_copy) {
        memcpy(hdr_copy, buf, hdr_bytes);
        hdr_copy[hdr_bytes] = '\0';
        for (size_t i = 0; i < hdr_bytes; i++) hdr_copy[i] = (char)tolower((unsigned char)hdr_copy[i]);
        if (strstr(hdr_copy, "transfer-encoding: chunked") != NULL) chunked = true;
        free(hdr_copy);
    }

    if (chunked) {
        out->body = decode_chunked(buf + raw_body_off, raw_body_len, max_bytes, &out->len);
        free(buf);
        return out->body != NULL;
    }

    uint8_t *body = malloc(raw_body_len + 1);
    if (!body) { free(buf); return false; }
    memcpy(body, buf + raw_body_off, raw_body_len);
    body[raw_body_len] = '\0';
    free(buf);
    out->body = body;
    out->len = raw_body_len;
    return true;
}
