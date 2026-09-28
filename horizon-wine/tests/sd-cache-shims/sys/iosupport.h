#ifndef SD_CACHE_TEST_IOSUPPORT_H
#define SD_CACHE_TEST_IOSUPPORT_H

#include <sys/stat.h>
#include <sys/types.h>

struct _reent { int _errno; };
extern struct _reent test_reent;
#define _REENT (&test_reent)
typedef struct { int device; } DIR_ITER;

typedef struct
{
    int (*open_r)( struct _reent *, void *, const char *, int, int );
    int (*close_r)( struct _reent *, void * );
    ssize_t (*read_r)( struct _reent *, void *, char *, size_t );
    ssize_t (*write_r)( struct _reent *, void *, const char *, size_t );
    off_t (*seek_r)( struct _reent *, void *, off_t, int );
    int (*fstat_r)( struct _reent *, void *, struct stat * );
    int (*stat_r)( struct _reent *, const char *, struct stat * );
    int (*lstat_r)( struct _reent *, const char *, struct stat * );
    int (*fsync_r)( struct _reent *, void * );
    int (*mkdir_r)( struct _reent *, const char *, int );
    int (*rmdir_r)( struct _reent *, const char * );
    DIR_ITER *(*diropen_r)( struct _reent *, DIR_ITER *, const char * );
    int (*rename_r)( struct _reent *, const char *, const char * );
    int (*unlink_r)( struct _reent *, const char * );
    int (*ftruncate_r)( struct _reent *, void *, off_t );
} devoptab_t;

extern const devoptab_t *devoptab_list[1];
int FindDevice( const char *name );

#endif
