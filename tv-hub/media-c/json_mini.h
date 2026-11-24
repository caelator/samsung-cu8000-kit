#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    bool bval;
    double nval;
    char *sval;
    JVal **items;
    char **keys;
    size_t count;
};

JVal *json_parse(const char *src);
void  json_free(JVal *v);

const JVal *json_get(const JVal *obj, const char *key);
const JVal *json_at(const JVal *arr, size_t idx);
const char *json_str(const JVal *v, const char *def);
int64_t     json_i64(const JVal *v, int64_t def);
double      json_f64(const JVal *v, double def);
bool        json_bool(const JVal *v, bool def);
