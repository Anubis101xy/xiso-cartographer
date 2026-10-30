/*
 * libFuzzer target for the map loader (docs/formats/map.md).
 *
 * The input is split at NUL bytes into files: part 0 is the top-level map
 * ("maps/top.txt"); part N is served for any @include whose resolved location
 * ends in "fN.txt" (so includes, nesting, cycles and the file limit are all
 * reachable), and as the archive.org listing for "@archiveorg fN". After loading,
 * the result is checked against the guarantees the installer relies on; a
 * violation aborts:
 *
 *   - limits hold (entries, files, stored problems);
 *   - every entry has a valid key, a URL the HTTP client accepts, and a source;
 *   - keys are unique (§4.3) and xc_map_find returns each entry for its key;
 *     generated entries have a unique name, which xc_map_find_name returns;
 *   - every problem names a loaded source;
 *   - xc_map_hosts lists each server once, and every entry is on one of them.
 *
 * Seeds are in core/tests/fuzz-seeds/map/. Built like fuzz_http.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/archiveorg.h"
#include "xcat/map.h"
#include "xcat/url.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_PARTS 40

static const uint8_t *part_data[MAX_PARTS];
static size_t part_len[MAX_PARTS];
static size_t parts;

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

/* An @smb folder (§5.4) holds an image named after its last name, one in a subfolder, a game
   folder (smb.md §3.1), and a file that isn't an image; a folder named "fail" can't be listed,
   and one named "game" is itself a game folder. */
static xc_err list_smb(void *ctx, const xc_smb_url *folder, xc_smb_listing *out)
{
    (void)ctx;
    const char *last = strrchr(folder->path, '/');
    last = last != NULL ? last + 1 : folder->path[0] != '\0' ? folder->path : folder->share;
    if (strcmp(last, "fail") == 0) {
        return XC_ERR_SMB_LOGIN;
    }
    if (strcmp(last, "game") == 0) {
        return xc_smb_listing_add_game(out, "");
    }
    char name[XC_SMB_MAX_PATH + 16];
    (void)snprintf(name, sizeof name, "%s.xiso.iso", last);
    xc_err err = xc_smb_listing_add(out, name, 4096);
    if (err == XC_OK) {
        err = xc_smb_listing_add(out, "sub/Game (USA).zip", 1);
    }
    if (err == XC_OK) {
        err = xc_smb_listing_add(out, "notes.txt", 10);
    }
    if (err == XC_OK) {
        err = xc_smb_listing_add_game(out, "sub/Folder Game #1");
    }
    return err;
}

static xc_err fetch(void *ctx, const char *location, bool is_url, size_t max_len, char **out,
                    size_t *len)
{
    (void)ctx;
    size_t n = strlen(location);
    static const char listing[] = "https://archive.org/metadata/";
    bool is_listing = strncmp(location, listing, sizeof listing - 1) == 0;
    if (is_listing) {
        /* @archiveorg asks with the listing limit; an @include of the same URL with 1 MiB. */
        check(is_url && (max_len == XC_ARCHIVEORG_MAX_METADATA || max_len == XC_MAP_MAX_FILE));
    } else if (n < 6 || strcmp(location + n - 4, ".txt") != 0) {
        return XC_ERR_IO;
    }
    /* "...f<digits>.txt", or "@archiveorg f<digits>" */
    size_t end = is_listing ? n : n - 4;
    size_t start = end;
    while (start > 0 && location[start - 1] >= '0' && location[start - 1] <= '9') {
        start--;
    }
    if (start == end || start == 0 || location[start - 1] != 'f' || end - start > 2) {
        return XC_ERR_IO;
    }
    size_t index = (size_t)strtoul(location + start, NULL, 10);
    if (index == 0 || index >= parts) {
        return XC_ERR_IO;
    }
    if (part_len[index] > max_len) {
        return XC_ERR_LIMIT;
    }
    *out = malloc(part_len[index] + 1);
    if (*out == NULL) {
        return XC_ERR_NOMEM;
    }
    memcpy(*out, part_data[index], part_len[index]);
    *len = part_len[index];
    return XC_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= size && parts < MAX_PARTS; i++) {
        if (i == size || data[i] == 0) {
            part_data[parts] = data + start;
            part_len[parts] = i - start;
            parts++;
            start = i + 1;
        }
    }
    xc_map map;
    xc_map_loader loader = {fetch, NULL, list_smb};
    xc_err err =
        xc_map_load((const char *)part_data[0], part_len[0], "maps/top.txt", &loader, &map);
    check(err == XC_OK || err == XC_ERR_MAP);
    check(map.entry_count <= XC_MAP_MAX_ENTRIES);
    check(map.source_count >= 1 && map.source_count <= XC_MAP_MAX_FILES);
    check(map.problem_count <= XC_MAP_MAX_PROBLEMS);
    check(err == XC_OK || map.entry_count == 0);
    size_t stored_errors = 0;
    for (size_t i = 0; i < map.problem_count; i++) {
        const xc_map_problem *p = &map.problems[i];
        check(p->source < map.source_count);
        check(memchr(p->message, '\0', sizeof p->message) != NULL);
        stored_errors += p->severity == XC_MAP_ERROR ? 1U : 0U;
    }
    check(stored_errors <= map.error_count);
    static xc_url u;
    static xc_smb_url su;
    for (size_t i = 0; i < map.entry_count; i++) {
        const xc_map_entry *e = &map.entries[i];
        check(e->source < map.source_count && e->line >= 1);
        check(strlen(e->variant) <= XC_MAP_MAX_VARIANT);
        for (const char *c = e->variant; *c != '\0'; c++) {
            check(!(*c >= 'a' && *c <= 'z'));
        }
        bool smb = xc_smb_is_url(e->url);
        if (smb) {
            /* A file, or a game folder (its address ends in '/'), which may be a whole share. */
            check(xc_smb_url_parse(e->url, strlen(e->url), &su) == XC_OK &&
                  (su.path[0] != '\0' || e->url[strlen(e->url) - 1U] == '/'));
        } else {
            check(xc_url_parse(e->url, strlen(e->url), &u) == XC_OK);
        }
        if (e->name != NULL) {
            /* Generated (§5.2, §5.4): keyed by a unique name, with no title ID or variant. */
            check(e->name[0] != '\0' && e->title_id == 0 && e->variant[0] == '\0');
            check(smb || e->size >= 1);
            check(smb || strncmp(e->url, "https://archive.org/download/", 29) == 0);
            check(xc_map_find_name(&map, e->name) == e);
            continue;
        }
        check(e->size == 0 && !e->has_md5 && !e->has_sha1 && !e->has_crc32);
        check(xc_map_find(&map, e->title_id, e->variant) == e);
        for (size_t k = 0; k < i; k++) {
            check(map.entries[k].name != NULL || map.entries[k].title_id != e->title_id ||
                  strcmp(map.entries[k].variant, e->variant) != 0);
        }
    }
    /* The servers: each once, each used, and every entry on one of them (all parse, above). */
    static xc_map_host hosts[64];
    size_t nh = xc_map_hosts(&map, hosts, 64);
    check(nh <= 64);
    size_t on_hosts = 0;
    for (size_t i = 0; i < nh; i++) {
        check(hosts[i].entries + hosts[i].maps + hosts[i].folders >= 1);
        check(hosts[i].folders == 0 || hosts[i].scheme == XC_URL_SMB);
        check(memchr(hosts[i].host, '\0', sizeof hosts[i].host) != NULL);
        for (size_t k = 0; k < i; k++) {
            check(hosts[k].scheme != hosts[i].scheme || hosts[k].port != hosts[i].port ||
                  strcmp(hosts[k].host, hosts[i].host) != 0);
        }
        on_hosts += hosts[i].entries;
    }
    check(nh == 64 || on_hosts == map.entry_count);
    xc_map_free(&map);
    return 0;
}
