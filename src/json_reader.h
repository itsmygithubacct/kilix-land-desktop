#ifndef KILIX_LAND_DESKTOP_JSON_READER_H
#define KILIX_LAND_DESKTOP_JSON_READER_H

#include <stdbool.h>
#include <stddef.h>

/* Bounded streaming reader for the strict JSON subset shared by the
 * desktop's data files: objects, arrays, strings with the \" \\ \/ \n \t
 * escapes, numbers with an optional fraction, and true/false. Unknown keys
 * are the schema parser's business; this layer owns tokens only. Errors are
 * reported as "<name>:<byte-offset>: <what>". The struct is transparent so
 * schema parsers can capture offsets for their own error messages. */
typedef struct desk_json_reader {
    const char *text;
    size_t length;
    size_t offset;
    size_t key_offset; /* start of the most recent object key */
    const char *name;  /* basename used in error prefixes */
    char *error;
    size_t error_size;
} desk_json_reader;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
bool desk_json_fail_at(desk_json_reader *reader, size_t offset,
                       const char *format, ...);

/* Reads path into buffer and leaves the reader at offset 0. buffer_size
 * must include one spare byte: a file longer than buffer_size - 1 bytes is
 * rejected, never truncated. On failure the error buffer holds the reason
 * prefixed with basename(path) (or fallback_name when path is NULL). */
bool desk_json_open(desk_json_reader *reader, const char *path,
                    const char *fallback_name, char *buffer,
                    size_t buffer_size, char *error, size_t error_size);

void desk_json_skip_ws(desk_json_reader *reader);
bool desk_json_expect(desk_json_reader *reader, char expected);
bool desk_json_parse_string(desk_json_reader *reader, char *out,
                            size_t capacity);
bool desk_json_parse_number(desk_json_reader *reader, float *out);
bool desk_json_parse_bool(desk_json_reader *reader, bool *out);
/* 1 = key parsed (name + ':' consumed), 0 = object closed, -1 = error. */
int desk_json_next_key(desk_json_reader *reader, bool *first, char *key,
                       size_t capacity);
/* 1 = element follows, 0 = array closed, -1 = error. */
int desk_json_next_element(desk_json_reader *reader, bool *first);
bool desk_json_claim_key(desk_json_reader *reader, unsigned *seen,
                         unsigned bit, const char *key);

#endif
