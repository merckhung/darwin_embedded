#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { PLIST_DICT, PLIST_ARRAY, PLIST_STRING, PLIST_INTEGER, PLIST_BOOL, PLIST_DATA } PlistType;

typedef struct PlistNode PlistNode;
struct PlistNode {
  PlistType type;
  union {
    char *string;
    uint64_t integer;   // also holds 0/1 for PLIST_BOOL
    uint8_t *data;      // PLIST_DATA raw bytes
  };
  size_t data_len;      // PLIST_DATA byte count
  char *key;
  PlistNode *next;
  PlistNode *child;
};

PlistNode *plist_new_data(const uint8_t *bytes, size_t len);

PlistNode *plist_parse(const char *buf, size_t len);
void plist_free(PlistNode *n);
PlistNode *plist_dict_get(PlistNode *dict, const char *key);
const char *plist_dict_get_str(PlistNode *dict, const char *key);

PlistNode *plist_new_dict(void);
PlistNode *plist_new_array(void);
PlistNode *plist_new_string(const char *s);
PlistNode *plist_new_integer(uint64_t v);
void plist_dict_set(PlistNode *dict, const char *key, PlistNode *value);
void plist_array_append(PlistNode *array, PlistNode *value);

char *plist_write_xml(PlistNode *root, size_t *out_len);
