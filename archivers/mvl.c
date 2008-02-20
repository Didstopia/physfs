/*
 * MVL support routines for PhysicsFS.
 *
 * This driver handles Descent II Movielib archives.
 *
 * The file format of MVL is quite easy...
 *
 *   //MVL File format - Written by Heiko Herrmann
 *   char sig[4] = {'D','M', 'V', 'L'}; // "DMVL"=Descent MoVie Library
 *
 *   int num_files; // the number of files in this MVL
 *
 *   struct {
 *    char file_name[13]; // Filename, padded to 13 bytes with 0s
 *    int file_size; // filesize in bytes
 *   }DIR_STRUCT[num_files];
 *
 *   struct {
 *    char data[file_size]; // The file data
 *   }FILE_STRUCT[num_files];
 *
 * (That info is from http://www.descent2.com/ddn/specs/mvl/)
 *
 * Please see the file LICENSE in the source's root directory.
 *
 *  This file written by Bradley Bell.
 *  Based on grp.c by Ryan C. Gordon.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#if (defined PHYSFS_SUPPORTS_MVL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "physfs.h"

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"

typedef struct
{
    char name[13];
    PHYSFS_uint32 startPos;
    PHYSFS_uint32 size;
} MVLentry;

typedef struct
{
    char *filename;
    PHYSFS_sint64 last_mod_time;
    PHYSFS_uint32 entryCount;
    MVLentry *entries;
} MVLinfo;

typedef struct
{
    void *handle;
    MVLentry *entry;
    PHYSFS_uint32 curPos;
} MVLfileinfo;


static void MVL_dirClose(DirHandle *h);
static PHYSFS_sint64 MVL_read(FileHandle *handle, void *buffer,
                              PHYSFS_uint32 objSize, PHYSFS_uint32 objCount);
static PHYSFS_sint64 MVL_write(FileHandle *handle, const void *buffer,
                               PHYSFS_uint32 objSize, PHYSFS_uint32 objCount);
static int MVL_eof(FileHandle *handle);
static PHYSFS_sint64 MVL_tell(FileHandle *handle);
static int MVL_seek(FileHandle *handle, PHYSFS_uint64 offset);
static PHYSFS_sint64 MVL_fileLength(FileHandle *handle);
static int MVL_fileClose(FileHandle *handle);
static int MVL_isArchive(const char *filename, int forWriting);
static DirHandle *MVL_openArchive(const char *name, int forWriting);
static LinkedStringList *MVL_enumerateFiles(DirHandle *h,
                                            const char *dirname,
                                            int omitSymLinks);
static int MVL_exists(DirHandle *h, const char *name);
static int MVL_isDirectory(DirHandle *h, const char *name, int *fileExists);
static int MVL_isSymLink(DirHandle *h, const char *name, int *fileExists);
static PHYSFS_sint64 MVL_getLastModTime(DirHandle *h, const char *n, int *e);
static FileHandle *MVL_openRead(DirHandle *h, const char *name, int *exist);
static FileHandle *MVL_openWrite(DirHandle *h, const char *name);
static FileHandle *MVL_openAppend(DirHandle *h, const char *name);
static int MVL_remove(DirHandle *h, const char *name);
static int MVL_mkdir(DirHandle *h, const char *name);

const PHYSFS_ArchiveInfo __PHYSFS_ArchiveInfo_MVL =
{
    "MVL",
    MVL_ARCHIVE_DESCRIPTION,
    "Bradley Bell <btb@icculus.org>",
    "http://icculus.org/physfs/",
};


static const FileFunctions __PHYSFS_FileFunctions_MVL =
{
    MVL_read,       /* read() method       */
    MVL_write,      /* write() method      */
    MVL_eof,        /* eof() method        */
    MVL_tell,       /* tell() method       */
    MVL_seek,       /* seek() method       */
    MVL_fileLength, /* fileLength() method */
    MVL_fileClose   /* fileClose() method  */
};


const DirFunctions __PHYSFS_DirFunctions_MVL =
{
    &__PHYSFS_ArchiveInfo_MVL,
    MVL_isArchive,          /* isArchive() method      */
    MVL_openArchive,        /* openArchive() method    */
    MVL_enumerateFiles,     /* enumerateFiles() method */
    MVL_exists,             /* exists() method         */
    MVL_isDirectory,        /* isDirectory() method    */
    MVL_isSymLink,          /* isSymLink() method      */
    MVL_getLastModTime,     /* getLastModTime() method */
    MVL_openRead,           /* openRead() method       */
    MVL_openWrite,          /* openWrite() method      */
    MVL_openAppend,         /* openAppend() method     */
    MVL_remove,             /* remove() method         */
    MVL_mkdir,              /* mkdir() method          */
    MVL_dirClose            /* dirClose() method       */
};



static void MVL_dirClose(DirHandle *h)
{
    MVLinfo *info = ((MVLinfo *) h->opaque);
    free(info->filename);
    free(info->entries);
    free(info);
    free(h);
} /* MVL_dirClose */


static PHYSFS_sint64 MVL_read(FileHandle *handle, void *buffer,
                              PHYSFS_uint32 objSize, PHYSFS_uint32 objCount)
{
    MVLfileinfo *finfo = (MVLfileinfo *) (handle->opaque);
    MVLentry *entry = finfo->entry;
    PHYSFS_uint32 bytesLeft = entry->size - finfo->curPos;
    PHYSFS_uint32 objsLeft = (bytesLeft / objSize);
    PHYSFS_sint64 rc;

    if (objsLeft < objCount)
        objCount = objsLeft;

    rc = __PHYSFS_platformRead(finfo->handle, buffer, objSize, objCount);
    if (rc > 0)
        finfo->curPos += (PHYSFS_uint32) (rc * objSize);

    return(rc);
} /* MVL_read */


static PHYSFS_sint64 MVL_write(FileHandle *handle, const void *buffer,
                               PHYSFS_uint32 objSize, PHYSFS_uint32 objCount)
{
    BAIL_MACRO(ERR_NOT_SUPPORTED, -1);
} /* MVL_write */


static int MVL_eof(FileHandle *handle)
{
    MVLfileinfo *finfo = (MVLfileinfo *) (handle->opaque);
    MVLentry *entry = finfo->entry;
    return(finfo->curPos >= entry->size);
} /* MVL_eof */


static PHYSFS_sint64 MVL_tell(FileHandle *handle)
{
    return(((MVLfileinfo *) (handle->opaque))->curPos);
} /* MVL_tell */


static int MVL_seek(FileHandle *handle, PHYSFS_uint64 offset)
{
    MVLfileinfo *finfo = (MVLfileinfo *) (handle->opaque);
    MVLentry *entry = finfo->entry;
    int rc;

    BAIL_IF_MACRO(offset < 0, ERR_INVALID_ARGUMENT, 0);
    BAIL_IF_MACRO(offset >= entry->size, ERR_PAST_EOF, 0);
    rc = __PHYSFS_platformSeek(finfo->handle, entry->startPos + offset);
    if (rc)
        finfo->curPos = (PHYSFS_uint32) offset;

    return(rc);
} /* MVL_seek */


static PHYSFS_sint64 MVL_fileLength(FileHandle *handle)
{
    MVLfileinfo *finfo = ((MVLfileinfo *) handle->opaque);
    return((PHYSFS_sint64) finfo->entry->size);
} /* MVL_fileLength */


static int MVL_fileClose(FileHandle *handle)
{
    MVLfileinfo *finfo = ((MVLfileinfo *) handle->opaque);
    BAIL_IF_MACRO(!__PHYSFS_platformClose(finfo->handle), NULL, 0);
    free(finfo);
    free(handle);
    return(1);
} /* MVL_fileClose */


static int mvl_open(const char *filename, int forWriting,
                    void **fh, PHYSFS_uint32 *count)
{
    PHYSFS_uint8 buf[4];

    *fh = NULL;
    BAIL_IF_MACRO(forWriting, ERR_ARC_IS_READ_ONLY, 0);

    *fh = __PHYSFS_platformOpenRead(filename);
    BAIL_IF_MACRO(*fh == NULL, NULL, 0);
    
    if (__PHYSFS_platformRead(*fh, buf, 4, 1) != 1)
        goto openMvl_failed;

    if (memcmp(buf, "DMVL", 4) != 0)
    {
        __PHYSFS_setError(ERR_UNSUPPORTED_ARCHIVE);
        goto openMvl_failed;
    } /* if */

    if (__PHYSFS_platformRead(*fh, count, sizeof (PHYSFS_uint32), 1) != 1)
        goto openMvl_failed;

    *count = PHYSFS_swapULE32(*count);

    return(1);

openMvl_failed:
    if (*fh != NULL)
        __PHYSFS_platformClose(*fh);

    *count = -1;
    *fh = NULL;
    return(0);
} /* mvl_open */


static int MVL_isArchive(const char *filename, int forWriting)
{
    void *fh;
    PHYSFS_uint32 fileCount;
    int retval = mvl_open(filename, forWriting, &fh, &fileCount);

    if (fh != NULL)
        __PHYSFS_platformClose(fh);

    return(retval);
} /* MVL_isArchive */


static int mvl_entry_cmp(void *_a, PHYSFS_uint32 one, PHYSFS_uint32 two)
{
    if (one != two)
    {
        const MVLentry *a = (const MVLentry *) _a;
        return(strcmp(a[one].name, a[two].name));
    } /* if */

    return 0;
} /* mvl_entry_cmp */


static void mvl_entry_swap(void *_a, PHYSFS_uint32 one, PHYSFS_uint32 two)
{
    if (one != two)
    {
        MVLentry tmp;
        MVLentry *first = &(((MVLentry *) _a)[one]);
        MVLentry *second = &(((MVLentry *) _a)[two]);
        memcpy(&tmp, first, sizeof (MVLentry));
        memcpy(first, second, sizeof (MVLentry));
        memcpy(second, &tmp, sizeof (MVLentry));
    } /* if */
} /* mvl_entry_swap */


static int mvl_load_entries(const char *name, int forWriting, MVLinfo *info)
{
    void *fh = NULL;
    PHYSFS_uint32 fileCount;
    PHYSFS_uint32 location = 8;  /* sizeof sig. */
    MVLentry *entry;

    BAIL_IF_MACRO(!mvl_open(name, forWriting, &fh, &fileCount), NULL, 0);
    info->entryCount = fileCount;
    info->entries = (MVLentry *) malloc(sizeof (MVLentry) * fileCount);
    if (info->entries == NULL)
    {
        __PHYSFS_platformClose(fh);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, 0);
    } /* if */

    location += (17 * fileCount);

    for (entry = info->entries; fileCount > 0; fileCount--, entry++)
    {
        if (__PHYSFS_platformRead(fh, &entry->name, 13, 1) != 1)
        {
            __PHYSFS_platformClose(fh);
            return(0);
        } /* if */

        if (__PHYSFS_platformRead(fh, &entry->size, 4, 1) != 1)
        {
            __PHYSFS_platformClose(fh);
            return(0);
        } /* if */

        entry->size = PHYSFS_swapULE32(entry->size);
        entry->startPos = location;
        location += entry->size;
    } /* for */

    __PHYSFS_platformClose(fh);

    __PHYSFS_sort(info->entries, info->entryCount,
                  mvl_entry_cmp, mvl_entry_swap);
    return(1);
} /* mvl_load_entries */


static DirHandle *MVL_openArchive(const char *name, int forWriting)
{
    MVLinfo *info;
    DirHandle *retval = malloc(sizeof (DirHandle));
    PHYSFS_sint64 modtime = __PHYSFS_platformGetLastModTime(name);

    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    info = retval->opaque = malloc(sizeof (MVLinfo));
    if (info == NULL)
    {
        __PHYSFS_setError(ERR_OUT_OF_MEMORY);
        goto MVL_openArchive_failed;
    } /* if */

    memset(info, '\0', sizeof (MVLinfo));

    info->filename = (char *) malloc(strlen(name) + 1);
    if (info->filename == NULL)
    {
        __PHYSFS_setError(ERR_OUT_OF_MEMORY);
        goto MVL_openArchive_failed;
    } /* if */

    if (!mvl_load_entries(name, forWriting, info))
        goto MVL_openArchive_failed;

    strcpy(info->filename, name);
    info->last_mod_time = modtime;
    retval->funcs = &__PHYSFS_DirFunctions_MVL;
    return(retval);

MVL_openArchive_failed:
    if (retval != NULL)
    {
        if (retval->opaque != NULL)
        {
            if (info->filename != NULL)
                free(info->filename);
            if (info->entries != NULL)
                free(info->entries);
            free(info);
        } /* if */
        free(retval);
    } /* if */

    return(NULL);
} /* MVL_openArchive */


static LinkedStringList *MVL_enumerateFiles(DirHandle *h,
                                            const char *dirname,
                                            int omitSymLinks)
{
    MVLinfo *info = ((MVLinfo *) h->opaque);
    MVLentry *entry = info->entries;
    LinkedStringList *retval = NULL, *p = NULL;
    PHYSFS_uint32 max = info->entryCount;
    PHYSFS_uint32 i;

    /* no directories in MVL files. */
    BAIL_IF_MACRO(*dirname != '\0', ERR_NOT_A_DIR, NULL);

    for (i = 0; i < max; i++, entry++)
        retval = __PHYSFS_addToLinkedStringList(retval, &p, entry->name, -1);

    return(retval);
} /* MVL_enumerateFiles */


static MVLentry *mvl_find_entry(MVLinfo *info, const char *name)
{
    char *ptr = strchr(name, '.');
    MVLentry *a = info->entries;
    PHYSFS_sint32 lo = 0;
    PHYSFS_sint32 hi = (PHYSFS_sint32) (info->entryCount - 1);
    PHYSFS_sint32 middle;
    int rc;

    /*
     * Rule out filenames to avoid unneeded processing...no dirs,
     *   big filenames, or extensions > 3 chars.
     */
    BAIL_IF_MACRO((ptr) && (strlen(ptr) > 4), ERR_NO_SUCH_FILE, NULL);
    BAIL_IF_MACRO(strlen(name) > 12, ERR_NO_SUCH_FILE, NULL);
    BAIL_IF_MACRO(strchr(name, '/') != NULL, ERR_NO_SUCH_FILE, NULL);

    while (lo <= hi)
    {
        middle = lo + ((hi - lo) / 2);
        rc = __PHYSFS_platformStricmp(name, a[middle].name);
        if (rc == 0)  /* found it! */
            return(&a[middle]);
        else if (rc > 0)
            lo = middle + 1;
        else
            hi = middle - 1;
    } /* while */

    BAIL_MACRO(ERR_NO_SUCH_FILE, NULL);
} /* mvl_find_entry */


static int MVL_exists(DirHandle *h, const char *name)
{
    return(mvl_find_entry(((MVLinfo *) h->opaque), name) != NULL);
} /* MVL_exists */


static int MVL_isDirectory(DirHandle *h, const char *name, int *fileExists)
{
    *fileExists = MVL_exists(h, name);
    return(0);  /* never directories in a groupfile. */
} /* MVL_isDirectory */


static int MVL_isSymLink(DirHandle *h, const char *name, int *fileExists)
{
    *fileExists = MVL_exists(h, name);
    return(0);  /* never symlinks in a groupfile. */
} /* MVL_isSymLink */


static PHYSFS_sint64 MVL_getLastModTime(DirHandle *h,
                                        const char *name,
                                        int *fileExists)
{
    MVLinfo *info = ((MVLinfo *) h->opaque);
    PHYSFS_sint64 retval = -1;

    *fileExists = (mvl_find_entry(info, name) != NULL);
    if (*fileExists)  /* use time of MVL itself in the physical filesystem. */
        retval = ((MVLinfo *) h->opaque)->last_mod_time;

    return(retval);
} /* MVL_getLastModTime */


static FileHandle *MVL_openRead(DirHandle *h, const char *fnm, int *fileExists)
{
    MVLinfo *info = ((MVLinfo *) h->opaque);
    FileHandle *retval;
    MVLfileinfo *finfo;
    MVLentry *entry;

    entry = mvl_find_entry(info, fnm);
    *fileExists = (entry != NULL);
    BAIL_IF_MACRO(entry == NULL, NULL, NULL);

    retval = (FileHandle *) malloc(sizeof (FileHandle));
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    finfo = (MVLfileinfo *) malloc(sizeof (MVLfileinfo));
    if (finfo == NULL)
    {
        free(retval);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    finfo->handle = __PHYSFS_platformOpenRead(info->filename);
    if ( (finfo->handle == NULL) ||
         (!__PHYSFS_platformSeek(finfo->handle, entry->startPos)) )
    {
        free(finfo);
        free(retval);
        return(NULL);
    } /* if */

    finfo->curPos = 0;
    finfo->entry = entry;
    retval->opaque = (void *) finfo;
    retval->funcs = &__PHYSFS_FileFunctions_MVL;
    retval->dirHandle = h;
    return(retval);
} /* MVL_openRead */


static FileHandle *MVL_openWrite(DirHandle *h, const char *name)
{
    BAIL_MACRO(ERR_NOT_SUPPORTED, NULL);
} /* MVL_openWrite */


static FileHandle *MVL_openAppend(DirHandle *h, const char *name)
{
    BAIL_MACRO(ERR_NOT_SUPPORTED, NULL);
} /* MVL_openAppend */


static int MVL_remove(DirHandle *h, const char *name)
{
    BAIL_MACRO(ERR_NOT_SUPPORTED, 0);
} /* MVL_remove */


static int MVL_mkdir(DirHandle *h, const char *name)
{
    BAIL_MACRO(ERR_NOT_SUPPORTED, 0);
} /* MVL_mkdir */

#endif  /* defined PHYSFS_SUPPORTS_MVL */

/* end of mvl.c ... */

