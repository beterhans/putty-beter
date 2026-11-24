/*
 * jumplist-store.c: remember which sessions the jump list should offer.
 *
 * The shell jump list itself is built by jump-list.c, but it needs to be told
 * which sessions have been opened and which are still worth offering. Upstream
 * keeps that list in the registry. PuTTY-beter keeps no configuration in the
 * registry at all, so it lives in a file in the configuration folder - which
 * also means it travels with the rest of the configuration when that is
 * relocated, and that a Mac and a PC can share one folder.
 *
 * The file holds the names as a run of NUL-terminated strings, the same shape
 * the shell wants and the same shape upstream kept in its registry value.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "storage-win32-compat.h"   /* UTF-8 paths, and open/read/write/close */

static char *jumplist_path(void)
{
    char *dir = config_location_current();
    char *path = dupprintf("%s/jumplist", dir);
    sfree(dir);
    return path;
}

/* The stored list, as a NUL-terminated run of strings. Never NULL. */
static strbuf *jumplist_read(void)
{
    strbuf *sb = strbuf_new();
    char *path = jumplist_path();
    int fd = open(path, O_RDONLY);

    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            put_data(sb, buf, (size_t)n);
        close(fd);
    }
    sfree(path);

    /* Terminate, so that the walker always stops cleanly. */
    if (!sb->len || sb->s[sb->len - 1] != '\0')
        put_data(sb, "\0", 1);

    return sb;
}

static bool jumplist_write(strbuf *list)
{
    char *path = jumplist_path();
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = false;

    if (fd >= 0) {
        size_t done = 0;
        while (done < list->len) {
            ssize_t n = write(fd, list->s + done, list->len - done);
            if (n <= 0)
                break;
            done += (size_t)n;
        }
        ok = (done == list->len);
        close(fd);
    }

    sfree(path);
    return ok;
}

/*
 * The one piece of work, shared by all three entry points: optionally add an
 * item at the front, optionally drop an item, and hand back the result. Items
 * that are no longer valid sessions are dropped as we go, which is how the
 * list stays tidy as sessions are deleted.
 */
static int jumplist_transform(const char *add, const char *rem, char **out)
{
    strbuf *oldlist = jumplist_read();
    bool write_failure = false;

    if (out)
        *out = NULL;

    if (add || rem) {
        BinarySource src[1];
        strbuf *newlist = strbuf_new();

        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(oldlist));

        /* The new item goes at the front. */
        if (add)
            put_asciz(newlist, add);

        /* Then the existing list, minus the removed item. */
        while (true) {
            const char *olditem = get_asciz(src);

            if (get_err(src))
                break;
            if (!*olditem)
                continue;              /* the terminator, not an item */
            if (rem && strcmp(olditem, rem) == 0)
                continue;

            /* Only keep names that are still real sessions. */
            settings_r *ps = open_settings_r(olditem);
            if (ps != NULL) {
                close_settings_r(ps);
                put_asciz(newlist, olditem);
            }
        }

        write_failure = !jumplist_write(newlist);

        strbuf_free(oldlist);
        oldlist = newlist;
    }

    if (write_failure) {
        strbuf_free(oldlist);
        return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
    }

    if (out)
        *out = strbuf_to_str(oldlist);
    else
        strbuf_free(oldlist);

    return JUMPLISTREG_OK;
}

int add_to_jumplist_registry(const char *item)
{
    return jumplist_transform(item, item, NULL);
}

int remove_from_jumplist_registry(const char *item)
{
    return jumplist_transform(NULL, item, NULL);
}

char *get_jumplist_registry_entries(void)
{
    char *list_value = NULL;

    if (jumplist_transform(NULL, NULL, &list_value) != JUMPLISTREG_OK ||
        !list_value) {
        list_value = snewn(2, char);
        list_value[0] = list_value[1] = '\0';
    }
    return list_value;
}
