#include "horizon_dlls.h"
#include "json_reader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
typedef Sha256Context hash_context;
static void hash_begin( hash_context *c ) { sha256ContextCreate( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { sha256ContextUpdate( c, data, size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { sha256ContextGetHash( c, out ); }
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX hash_context;
static void hash_begin( hash_context *c ) { CC_SHA256_Init( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { CC_SHA256_Update( c, data, (CC_LONG)size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { CC_SHA256_Final( out, c ); }
#else
#include <openssl/sha.h>
typedef SHA256_CTX hash_context;
static void hash_begin( hash_context *c ) { SHA256_Init( c ); }
static void hash_add( hash_context *c, const void *data, size_t size ) { SHA256_Update( c, data, size ); }
static void hash_end( hash_context *c, unsigned char out[32] ) { SHA256_Final( out, c ); }
#endif

/* The published manifest is a MB or two; this is room for it to grow. */
#define MANIFEST_MAX   (32u * 1024u * 1024u)
/* The largest file the repository holds is a few tens of MB. */
#define FILE_MAX       (512ull * 1024 * 1024)

const char *const horizon_dlls_folders[] =
{
    "drive_c/windows/system32",
    "drive_c/windows/syswow64",
    "drive_c/dxvk",
    "drive_c/dxvk64",
    "drive_c/vkd3d64",
    NULL
};

/***********************************************************************
 * Reading a manifest
 */

static int folder_index( const char *path )
{
    int i;

    for (i = 0; horizon_dlls_folders[i]; i++)
        if (!strcmp( path, horizon_dlls_folders[i] )) return i;
    return -1;
}

/* A file name as the repository has them: no folders, nothing hidden. */
static int valid_name( const char *name )
{
    const char *p;

    if (!name[0] || name[0] == '.' || strlen( name ) >= 64) return 0;
    for (p = name; *p; p++)
        if (!isalnum( (unsigned char)*p ) && !strchr( "._-+", *p )) return 0;
    return 1;
}

static int valid_hash( const char *text )
{
    size_t i;

    if (strlen( text ) != 64) return 0;
    for (i = 0; i < 64; i++) if (!isxdigit( (unsigned char)text[i] )) return 0;
    return 1;
}

/* A URL from the repository, for the file it says it is. */
static int official_url( const char *url, const char *folder, const char *name )
{
    char tail[160];
    size_t length = strlen( url ), tail_length;

    if (strncmp( url, HORIZON_DLLS_RAW, sizeof(HORIZON_DLLS_RAW) - 1 ) || strstr( url, ".." )) return 0;
    tail_length = (size_t)snprintf( tail, sizeof(tail), "/switch/wine/%s/%s", folder, name );
    return tail_length < sizeof(tail) && length > tail_length && !strcmp( url + length - tail_length, tail );
}

static int grow( void **array, unsigned int *capacity, unsigned int count, size_t size )
{
    unsigned int wanted;
    void *grown;

    if (count < *capacity) return 1;
    wanted = *capacity ? *capacity * 2 : 256;
    if (!(grown = realloc( *array, (size_t)wanted * size ))) return 0;
    *array = grown;
    *capacity = wanted;
    return 1;
}

static int category_index( struct horizon_dll_manifest *m, const char *key )
{
    unsigned int i;

    for (i = 0; i < m->category_count; i++)
        if (!strcmp( m->categories[i].key, key )) return i;
    if (m->category_count >= HORIZON_DLLS_CATEGORIES || strlen( key ) >= sizeof(m->categories[0].key)) return -1;
    snprintf( m->categories[m->category_count].key, sizeof(m->categories[0].key), "%s", key );
    m->categories[m->category_count].description[0] = 0;
    return m->category_count++;
}

/* Each key of an object in turn: calls field with the parser at its value. */
static int object( struct parser *p, int (*field)( struct parser *p, const char *key, void *data ), void *data )
{
    char key[64];

    if (!consume( p, '{' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
    for (;;)
    {
        if (!json_string( p, key, sizeof(key) ) || !consume( p, ':' )) return 0;
        if (!field( p, key, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

static int array( struct parser *p, int (*item)( struct parser *p, void *data ), void *data )
{
    if (!consume( p, '[' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
    for (;;)
    {
        if (!item( p, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

struct reading
{
    struct horizon_dll_manifest *manifest;
    const char *const *features;
    size_t feature_count;
    struct horizon_dll_file *file;
    char path[64], category[32];
    unsigned int seen;
};

static int has_feature( const struct reading *r, const char *feature )
{
    size_t i;

    for (i = 0; i < r->feature_count; i++)
        if (!strcmp( r->features[i], feature )) return 1;
    return 0;
}

static int read_feature( struct parser *p, void *data )
{
    struct reading *r = data;
    char feature[96];

    if (!json_string( p, feature, sizeof(feature) )) return 0;
    if (!has_feature( r, feature ) && r->file->satisfied)
    {
        r->file->satisfied = 0;
        snprintf( r->file->missing, sizeof(r->file->missing), "%s", feature );
    }
    return 1;
}

static int read_requires( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    char flavor[16];

    if (!strcmp( key, "features" )) return array( p, read_feature, r );
    if (!strcmp( key, "flavor" ))
    {
        if (!json_string( p, flavor, sizeof(flavor) )) return 0;
        /* A record written for another runtime's files is not this one's. */
        if (strcmp( flavor, HORIZON_DLLS_FLAVOR ) && r->manifest->schema >= 2 && r->file->satisfied)
        {
            r->file->satisfied = 0;
            snprintf( r->file->missing, sizeof(r->file->missing), "flavor %s", flavor );
        }
        return 1;
    }
    return skip_value( p );
}

static int read_class_field( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_class *c = data;

    if (!strcmp( key, "clsid" )) return json_string( p, c->clsid, sizeof(c->clsid) );
    if (!strcmp( key, "name" )) return json_string( p, c->name, sizeof(c->name) );
    if (!strcmp( key, "threading" )) return json_string( p, c->threading, sizeof(c->threading) );
    return skip_value( p );
}

static int read_class( struct parser *p, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    struct horizon_dll_class *c;
    const char *s;

    if (!grow( (void **)&m->classes, &m->class_capacity, m->class_count, sizeof(*m->classes) )) return 0;
    c = &m->classes[m->class_count];
    memset( c, 0, sizeof(*c) );
    if (!object( p, read_class_field, c )) return 0;
    /* A class id is hex and dashes, and the rest goes into a .reg file. */
    if (strlen( c->clsid ) != 36) return 0;
    for (s = c->clsid; *s; s++) if (!isxdigit( (unsigned char)*s ) && *s != '-') return 0;
    for (s = c->name; *s; s++) if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r') return 0;
    for (s = c->threading; *s; s++) if (!isalpha( (unsigned char)*s )) return 0;
    if (!c->threading[0]) snprintf( c->threading, sizeof(c->threading), "Both" );
    m->class_count++;
    r->file->class_count++;
    return 1;
}

static int read_file_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct horizon_dll_file *f = r->file;
    unsigned long long value;

    if (!strcmp( key, "name" )) { r->seen |= 1; return json_string( p, f->name, sizeof(f->name) ); }
    if (!strcmp( key, "path" )) { r->seen |= 2; return json_string( p, r->path, sizeof(r->path) ); }
    if (!strcmp( key, "arch" )) return json_string( p, f->arch, sizeof(f->arch) );
    /* schema 1 called the category a group */
    if (!strcmp( key, "category" ) || (!strcmp( key, "group" ) && !r->category[0]))
        return json_string( p, r->category, sizeof(r->category) );
    if (!strcmp( key, "version" ))
    {
        if (!json_uint( p, &value ) || value > 0xffffffffu) return 0;
        f->version = (unsigned int)value;
        return 1;
    }
    if (!strcmp( key, "size" )) { r->seen |= 4; return json_uint( p, &f->size ); }
    if (!strcmp( key, "sha256" )) { r->seen |= 8; return json_string( p, f->sha256, sizeof(f->sha256) ); }
    if (!strcmp( key, "url" )) { r->seen |= 16; return json_string( p, f->url, sizeof(f->url) ); }
    if (!strcmp( key, "requires" )) return object( p, read_requires, r );
    if (!strcmp( key, "classes" ))
    {
        f->class_first = r->manifest->class_count;
        f->class_count = 0;
        return array( p, read_class, r );
    }
    return skip_value( p );
}

static int read_file( struct parser *p, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    struct horizon_dll_file *f;
    int folder, category;

    if (!grow( (void **)&m->files, &m->capacity, m->count, sizeof(*m->files) )) return 0;
    f = r->file = &m->files[m->count];
    memset( f, 0, sizeof(*f) );
    f->satisfied = 1;
    f->class_first = m->class_count;
    r->path[0] = r->category[0] = 0;
    r->seen = 0;
    if (!object( p, read_file_field, r )) return 0;
    if (r->seen != 31 || !valid_name( f->name ) || (folder = folder_index( r->path )) < 0 ||
        !f->size || f->size > FILE_MAX || !valid_hash( f->sha256 ) || !official_url( f->url, r->path, f->name ))
        return 0;
    for (char *c = f->sha256; *c; c++) *c = tolower( (unsigned char)*c );
    if ((category = category_index( m, r->category[0] ? r->category : "system" )) < 0) return 0;
    f->folder = (unsigned char)folder;
    f->category = (unsigned char)category;
    m->count++;
    return 1;
}

static int read_category( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_manifest *m = data;
    int index = category_index( m, key );

    if (index < 0) return 0;
    return json_string( p, m->categories[index].description, sizeof(m->categories[0].description) );
}

static int read_source( struct parser *p, const char *key, void *data )
{
    struct horizon_dll_manifest *m = data;

    if (!strcmp( key, "commit" )) return json_string( p, m->commit, sizeof(m->commit) );
    if (!strcmp( key, "wine" )) return json_string( p, m->wine, sizeof(m->wine) );
    return skip_value( p );
}

static int read_top( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct horizon_dll_manifest *m = r->manifest;
    unsigned long long schema;

    if (!strcmp( key, "schema" ))
    {
        if (!json_uint( p, &schema ) || !schema || schema > 2) return 0;
        m->schema = (int)schema;
        return 1;
    }
    if (!strcmp( key, "flavor" )) return json_string( p, m->flavor, sizeof(m->flavor) );
    if (!strcmp( key, "source" )) return object( p, read_source, m );
    if (!strcmp( key, "categories" )) return object( p, read_category, m );
    if (!strcmp( key, "files" )) return m->schema ? array( p, read_file, r ) : 0;
    return skip_value( p );
}

enum horizon_dlls_result horizon_dlls_parse( const char *text, size_t size, const char *const *features,
                                             size_t feature_count, struct horizon_dll_manifest *out )
{
    struct parser p = { (const unsigned char *)text, (const unsigned char *)text + size, 0 };
    struct reading r = { .manifest = out, .features = features, .feature_count = feature_count };
    unsigned int i, j;

    memset( out, 0, sizeof(*out) );
    if (!text || !object( &p, read_top, &r )) goto invalid;
    whitespace( &p );
    if (p.p != p.end || !out->schema || !out->flavor[0]) goto invalid;
    /* One file per place on the card. */
    for (i = 0; i < out->count; i++)
        for (j = i + 1; j < out->count; j++)
            if (out->files[i].folder == out->files[j].folder && !strcasecmp( out->files[i].name, out->files[j].name ))
                goto invalid;
    return HORIZON_DLLS_OK;
invalid:
    horizon_dlls_free( out );
    return HORIZON_DLLS_INVALID;
}

void horizon_dlls_free( struct horizon_dll_manifest *m )
{
    free( m->files );
    free( m->classes );
    memset( m, 0, sizeof(*m) );
}

static int join( char *out, size_t size, const char *root, const char *a, const char *b )
{
    int length = b ? snprintf( out, size, "%s/%s/%s", root, a, b ) : snprintf( out, size, "%s/%s", root, a );
    return length > 0 && (size_t)length < size;
}

enum horizon_dlls_result horizon_dlls_load( const char *root, const char *const *features, size_t feature_count,
                                            struct horizon_dll_manifest *out )
{
    char path[768];
    enum horizon_dlls_result result;
    FILE *file;
    char *text;
    long size;

    memset( out, 0, sizeof(*out) );
    if (!join( path, sizeof(path), root, HORIZON_DLLS_MANIFEST, NULL )) return HORIZON_DLLS_INVALID;
    if (!(file = fopen( path, "rb" ))) return errno == ENOENT ? HORIZON_DLLS_NOT_FOUND : HORIZON_DLLS_IO;
    if (fseek( file, 0, SEEK_END ) || (size = ftell( file )) < 0 || size > (long)MANIFEST_MAX ||
        fseek( file, 0, SEEK_SET ))
    {
        fclose( file );
        return HORIZON_DLLS_INVALID;
    }
    if (!(text = malloc( size + 1 ))) { fclose( file ); return HORIZON_DLLS_MEMORY; }
    if (fread( text, 1, size, file ) != (size_t)size) { free( text ); fclose( file ); return HORIZON_DLLS_IO; }
    fclose( file );
    text[size] = 0;
    result = horizon_dlls_parse( text, size, features, feature_count, out );
    free( text );
    return result;
}

struct memory
{
    char *data;
    size_t size, capacity;
};

static int to_memory( void *context, const void *data, size_t size )
{
    struct memory *m = context;
    size_t wanted;
    char *grown;

    if (size > MANIFEST_MAX - m->size) return 0;
    if (m->size + size + 1 > m->capacity)
    {
        wanted = m->capacity ? m->capacity : 65536;
        while (wanted < m->size + size + 1) wanted *= 2;
        if (!(grown = realloc( m->data, wanted ))) return 0;
        m->data = grown;
        m->capacity = wanted;
    }
    memcpy( m->data + m->size, data, size );
    m->size += size;
    m->data[m->size] = 0;
    return 1;
}

enum horizon_dlls_result horizon_dlls_fetch_manifest( const struct horizon_dlls_transport *transport,
        const char *url, const char *const *features, size_t feature_count,
        struct horizon_dll_manifest *out, horizon_dlls_progress progress, void *opaque )
{
    struct memory body = {0};
    enum horizon_dlls_result result;

    memset( out, 0, sizeof(*out) );
    result = transport->fetch( transport->opaque, url, to_memory, &body, progress, opaque );
    if (result == HORIZON_DLLS_OK)
    {
        result = horizon_dlls_parse( body.data, body.size, features, feature_count, out );
        if (result == HORIZON_DLLS_OK && (out->schema < 2 || strcmp( out->flavor, HORIZON_DLLS_FLAVOR )))
        {
            horizon_dlls_free( out );
            result = HORIZON_DLLS_FLAVOR_MISMATCH;
        }
    }
    free( body.data );
    return result;
}

/***********************************************************************
 * What the card has
 */

static void hex( const unsigned char hash[32], char out[65] )
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++)
    {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[64] = 0;
}

/* The SHA-256 of a file on the card; 0 when it cannot be read. */
static int hash_file( const char *path, char out[65], const char *what, unsigned long long *done,
                      unsigned long long total, horizon_dlls_progress progress, void *opaque, int *cancelled )
{
    static unsigned char buffer[256 * 1024];
    unsigned char hash[32];
    hash_context context;
    FILE *file;
    size_t got;

    if (!(file = fopen( path, "rb" ))) return 0;
    hash_begin( &context );
    while ((got = fread( buffer, 1, sizeof(buffer), file )) > 0)
    {
        hash_add( &context, buffer, got );
        *done += got;
        if (progress && progress( opaque, what, *done, total )) { *cancelled = 1; break; }
    }
    fclose( file );
    if (*cancelled) return 0;
    hash_end( &context, hash );
    hex( hash, out );
    return 1;
}

static const struct horizon_dll_file *find( const struct horizon_dll_manifest *m, unsigned int folder,
                                            const char *name )
{
    unsigned int i;

    if (!m) return NULL;
    for (i = 0; i < m->count; i++)
        if (m->files[i].folder == folder && !strcasecmp( m->files[i].name, name )) return &m->files[i];
    return NULL;
}

enum horizon_dlls_result horizon_dlls_plan( const char *root, struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, int verify, horizon_dlls_progress progress, void *opaque,
        struct horizon_dlls_plan *plan )
{
    unsigned long long to_hash = 0, hashed = 0;
    unsigned int i;
    int cancelled = 0;

    memset( plan, 0, sizeof(*plan) );
    /* What has to be read to be known: everything when verifying, and a file
     * the card has with no record of where it came from. */
    for (i = 0; i < remote->count; i++)
    {
        struct horizon_dll_file *f = &remote->files[i];
        const struct horizon_dll_file *known = find( local, f->folder, f->name );
        char path[768];
        struct stat st;

        f->state = HORIZON_DLL_NEW;
        if (!join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name )) return HORIZON_DLLS_INVALID;
        if (!stat( path, &st ) && S_ISREG( st.st_mode ) && (unsigned long long)st.st_size == f->size &&
            (verify || !known))
            to_hash += f->size;
    }
    for (i = 0; i < remote->count; i++)
    {
        struct horizon_dll_file *f = &remote->files[i];
        struct horizon_dlls_category_plan *c = &plan->categories[f->category];
        const struct horizon_dll_file *known = find( local, f->folder, f->name );
        char path[768], digest[65], what[96];
        struct stat st;
        int present;

        join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name );
        present = !stat( path, &st ) && S_ISREG( st.st_mode );
        if (!f->satisfied) f->state = HORIZON_DLL_UNSUPPORTED;
        else if (!present) f->state = HORIZON_DLL_NEW;
        else if ((unsigned long long)st.st_size != f->size) f->state = HORIZON_DLL_CHANGED;
        else if (known && !verify) f->state = strcmp( known->sha256, f->sha256 ) ? HORIZON_DLL_CHANGED : HORIZON_DLL_CURRENT;
        else
        {
            snprintf( what, sizeof(what), "Checking %s", f->name );
            if (!hash_file( path, digest, what, &hashed, to_hash, progress, opaque, &cancelled ))
            {
                if (cancelled) return HORIZON_DLLS_CANCELLED;
                f->state = HORIZON_DLL_CHANGED;
            }
            else f->state = strcmp( digest, f->sha256 ) ? HORIZON_DLL_CHANGED : HORIZON_DLL_CURRENT;
        }
        plan->files++;
        c->files++;
        plan->bytes += f->size;
        c->bytes += f->size;
        switch (f->state)
        {
        case HORIZON_DLL_CURRENT: plan->current++; c->current++; break;
        case HORIZON_DLL_UNSUPPORTED: plan->unsupported++; c->unsupported++; break;
        default:
            plan->pending++; c->pending++;
            plan->pending_bytes += f->size; c->pending_bytes += f->size;
            break;
        }
    }
    if (local)
        for (i = 0; i < local->count; i++)
            if (!find( remote, local->files[i].folder, local->files[i].name )) plan->removed++;
    return HORIZON_DLLS_OK;
}

/***********************************************************************
 * Bringing the card up to date
 */

struct download
{
    FILE *file;
    hash_context hash;
    unsigned long long size, expected;
    int failed;
};

static int to_file( void *context, const void *data, size_t size )
{
    struct download *d = context;

    if (size > d->expected - d->size) { d->failed = HORIZON_DLLS_INVALID; return 0; }
    if (fwrite( data, 1, size, d->file ) != size) { d->failed = HORIZON_DLLS_IO; return 0; }
    hash_add( &d->hash, data, size );
    d->size += size;
    return 1;
}

struct file_progress
{
    horizon_dlls_progress progress;
    void *opaque;
    const char *what;
    unsigned long long done, total;
};

static int within_file( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    struct file_progress *p = opaque;

    (void)what;
    (void)total;
    return p->progress && p->progress( p->opaque, p->what, p->done + current, p->total );
}

static int make_folders( const char *root, const char *folder )
{
    char path[768];
    char *slash;

    if (!join( path, sizeof(path), root, folder, NULL )) return 0;
    for (slash = path + strlen( root ) + 1; (slash = strchr( slash, '/' )); slash++)
    {
        *slash = 0;
        if (mkdir( path, 0777 ) && errno != EEXIST) return 0;
        *slash = '/';
    }
    return !mkdir( path, 0777 ) || errno == EEXIST;
}

static enum horizon_dlls_result download_file( const char *root, const struct horizon_dll_file *f,
        const struct horizon_dlls_transport *transport, struct file_progress *progress )
{
    char path[768], part[800], digest[65];
    unsigned char hash[32];
    struct download d = {0};
    enum horizon_dlls_result result;

    if (!make_folders( root, horizon_dlls_folders[f->folder] ) ||
        !join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name ) ||
        (size_t)snprintf( part, sizeof(part), "%s.part", path ) >= sizeof(part))
        return HORIZON_DLLS_IO;
    if (!(d.file = fopen( part, "wb" ))) return HORIZON_DLLS_IO;
    d.expected = f->size;
    hash_begin( &d.hash );
    result = transport->fetch( transport->opaque, f->url, to_file, &d, within_file, progress );
    if (fflush( d.file ) || fsync( fileno( d.file ) )) d.failed = HORIZON_DLLS_IO;
    if (fclose( d.file )) d.failed = HORIZON_DLLS_IO;
    if (result == HORIZON_DLLS_OK && d.failed) result = d.failed;
    if (result == HORIZON_DLLS_OK && d.size != f->size) result = HORIZON_DLLS_INVALID;
    if (result == HORIZON_DLLS_OK)
    {
        hash_end( &d.hash, hash );
        hex( hash, digest );
        if (strcmp( digest, f->sha256 )) result = HORIZON_DLLS_HASH;
    }
    /* A rename does not replace a file on the card: the old one goes first,
     * and one missing after a power cut is simply downloaded again. */
    if (result == HORIZON_DLLS_OK && ((remove( path ) && errno != ENOENT) || rename( part, path )))
        result = HORIZON_DLLS_IO;
    if (result != HORIZON_DLLS_OK) remove( part );
    return result;
}

struct text
{
    char *data;
    size_t size, capacity;
    int failed;
};

static void add( struct text *t, const char *format, ... ) __attribute__((format(printf, 2, 3)));
static void add( struct text *t, const char *format, ... )
{
    va_list args;
    int length;
    char *grown;

    if (t->failed) return;
    for (;;)
    {
        va_start( args, format );
        length = vsnprintf( t->data ? t->data + t->size : NULL, t->data ? t->capacity - t->size : 0, format, args );
        va_end( args );
        if (length < 0) { t->failed = 1; return; }
        if (t->data && t->size + length < t->capacity) { t->size += length; return; }
        if (!(grown = realloc( t->data, t->capacity ? t->capacity * 2 + length : 65536 + length )))
        {
            t->failed = 1;
            return;
        }
        t->capacity = t->capacity ? t->capacity * 2 + length : 65536 + length;
        t->data = grown;
    }
}

/* Names and descriptions come from a manifest this parsed; quotes and
 * backslashes are all a JSON string needs escaped of them. */
static void add_string( struct text *t, const char *s )
{
    add( t, "\"" );
    for (; *s; s++)
    {
        if (*s == '"' || *s == '\\') add( t, "\\%c", *s );
        else if ((unsigned char)*s < 0x20) add( t, "\\u%04x", *s );
        else add( t, "%c", *s );
    }
    add( t, "\"" );
}

struct kept
{
    const struct horizon_dll_manifest *from;
    const struct horizon_dll_file *file;
};

static int write_atomically( const char *root, const char *name, const char *data, size_t size )
{
    char path[768], part[800];
    FILE *file;
    int ok;

    if (!join( path, sizeof(path), root, name, NULL ) ||
        (size_t)snprintf( part, sizeof(part), "%s.part", path ) >= sizeof(part) ||
        !make_folders( root, "horizon-dlls" ))
        return 0;
    if (!(file = fopen( part, "wb" ))) return 0;
    ok = fwrite( data, 1, size, file ) == size && !fflush( file ) && !fsync( fileno( file ) );
    ok = !fclose( file ) && ok;
    if (ok) ok = (!remove( path ) || errno == ENOENT) && !rename( part, path );
    if (!ok) remove( part );
    return ok;
}

/* The card's manifest, of the files it holds, and the classes they serve. */
static int write_record( const char *root, const struct horizon_dll_manifest *remote, const struct kept *kept,
                         unsigned int count )
{
    struct text manifest = {0}, classes = {0};
    unsigned int i, j, pass, written = 0, claimed_capacity = 0;
    const char **claimed;
    int ok;

    for (i = 0; i < count; i++) claimed_capacity += kept[i].file->class_count;
    if (!(claimed = calloc( claimed_capacity + 1, sizeof(*claimed) ))) return 0;

    add( &manifest, "{\"schema\":2,\"flavor\":" );
    add_string( &manifest, HORIZON_DLLS_FLAVOR );
    add( &manifest, ",\"source\":{\"commit\":" );
    add_string( &manifest, remote->commit );
    add( &manifest, ",\"wine\":" );
    add_string( &manifest, remote->wine );
    add( &manifest, "},\"categories\":{" );
    for (i = 0; i < remote->category_count; i++)
    {
        add( &manifest, "%s", i ? "," : "" );
        add_string( &manifest, remote->categories[i].key );
        add( &manifest, ":" );
        add_string( &manifest, remote->categories[i].description );
    }
    add( &manifest, "},\"files\":[" );
    for (i = 0; i < count; i++)
    {
        const struct horizon_dll_file *f = kept[i].file;
        const struct horizon_dll_manifest *m = kept[i].from;

        add( &manifest, "%s\n{\"name\":", i ? "," : "" );
        add_string( &manifest, f->name );
        add( &manifest, ",\"path\":" );
        add_string( &manifest, horizon_dlls_folders[f->folder] );
        add( &manifest, ",\"arch\":" );
        add_string( &manifest, f->arch );
        add( &manifest, ",\"category\":" );
        add_string( &manifest, m->categories[f->category].key );
        add( &manifest, ",\"version\":%u,\"size\":%llu,\"sha256\":\"%s\",\"url\":", f->version, f->size, f->sha256 );
        add_string( &manifest, f->url );
        add( &manifest, ",\"requires\":{\"flavor\":\"%s\",\"features\":[]},\"classes\":[", HORIZON_DLLS_FLAVOR );
        for (j = 0; j < f->class_count; j++)
        {
            const struct horizon_dll_class *c = &m->classes[f->class_first + j];

            add( &manifest, "%s{\"clsid\":\"%s\",\"name\":", j ? "," : "", c->clsid );
            add_string( &manifest, c->name );
            add( &manifest, ",\"threading\":\"%s\"}", c->threading );
        }
        add( &manifest, "]}" );
    }
    add( &manifest, "\n]}\n" );

    /* The first file to claim a class keeps it, 32-bit ones first, as the
     * repository's own classes.reg has them. */
    add( &classes, "WINE REGISTRY Version 2\n;; The COM classes the DLLs on this card serve, from their manifest.\n\n" );
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < count; i++)
        {
            const struct horizon_dll_file *f = kept[i].file;
            const struct horizon_dll_manifest *m = kept[i].from;
            const char *dot = strrchr( f->name, '.' );

            if ((pass == 0) != !strcmp( f->arch, "i386" )) continue;
            for (j = 0; j < f->class_count; j++)
            {
                const struct horizon_dll_class *c = &m->classes[f->class_first + j];
                unsigned int k;

                for (k = 0; k < written; k++) if (!strcasecmp( claimed[k], c->clsid )) break;
                if (k < written) continue;
                if (written < claimed_capacity) claimed[written++] = c->clsid;
                add( &classes, ";; %.*s: %s\n[Software\\\\Classes\\\\CLSID\\\\{%s}\\\\InprocServer32]\n"
                     "@=\"%s\"\n\"ThreadingModel\"=\"%s\"\n\n",
                     (int)(dot ? dot - f->name : (long)strlen( f->name )), f->name, c->name, c->clsid,
                     f->name, c->threading );
            }
        }
    ok = !manifest.failed && !classes.failed &&
         write_atomically( root, HORIZON_DLLS_CLASSES, classes.data, classes.size ) &&
         write_atomically( root, HORIZON_DLLS_MANIFEST, manifest.data, manifest.size );
    free( manifest.data );
    free( classes.data );
    free( claimed );
    return ok;
}

/* A file the repository no longer has goes, unless someone put other bytes
 * there since, which are theirs. */
static void remove_stale( const char *root, const struct horizon_dll_file *f )
{
    char path[768], digest[65];
    unsigned long long done = 0;
    int cancelled = 0;

    if (!join( path, sizeof(path), root, horizon_dlls_folders[f->folder], f->name )) return;
    if (hash_file( path, digest, NULL, &done, 0, NULL, NULL, &cancelled ) && !strcmp( digest, f->sha256 ))
        remove( path );
}

enum horizon_dlls_result horizon_dlls_apply( const char *root, const struct horizon_dll_manifest *remote,
        const struct horizon_dll_manifest *local, const struct horizon_dlls_transport *transport,
        horizon_dlls_progress progress, void *opaque )
{
    struct file_progress fp = { .progress = progress, .opaque = opaque };
    enum horizon_dlls_result result = HORIZON_DLLS_OK;
    unsigned char *done;
    struct kept *kept;
    unsigned int i, count = 0, pending = 0, index = 0;
    char what[128];

    for (i = 0; i < remote->count; i++)
        if (remote->files[i].state == HORIZON_DLL_NEW || remote->files[i].state == HORIZON_DLL_CHANGED)
        {
            fp.total += remote->files[i].size;
            pending++;
        }
    if (!(done = calloc( remote->count + 1, 1 )) ||
        !(kept = calloc( remote->count + (local ? local->count : 0) + 1, sizeof(*kept) )))
    {
        free( done );
        return HORIZON_DLLS_MEMORY;
    }
    for (i = 0; i < remote->count && result == HORIZON_DLLS_OK; i++)
    {
        const struct horizon_dll_file *f = &remote->files[i];

        if (f->state == HORIZON_DLL_CURRENT) { done[i] = 1; continue; }
        if (f->state != HORIZON_DLL_NEW && f->state != HORIZON_DLL_CHANGED) continue;
        snprintf( what, sizeof(what), "%s (%u of %u)", f->name, ++index, pending );
        fp.what = what;
        if (progress && progress( opaque, what, fp.done, fp.total )) { result = HORIZON_DLLS_CANCELLED; break; }
        result = download_file( root, f, transport, &fp );
        if (result == HORIZON_DLLS_OK) done[i] = 1;
        fp.done += f->size;
    }
    /* What the card holds now: the repository's files it has, and of the
     * rest, those it had and still has. */
    for (i = 0; i < remote->count; i++)
    {
        const struct horizon_dll_file *f = &remote->files[i], *before = find( local, f->folder, f->name );

        if (done[i]) kept[count++] = (struct kept){ remote, f };
        else if (before) kept[count++] = (struct kept){ local, before };
    }
    if (local)
        for (i = 0; i < local->count; i++)
        {
            const struct horizon_dll_file *f = &local->files[i];

            if (find( remote, f->folder, f->name )) continue;
            if (result == HORIZON_DLLS_OK) remove_stale( root, f );
            else kept[count++] = (struct kept){ local, f };
        }
    if (!write_record( root, remote, kept, count ) && result == HORIZON_DLLS_OK) result = HORIZON_DLLS_IO;
    free( kept );
    free( done );
    return result;
}

int horizon_dlls_installed( const char *root )
{
    static const char *const needed[] =
    {
        "drive_c/windows/system32/ntdll.dll", "drive_c/windows/system32/wow64.dll",
        "drive_c/windows/syswow64/ntdll.dll", "drive_c/windows/syswow64/kernel32.dll",
    };
    char path[768];
    struct stat st;
    size_t i;

    for (i = 0; i < sizeof(needed) / sizeof(needed[0]); i++)
        if (!join( path, sizeof(path), root, needed[i], NULL ) || stat( path, &st ) || !S_ISREG( st.st_mode ))
            return 0;
    return 1;
}

const char *horizon_dlls_error( enum horizon_dlls_result result )
{
    switch (result)
    {
    case HORIZON_DLLS_OK: return "The Windows DLLs are up to date.";
    case HORIZON_DLLS_CANCELLED: return "Stopped. The files already downloaded are kept.";
    case HORIZON_DLLS_NETWORK: return "Could not reach the DLL repository on GitHub. Check the connection and try again.";
    case HORIZON_DLLS_NOT_FOUND: return "The DLL repository has no manifest where Autorun looks for it.";
    case HORIZON_DLLS_INVALID: return "The DLL repository returned a manifest or file that is not valid.";
    case HORIZON_DLLS_FLAVOR_MISMATCH: return "The DLL repository's files are for another Autorun runtime.";
    case HORIZON_DLLS_IO: return "A file could not be written to the SD card.";
    case HORIZON_DLLS_HASH: return "A download did not match its SHA-256. Try again.";
    case HORIZON_DLLS_MEMORY: return "Not enough memory to read the DLL manifest.";
    }
    return "The DLLs could not be updated.";
}

const char *horizon_dlls_category_title( const char *key, char *buffer, size_t size )
{
    static const struct { const char *key, *title; } titles[] =
    {
        { "core", "Windows core" },
        { "directx-graphics", "DirectX graphics" },
        { "directx-audio", "DirectX audio" },
        { "directx-input", "DirectX input" },
        { "directx-play", "DirectPlay" },
        { "directx-media", "DirectShow and Media Foundation" },
        { "translation-layers", "DXVK and VKD3D-Proton" },
        { "gaming", "Gaming" },
        { "audio", "Audio" },
        { "opengl-vulkan", "OpenGL and Vulkan" },
        { "imaging-text", "Imaging and text" },
        { "c-runtime", "C and C++ runtimes" },
        { "dotnet", ".NET" },
        { "com-ole", "COM and OLE" },
        { "winrt", "Windows Runtime" },
        { "xml", "XML" },
        { "data", "Databases" },
        { "network", "Networking" },
        { "security", "Security" },
        { "shell-ui", "Shell and controls" },
        { "printing", "Printing" },
        { "installers", "Installers" },
        { "legacy-16bit", "16-bit Windows" },
        { "drivers", "Drivers" },
        { "programs", "Programs" },
        { "system", "System" },
    };
    size_t i;

    for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
        if (!strcmp( key, titles[i].key )) return titles[i].title;
    /* One the repository added since: its key, readably. */
    snprintf( buffer, size, "%s", key );
    for (i = 0; buffer[i]; i++) if (buffer[i] == '-') buffer[i] = ' ';
    if (buffer[0]) buffer[0] = toupper( (unsigned char)buffer[0] );
    return buffer;
}
