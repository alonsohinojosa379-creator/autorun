/*
 * A strict reader of JSON from GitHub and the DLL repository: strings with
 * their escapes, numbers without fractions, and values skipped whole, with
 * nesting bounded. The data is the caller's; nothing here allocates.
 */
#ifndef WINE_NX_JSON_READER_H
#define WINE_NX_JSON_READER_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>

struct parser
{
    const unsigned char *p, *end;
    unsigned int depth;
};

static inline void whitespace( struct parser *p )
{
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\r' || *p->p == '\n')) p->p++;
}

static inline int hex4( const unsigned char *p, uint32_t *value )
{
    unsigned int i;
    uint32_t v = 0;

    for (i = 0; i < 4; i++)
    {
        unsigned char c = p[i];
        unsigned int n = c >= '0' && c <= '9' ? c - '0' :
                         c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                         c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (n == 16) return 0;
        v = v * 16 + n;
    }
    *value = v;
    return 1;
}

static inline int emit_utf8( uint32_t code, char *out, size_t size, size_t *used )
{
    unsigned char bytes[4];
    size_t count, i;

    if (!code) return 0;
    if (code <= 0x7f) { bytes[0] = code; count = 1; }
    else if (code <= 0x7ff) { bytes[0] = 0xc0 | (code >> 6); bytes[1] = 0x80 | (code & 63); count = 2; }
    else if (code <= 0xffff) { bytes[0] = 0xe0 | (code >> 12); bytes[1] = 0x80 | ((code >> 6) & 63); bytes[2] = 0x80 | (code & 63); count = 3; }
    else if (code <= 0x10ffff) { bytes[0] = 0xf0 | (code >> 18); bytes[1] = 0x80 | ((code >> 12) & 63); bytes[2] = 0x80 | ((code >> 6) & 63); bytes[3] = 0x80 | (code & 63); count = 4; }
    else return 0;
    if (*used + count >= size) return 0;
    for (i = 0; i < count; i++) out[(*used)++] = bytes[i];
    return 1;
}

static inline int json_string( struct parser *p, char *out, size_t size )
{
    size_t used = 0;

    whitespace( p );
    if (!size || p->p == p->end || *p->p++ != '"') return 0;
    while (p->p < p->end)
    {
        uint32_t code;
        unsigned char c = *p->p++;

        if (c == '"') { out[used] = 0; return 1; }
        if (c < 0x20) return 0;
        if (c != '\\')
        {
            if (used + 1 >= size) return 0;
            out[used++] = c;
            continue;
        }
        if (p->p == p->end) return 0;
        c = *p->p++;
        if (c == '"' || c == '\\' || c == '/') code = c;
        else if (c == 'b') code = '\b';
        else if (c == 'f') code = '\f';
        else if (c == 'n') code = '\n';
        else if (c == 'r') code = '\r';
        else if (c == 't') code = '\t';
        else if (c == 'u')
        {
            uint32_t low;
            if (p->end - p->p < 4 || !hex4( p->p, &code )) return 0;
            p->p += 4;
            if (code >= 0xd800 && code <= 0xdbff)
            {
                if (p->end - p->p < 6 || p->p[0] != '\\' || p->p[1] != 'u' ||
                    !hex4( p->p + 2, &low ) || low < 0xdc00 || low > 0xdfff) return 0;
                p->p += 6;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            }
            else if (code >= 0xdc00 && code <= 0xdfff) return 0;
        }
        else return 0;
        if (!emit_utf8( code, out, size, &used )) return 0;
    }
    return 0;
}

static inline int skip_value( struct parser *p );

static inline int skip_string( struct parser *p )
{
    whitespace( p );
    if (p->p == p->end || *p->p++ != '"') return 0;
    while (p->p < p->end)
    {
        unsigned char c = *p->p++;
        uint32_t code, low;

        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c != '\\') continue;
        if (p->p == p->end) return 0;
        c = *p->p++;
        if (strchr( "\"\\/bfnrt", c )) continue;
        if (c != 'u' || p->end - p->p < 4 || !hex4( p->p, &code )) return 0;
        p->p += 4;
        if (code >= 0xd800 && code <= 0xdbff)
        {
            if (p->end - p->p < 6 || p->p[0] != '\\' || p->p[1] != 'u' ||
                !hex4( p->p + 2, &low ) || low < 0xdc00 || low > 0xdfff) return 0;
            p->p += 6;
        }
        else if (code >= 0xdc00 && code <= 0xdfff) return 0;
    }
    return 0;
}

static inline int consume( struct parser *p, unsigned char c )
{
    whitespace( p );
    if (p->p == p->end || *p->p != c) return 0;
    p->p++;
    return 1;
}

static inline int skip_compound( struct parser *p, unsigned char close )
{
    whitespace( p );
    if (p->p < p->end && *p->p == close) { p->p++; return 1; }
    for (;;)
    {
        if (close == '}' && (!skip_string( p ) || !consume( p, ':' ))) return 0;
        if (!skip_value( p )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == close) { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

static inline int skip_value( struct parser *p )
{
    const unsigned char *start;
    int result;

    whitespace( p );
    if (p->p == p->end) return 0;
    if (*p->p == '"') return skip_string( p );
    if (*p->p == '{' || *p->p == '[')
    {
        unsigned char close = *p->p++ == '{' ? '}' : ']';
        if (++p->depth > 32) { p->depth--; return 0; }
        result = skip_compound( p, close );
        p->depth--;
        return result;
    }
    start = p->p;
    while (p->p < p->end && !strchr( " \t\r\n,]}", *p->p )) p->p++;
    if (p->p == start) return 0;
    if ((size_t)(p->p - start) == 4 && (!memcmp( start, "true", 4 ) || !memcmp( start, "null", 4 ))) return 1;
    if ((size_t)(p->p - start) == 5 && !memcmp( start, "false", 5 )) return 1;
    if (*start == '-') start++;
    if (start == p->p) return 0;
    if (*start == '0') start++;
    else { if (!isdigit( *start )) return 0; while (start < p->p && isdigit( *start )) start++; }
    if (start < p->p && *start == '.') { start++; if (start == p->p || !isdigit( *start )) return 0; while (start < p->p && isdigit( *start )) start++; }
    if (start < p->p && (*start == 'e' || *start == 'E')) { start++; if (start < p->p && (*start == '+' || *start == '-')) start++; if (start == p->p || !isdigit( *start )) return 0; while (start < p->p && isdigit( *start )) start++; }
    return start == p->p;
}

static inline int json_bool( struct parser *p, int *value )
{
    whitespace( p );
    if (p->end - p->p >= 4 && !memcmp( p->p, "true", 4 )) { p->p += 4; *value = 1; return 1; }
    if (p->end - p->p >= 5 && !memcmp( p->p, "false", 5 )) { p->p += 5; *value = 0; return 1; }
    return 0;
}

static inline int json_null( struct parser *p )
{
    whitespace( p );
    if (p->end - p->p < 4 || memcmp( p->p, "null", 4 )) return 0;
    p->p += 4;
    return 1;
}

static inline int json_uint( struct parser *p, unsigned long long *value )
{
    unsigned long long v = 0;
    const unsigned char *start;

    whitespace( p );
    start = p->p;
    if (p->p == p->end || !isdigit( *p->p )) return 0;
    if (*p->p == '0' && p->p + 1 < p->end && isdigit( p->p[1] )) return 0;
    while (p->p < p->end && isdigit( *p->p ))
    {
        unsigned int n = *p->p++ - '0';
        if (v > (UINT64_MAX - n) / 10) return 0;
        v = v * 10 + n;
    }
    if (p->p == start || (p->p < p->end && (*p->p == '.' || *p->p == 'e' || *p->p == 'E'))) return 0;
    *value = v;
    return 1;
}

#endif
