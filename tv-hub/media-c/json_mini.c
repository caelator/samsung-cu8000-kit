#include "json_mini.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int depth;
} JParser;

static void skip_ws(JParser *jp) {
    while (*jp->p && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' || *jp->p == '\n')) jp->p++;
}

static JVal *parse_val(JParser *jp);

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_hex4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_val(s[i]);
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static size_t encode_utf8(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static char *parse_str_raw(JParser *jp) {
    if (*jp->p != '"') return NULL;
    jp->p++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (*jp->p && *jp->p != '"') {
        if (len + 8 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (*jp->p == '\\') {
            jp->p++;
            char c = *jp->p++;
            switch (c) {
                case '"':  buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(jp->p, &cp)) { free(buf); return NULL; }
                    jp->p += 4;
                    /* Surrogate pair */
                    if (cp >= 0xD800 && cp <= 0xDBFF && jp->p[0] == '\\' && jp->p[1] == 'u') {
                        uint32_t lo = 0;
                        if (parse_hex4(jp->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            jp->p += 6;
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        }
                    }
                    len += encode_utf8(cp, buf + len);
                    break;
                }
                default:
                    buf[len++] = c;
                    break;
            }
        } else {
            buf[len++] = *jp->p++;
        }
    }
    if (*jp->p != '"') { free(buf); return NULL; }
    jp->p++;
    buf[len] = '\0';
    return buf;
}

static JVal *new_val(JType t) {
    JVal *v = calloc(1, sizeof(JVal));
    if (v) v->type = t;
    return v;
}

static JVal *parse_arr(JParser *jp) {
    jp->p++; /* '[' */
    JVal *v = new_val(J_ARR);
    if (!v) return NULL;
    size_t cap = 0;
    skip_ws(jp);
    if (*jp->p == ']') { jp->p++; return v; }
    for (;;) {
        skip_ws(jp);
        JVal *it = parse_val(jp);
        if (!it) { json_free(v); return NULL; }
        if (v->count == cap) {
            cap = cap ? cap * 2 : 8;
            JVal **ni = realloc(v->items, cap * sizeof(JVal *));
            if (!ni) { json_free(it); json_free(v); return NULL; }
            v->items = ni;
        }
        v->items[v->count++] = it;
        skip_ws(jp);
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == ']') { jp->p++; return v; }
        json_free(v);
        return NULL;
    }
}

static JVal *parse_obj(JParser *jp) {
    jp->p++; /* '{' */
    JVal *v = new_val(J_OBJ);
    if (!v) return NULL;
    size_t cap = 0;
    skip_ws(jp);
    if (*jp->p == '}') { jp->p++; return v; }
    for (;;) {
        skip_ws(jp);
        char *k = parse_str_raw(jp);
        if (!k) { json_free(v); return NULL; }
        skip_ws(jp);
        if (*jp->p != ':') { free(k); json_free(v); return NULL; }
        jp->p++;
        skip_ws(jp);
        JVal *val = parse_val(jp);
        if (!val) { free(k); json_free(v); return NULL; }
        if (v->count == cap) {
            cap = cap ? cap * 2 : 8;
            char **nk = realloc(v->keys, cap * sizeof(char *));
            JVal **ni = realloc(v->items, cap * sizeof(JVal *));
            if (!nk || !ni) {
                if (nk) v->keys = nk;
                if (ni) v->items = ni;
                free(k); json_free(val); json_free(v); return NULL;
            }
            v->keys = nk;
            v->items = ni;
        }
        v->keys[v->count] = k;
        v->items[v->count] = val;
        v->count++;
        skip_ws(jp);
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == '}') { jp->p++; return v; }
        json_free(v);
        return NULL;
    }
}

static JVal *parse_val(JParser *jp) {
    skip_ws(jp);
    if (jp->depth > 48) return NULL;
    char c = *jp->p;
    if (c == '"') {
        char *s = parse_str_raw(jp);
        if (!s) return NULL;
        JVal *v = new_val(J_STR);
        if (!v) { free(s); return NULL; }
        v->sval = s;
        return v;
    }
    if (c == '{') { jp->depth++; JVal *v = parse_obj(jp); jp->depth--; return v; }
    if (c == '[') { jp->depth++; JVal *v = parse_arr(jp); jp->depth--; return v; }
    if (strncmp(jp->p, "true", 4) == 0) { jp->p += 4; JVal *v = new_val(J_BOOL); if (v) v->bval = true; return v; }
    if (strncmp(jp->p, "false", 5) == 0) { jp->p += 5; JVal *v = new_val(J_BOOL); if (v) v->bval = false; return v; }
    if (strncmp(jp->p, "null", 4) == 0) { jp->p += 4; return new_val(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end = NULL;
        double d = strtod(jp->p, &end);
        if (end == jp->p) return NULL;
        jp->p = end;
        JVal *v = new_val(J_NUM);
        if (v) v->nval = d;
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *src) {
    if (!src) return NULL;
    JParser jp = { .p = src, .depth = 0 };
    return parse_val(&jp);
}

void json_free(JVal *v) {
    if (!v) return;
    if (v->type == J_STR) free(v->sval);
    else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->count; i++) json_free(v->items[i]);
        free(v->items);
    } else if (v->type == J_OBJ) {
        for (size_t i = 0; i < v->count; i++) {
            free(v->keys[i]);
            json_free(v->items[i]);
        }
        free(v->keys);
        free(v->items);
    }
    free(v);
}

const JVal *json_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

const JVal *json_at(const JVal *arr, size_t idx) {
    if (!arr || arr->type != J_ARR || idx >= arr->count) return NULL;
    return arr->items[idx];
}

const char *json_str(const JVal *v, const char *def) {
    return (v && v->type == J_STR && v->sval) ? v->sval : def;
}

int64_t json_i64(const JVal *v, int64_t def) {
    return (v && v->type == J_NUM) ? (int64_t)v->nval : def;
}

double json_f64(const JVal *v, double def) {
    return (v && v->type == J_NUM) ? v->nval : def;
}

bool json_bool(const JVal *v, bool def) {
    return (v && v->type == J_BOOL) ? v->bval : def;
}
