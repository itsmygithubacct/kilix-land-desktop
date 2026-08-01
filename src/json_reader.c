/* json_reader.c — bounded token primitives shared by the desktop's strict
 * JSON parsers. Moved verbatim from rooms.c so every data file (world
 * manifest today, item catalog later) accepts exactly the same language and
 * reports errors in the same "<name>:<offset>: <what>" shape. Schema
 * knowledge stays with the callers; this file never sees a key name it
 * treats specially.
 */

#include "json_reader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

bool desk_json_fail_at(desk_json_reader *reader, size_t offset,
                       const char *format, ...)
{
    char message[112];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (reader->error && reader->error_size > 0u)
        (void)snprintf(reader->error, reader->error_size, "%s:%zu: %s",
                       reader->name, offset, message);
    return false;
}

bool desk_json_open(desk_json_reader *reader, const char *path,
                    const char *fallback_name, char *buffer,
                    size_t buffer_size, char *error, size_t error_size)
{
    FILE *stream;
    size_t bytes;

    if (error && error_size > 0u)
        error[0] = '\0';
    if (!reader)
        return false;
    memset(reader, 0, sizeof *reader);
    reader->name = path ? path_basename(path) : fallback_name;
    reader->error = error;
    reader->error_size = error_size;
    if (!path)
        return desk_json_fail_at(reader, 0u, "no path given");
    if (!buffer || buffer_size < 1u)
        return desk_json_fail_at(reader, 0u, "no read buffer given");

    stream = fopen(path, "rb");
    if (!stream)
        return desk_json_fail_at(reader, 0u, "cannot open file");
    bytes = fread(buffer, 1u, buffer_size, stream);
    if (ferror(stream) != 0) {
        (void)fclose(stream);
        return desk_json_fail_at(reader, 0u, "read failed");
    }
    (void)fclose(stream);
    if (bytes > buffer_size - 1u)
        return desk_json_fail_at(reader, 0u, "file larger than %u bytes",
                                 (unsigned)(buffer_size - 1u));
    reader->text = buffer;
    reader->length = bytes;
    return true;
}

void desk_json_skip_ws(desk_json_reader *reader)
{
    while (reader->offset < reader->length) {
        char c = reader->text[reader->offset];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        reader->offset++;
    }
}

static char peek_char(const desk_json_reader *reader)
{
    return reader->offset < reader->length ? reader->text[reader->offset] :
                                             '\0';
}

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

bool desk_json_expect(desk_json_reader *reader, char expected)
{
    desk_json_skip_ws(reader);
    if (reader->offset >= reader->length ||
        reader->text[reader->offset] != expected)
        return desk_json_fail_at(reader, reader->offset, "expected '%c'",
                                 expected);
    reader->offset++;
    return true;
}

static bool parse_keyword(desk_json_reader *reader, const char *word)
{
    size_t word_length = strlen(word);

    if (reader->length - reader->offset < word_length ||
        strncmp(reader->text + reader->offset, word, word_length) != 0)
        return false;
    reader->offset += word_length;
    return true;
}

bool desk_json_parse_string(desk_json_reader *reader, char *out,
                            size_t capacity)
{
    size_t start;
    size_t len = 0u;

    desk_json_skip_ws(reader);
    start = reader->offset;
    if (reader->offset >= reader->length ||
        reader->text[reader->offset] != '"')
        return desk_json_fail_at(reader, reader->offset, "expected string");
    reader->offset++;
    for (;;) {
        char c;

        if (reader->offset >= reader->length)
            return desk_json_fail_at(reader, start, "unterminated string");
        c = reader->text[reader->offset];
        if (c == '"') {
            reader->offset++;
            break;
        }
        if ((unsigned char)c < 0x20u)
            return desk_json_fail_at(reader, reader->offset,
                                     "raw control character in string");
        if (c == '\\') {
            reader->offset++;
            if (reader->offset >= reader->length)
                return desk_json_fail_at(reader, start,
                                         "unterminated string");
            switch (reader->text[reader->offset]) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            default:
                return desk_json_fail_at(reader, reader->offset,
                                         "unsupported escape sequence");
            }
        }
        if (len + 1u >= capacity)
            return desk_json_fail_at(reader, start,
                                     "string longer than capacity %zu",
                                     capacity - 1u);
        out[len++] = c;
        reader->offset++;
    }
    out[len] = '\0';
    return true;
}

bool desk_json_parse_number(desk_json_reader *reader, float *out)
{
    char scratch[48];
    size_t start;
    size_t span;

    desk_json_skip_ws(reader);
    start = reader->offset;
    if (peek_char(reader) == '-')
        reader->offset++;
    if (!is_digit_char(peek_char(reader)))
        return desk_json_fail_at(reader, start, "expected number");
    while (is_digit_char(peek_char(reader)))
        reader->offset++;
    if (peek_char(reader) == '.') {
        reader->offset++;
        if (!is_digit_char(peek_char(reader)))
            return desk_json_fail_at(reader, reader->offset,
                                     "expected digit after decimal point");
        while (is_digit_char(peek_char(reader)))
            reader->offset++;
    }
    span = reader->offset - start;
    if (span >= sizeof scratch)
        return desk_json_fail_at(reader, start, "number too long");
    memcpy(scratch, reader->text + start, span);
    scratch[span] = '\0';
    *out = strtof(scratch, NULL);
    return true;
}

bool desk_json_parse_bool(desk_json_reader *reader, bool *out)
{
    size_t start;

    desk_json_skip_ws(reader);
    start = reader->offset;
    if (parse_keyword(reader, "true")) {
        *out = true;
        return true;
    }
    if (parse_keyword(reader, "false")) {
        *out = false;
        return true;
    }
    return desk_json_fail_at(reader, start, "expected true or false");
}

int desk_json_next_key(desk_json_reader *reader, bool *first, char *key,
                       size_t capacity)
{
    desk_json_skip_ws(reader);
    if (reader->offset >= reader->length) {
        (void)desk_json_fail_at(reader, reader->offset,
                                "unterminated object");
        return -1;
    }
    if (reader->text[reader->offset] == '}') {
        reader->offset++;
        return 0;
    }
    if (!*first && !desk_json_expect(reader, ','))
        return -1;
    *first = false;
    desk_json_skip_ws(reader);
    reader->key_offset = reader->offset;
    if (!desk_json_parse_string(reader, key, capacity))
        return -1;
    if (!desk_json_expect(reader, ':'))
        return -1;
    return 1;
}

int desk_json_next_element(desk_json_reader *reader, bool *first)
{
    desk_json_skip_ws(reader);
    if (reader->offset >= reader->length) {
        (void)desk_json_fail_at(reader, reader->offset,
                                "unterminated array");
        return -1;
    }
    if (reader->text[reader->offset] == ']') {
        reader->offset++;
        return 0;
    }
    if (!*first && !desk_json_expect(reader, ','))
        return -1;
    *first = false;
    return 1;
}

bool desk_json_claim_key(desk_json_reader *reader, unsigned *seen,
                         unsigned bit, const char *key)
{
    if (*seen & bit)
        return desk_json_fail_at(reader, reader->key_offset,
                                 "duplicate key '%s'", key);
    *seen |= bit;
    return true;
}
