/*
 * xc_install_io (xcat/install.h) for the Xbox: nxdk's Win32-style file API on FATX.
 * "Durable" is NtFlushBuffersFile before the handle is closed, so a journal line or a
 * game file survives the power switch (docs/formats/journal.md §2). Spool files for Deflate
 * zips are flushed when the install asks (stream-install.md §7.1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/nxdk.h"

static bool io_exists(void *ctx, const char *path)
{
    (void)ctx;
    return strlen(path) < XC_NX_PATH_MAX && xc_nx_exists(path);
}

static xc_err io_file_size(void *ctx, const char *path, uint64_t *size)
{
    (void)ctx;
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err == XC_OK) {
        *size = src.size;
        xc_nx_file_close(&f);
    }
    return err;
}

static xc_err io_read_all(void *ctx, const char *path, size_t max_len, char **data, size_t *len)
{
    (void)ctx;
    xc_nx_file f = {NULL};
    xc_source src;
    xc_err err = xc_nx_file_open(&src, &f, path);
    if (err != XC_OK) {
        return err;
    }
    if (src.size > max_len) {
        xc_nx_file_close(&f);
        return XC_ERR_LIMIT;
    }
    char *buf = malloc((size_t)src.size + 1);
    err = buf != NULL ? xc_source_read(&src, 0, buf, (size_t)src.size) : XC_ERR_NOMEM;
    xc_nx_file_close(&f);
    if (err != XC_OK) {
        free(buf);
        return err;
    }
    *data = buf;
    *len = (size_t)src.size;
    return XC_OK;
}

/* Writes `data` at the end (append) or over the file (replace), then flushes it. */
static xc_err write_file(const char *path, bool append, const void *data, size_t len)
{
    if (strlen(path) >= XC_NX_PATH_MAX) {
        return XC_ERR_LIMIT;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XC_ERR_IO;
    }
    bool ok = !append || SetFilePointer(h, 0, NULL, FILE_END) != INVALID_SET_FILE_POINTER;
    const uint8_t *p = data;
    while (ok && len > 0) {
        DWORD put = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        ok = WriteFile(h, p, chunk, &put, NULL) && put == chunk;
        p += put;
        len -= put;
    }
    IO_STATUS_BLOCK io;
    ok = ok && NT_SUCCESS(NtFlushBuffersFile(h, &io));
    ok = CloseHandle(h) != 0 && ok;
    return ok ? XC_OK : XC_ERR_IO;
}

static xc_err io_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_file(path, true, data, len);
}

static xc_err io_write_all(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    return write_file(path, false, data, len);
}

static xc_err io_remove(void *ctx, const char *path)
{
    (void)ctx;
    return DeleteFileA(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_free_space(void *ctx, const char *dir, uint64_t *bytes)
{
    (void)ctx;
    /* The drive's root, "E:\" for "E:\Games". */
    char root[4] = {dir[0], ':', '\\', '\0'};
    return xc_nx_free_space(root, bytes);
}

static xc_err io_open_sink(void *ctx, const char *root, xc_sink *sink, void **handle)
{
    (void)ctx;
    xc_nx_dir_sink *ds = malloc(sizeof *ds);
    if (ds == NULL) {
        return XC_ERR_NOMEM;
    }
    xc_err err = xc_nx_dir_sink_open(sink, ds, root);
    if (err != XC_OK) {
        free(ds);
        return err;
    }
    *handle = ds;
    return XC_OK;
}

static void io_close_sink(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        xc_nx_dir_sink_close(handle);
        free(handle);
    }
}

static xc_err io_open_file(void *ctx, const char *path, xc_source *src, void **handle)
{
    (void)ctx;
    xc_nx_file *f = malloc(sizeof *f);
    if (f == NULL) {
        return XC_ERR_NOMEM;
    }
    f->handle = NULL;
    xc_err err = xc_nx_file_open(src, f, path);
    if (err != XC_OK) {
        free(f);
        return err;
    }
    *handle = f;
    return XC_OK;
}

static void io_close_file(void *ctx, void *handle)
{
    (void)ctx;
    if (handle != NULL) {
        xc_nx_file_close(handle);
        free(handle);
    }
}

/*
 * Spool files for Deflate zip installs (docs/formats/stream-install.md §4): one stays open
 * between calls, since the install appends to the same chunk many times in a row. A file
 * that was appended to is flushed to disk before another is opened, so spool_sync only has
 * the open one left to do (§7.1). One install at a time.
 */
static struct {
    char path[XC_NX_PATH_MAX];
    HANDLE h;
    bool dirty; /* appended to since it was last flushed */
} spool = {"", INVALID_HANDLE_VALUE, false};

static bool spool_flush(void)
{
    if (spool.h == INVALID_HANDLE_VALUE || !spool.dirty) {
        return true;
    }
    IO_STATUS_BLOCK io;
    spool.dirty = false;
    return NT_SUCCESS(NtFlushBuffersFile(spool.h, &io));
}

static bool spool_close(void)
{
    bool ok = spool_flush();
    if (spool.h != INVALID_HANDLE_VALUE) {
        ok = CloseHandle(spool.h) != 0 && ok;
        spool.h = INVALID_HANDLE_VALUE;
    }
    spool.path[0] = '\0';
    return ok;
}

static HANDLE spool_open(const char *path)
{
    if (spool.h != INVALID_HANDLE_VALUE && strcmp(spool.path, path) == 0) {
        return spool.h;
    }
    if (!spool_close() || strlen(path) >= sizeof spool.path) {
        return INVALID_HANDLE_VALUE;
    }
    spool.h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (spool.h != INVALID_HANDLE_VALUE) {
        memcpy(spool.path, path, strlen(path) + 1);
    }
    return spool.h;
}

static xc_err io_spool_append(void *ctx, const char *path, const void *data, size_t len)
{
    (void)ctx;
    HANDLE h = spool_open(path);
    if (h == INVALID_HANDLE_VALUE ||
        SetFilePointer(h, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER) {
        return XC_ERR_IO;
    }
    spool.dirty = true;
    const uint8_t *p = data;
    while (len > 0) {
        DWORD put = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        if (!WriteFile(h, p, chunk, &put, NULL) || put != chunk) {
            return XC_ERR_IO;
        }
        p += put;
        len -= put;
    }
    return XC_OK;
}

static xc_err io_spool_read(void *ctx, const char *path, uint64_t pos, void *buf, size_t len)
{
    (void)ctx;
    HANDLE h = spool_open(path);
    LONG hi = (LONG)(pos >> 32U);
    if (h == INVALID_HANDLE_VALUE ||
        (SetFilePointer(h, (LONG)(uint32_t)pos, &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
         GetLastError() != NO_ERROR)) {
        return XC_ERR_IO;
    }
    uint8_t *p = buf;
    while (len > 0) {
        DWORD got = 0;
        DWORD chunk = len > 0x40000000U ? 0x40000000U : (DWORD)len;
        if (!ReadFile(h, p, chunk, &got, NULL) || got == 0) {
            return XC_ERR_IO;
        }
        p += got;
        len -= got;
    }
    return XC_OK;
}

static xc_err io_spool_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (strcmp(spool.path, path) == 0) {
        spool.dirty = false; /* going anyway */
        (void)spool_close();
    }
    return DeleteFileA(path) ? XC_OK : XC_ERR_IO;
}

static xc_err io_spool_sync(void *ctx)
{
    (void)ctx;
    return spool_flush() ? XC_OK : XC_ERR_IO;
}

void xc_nx_spool_close(void)
{
    (void)spool_close();
}

void xc_nx_install_io(xc_install_io *io)
{
    memset(io, 0, sizeof *io);
    io->separator = '\\';
    io->exists = io_exists;
    io->file_size = io_file_size;
    io->read_all = io_read_all;
    io->append = io_append;
    io->write_all = io_write_all;
    io->remove = io_remove;
    io->free_space = io_free_space;
    io->open_sink = io_open_sink;
    io->close_sink = io_close_sink;
    io->open_file = io_open_file;
    io->close_file = io_close_file;
    io->spool_append = io_spool_append;
    io->spool_read = io_spool_read;
    io->spool_remove = io_spool_remove;
    io->spool_sync = io_spool_sync;
}

/* --- deleting an install folder ---------------------------------------------------------- */

#define TREE_PATH 256 /* FATX paths are at most 250 bytes below the drive */
#define TREE_DEPTH 40

/* The names in folder `path` (without "." and ".."), each followed by a byte that's 1 for a
   folder, 0 for a file, in one malloc'd buffer; *count of them. Read in full before anything
   is deleted, so deleting doesn't disturb the listing. */
static xc_err list_names(const char *path, char **out, size_t *count, size_t *used)
{
    char pattern[TREE_PATH + 3];
    if (snprintf(pattern, sizeof pattern, "%s\\*", path) >= (int)sizeof pattern) {
        return XC_ERR_LIMIT;
    }
    *out = NULL;
    *count = 0;
    *used = 0;
    size_t cap = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return XC_OK; /* empty, or gone */
    }
    xc_err err = XC_OK;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        size_t n = strlen(fd.cFileName) + 2U;
        if (*used + n > cap) {
            size_t grown = cap == 0 ? 4096U : cap * 2U;
            while (grown < *used + n) {
                grown *= 2U;
            }
            char *p = realloc(*out, grown);
            if (p == NULL) {
                err = XC_ERR_NOMEM;
                break;
            }
            *out = p;
            cap = grown;
        }
        memcpy(*out + *used, fd.cFileName, n - 1U);
        (*out)[*used + n - 1U] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        *used += n;
        (*count)++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return err;
}

static xc_err remove_tree(char *path, size_t len, int depth)
{
    if (depth > TREE_DEPTH) {
        return XC_ERR_LIMIT;
    }
    char *names = NULL;
    size_t count = 0;
    size_t used = 0;
    xc_err err = list_names(path, &names, &count, &used);
    xc_err first = err;
    for (size_t at = 0, i = 0; err == XC_OK && i < count; i++) {
        const char *name = names + at;
        size_t n = strlen(name);
        bool dir = name[n + 1U] == 1;
        at += n + 2U;
        if (len + 1U + n >= TREE_PATH) {
            first = first == XC_OK ? XC_ERR_LIMIT : first;
            continue;
        }
        path[len] = '\\';
        memcpy(path + len + 1U, name, n + 1U);
        xc_err e;
        if (dir) {
            e = remove_tree(path, len + 1U + n, depth + 1);
        } else {
            (void)SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
            e = DeleteFileA(path) ? XC_OK : XC_ERR_IO;
        }
        path[len] = '\0';
        if (e == XC_ERR_NOMEM) {
            err = e;
        }
        if (first == XC_OK) {
            first = e; /* keep going: delete what can be deleted */
        }
    }
    free(names);
    if (err != XC_OK) {
        return err;
    }
    (void)SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryA(path) && first == XC_OK) {
        first = XC_ERR_IO;
    }
    return first;
}

xc_err xc_nx_remove_tree(const char *path)
{
    char buf[TREE_PATH];
    size_t len = path != NULL ? strlen(path) : 0;
    if (len < 4 || len >= sizeof buf || path[1] != ':' || path[2] != '\\') {
        return XC_ERR_INVALID_ARG; /* never a drive root */
    }
    memcpy(buf, path, len + 1U);
    while (len > 3 && buf[len - 1U] == '\\') {
        buf[--len] = '\0';
    }
    return remove_tree(buf, len, 0);
}
