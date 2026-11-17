/*
 * config-location.c: choose where PuTTY-beter keeps its configuration.
 *
 * PuTTY normally keeps everything (saved sessions, host keys, CA records,
 * the random seed) in one directory, ~/.putty. This front-end-only file
 * offers to move that somewhere more convenient - on first run, when the
 * configuration is missing, or on demand.
 *
 * The choice itself is recorded by storage.c in a small file called
 * `config-location' at the base location, which is what makes plink, pscp
 * and psftp follow the same directory without any GUI support.
 *
 * When the dialog appears:
 *   - the configuration is missing (no sessions subdirectory yet), or
 *   - the user held Option/Alt while launching PuTTY, or
 *   - PUTTY_CONFIG_RELOCATE=1 is set in the environment.
 *
 * Set PUTTY_CONFIG_DEBUG=1 to trace what happens to stderr.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"
#include "gtkcompat.h"          /* PuTTY-beter: gtk_dialog_fix_macos_titlebar */
#include "gtkmisc.h"            /* PuTTY-beter: our_file_chooser_run */

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

static bool cfg_debug(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTY_CONFIG_DEBUG") != NULL);
    return cached != 0;
}

/*
 * Was the Option (Alt) key being held when this process started? On macOS
 * the hardware modifier state can be read straight from CoreGraphics,
 * which unlike GDK does not require a key event to have been delivered
 * first. Elsewhere, fall back to the GDK keymap.
 */
static bool option_key_held(void)
{
#ifdef __APPLE__
    CGEventFlags flags =
        CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    return (flags & kCGEventFlagMaskAlternate) != 0;
#else
    GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
    return km != NULL && (gdk_keymap_get_modifier_state(km) & GDK_MOD1_MASK);
#endif
}

static void show_error(GtkWindow *parent, const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), "PuTTY configuration folder");
    gtk_dialog_fix_macos_titlebar(dlg);   /* PuTTY-beter: opaque title bar */
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void browse_cb(GtkButton *button, gpointer ventry)
{
    GtkWidget *entry = GTK_WIDGET(ventry);
    /*
     * PuTTY-beter: the shared chooser, so this gets the same full-path box as
     * every other file selector in the application. The upstream call this
     * replaces was:
     *
     *     gtk_file_chooser_dialog_new(
     *         "Choose configuration folder", NULL,
     *         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
     *         "_Cancel", GTK_RESPONSE_CANCEL,
     *         "_Choose", GTK_RESPONSE_ACCEPT, NULL);
     *
     * which started browsing at whatever was typed in, and set
     * gtk_file_chooser_set_create_folders() - both of which the shared chooser
     * does for itself.
     */
    char *chosen = our_file_chooser_run(
        NULL, "Choose configuration folder", true, false,
        gtk_entry_get_text(GTK_ENTRY(entry)));

    if (chosen) {
        gtk_entry_set_text(GTK_ENTRY(entry), chosen);
        g_free(chosen);
    }

    (void)button;
}

/*
 * The relocation dialog. Returns true if the directory was changed.
 *
 * If the user cancels, the current directory is recorded as configured
 * anyway, so that "first run" really is a one-off rather than something
 * that reappears on every launch.
 */
static bool ask_dialog(bool forced)
{
    char *current = config_location_current();
    char *base = config_location_base();
    char *initial = config_location_is_redirected()
        ? dupstr(current) : config_location_default_dir();
    char *msg;
    GtkWidget *dlg, *content, *label, *hbox, *entry, *browse, *hint;
    bool changed = false;

    if (!initial)
        initial = dupstr(current);

    msg = dupprintf(
        "PuTTY-beter keeps saved sessions, host keys and settings in one "
        "folder.\n\n"
        "Folder in use now:  %s\n\n"
        "Choose the folder to use from now on. Anything already saved will "
        "be moved there. Nothing is deleted, and nothing is overwritten: if "
        "the new folder already contains one of these files, the old copy "
        "is left where it is.", current);

    dlg = gtk_dialog_new_with_buttons(
        "PuTTY configuration folder", NULL, GTK_DIALOG_MODAL,
        "Keep Current", GTK_RESPONSE_CANCEL,
        "Use This Folder", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(dlg), false);
    gtk_dialog_fix_macos_titlebar(dlg);   /* PuTTY-beter: opaque title bar */

    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    label = gtk_label_new(msg);
    gtk_label_set_line_wrap(GTK_LABEL(label), true);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_box_pack_start(GTK_BOX(content), label, false, false, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(hbox, 8);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 56);
    /* Show the end of a long path rather than the start of it. */
    gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    gtk_box_pack_start(GTK_BOX(hbox), entry, true, true, 0);

    browse = gtk_button_new_with_label("Choose...");
    g_signal_connect(browse, "clicked", G_CALLBACK(browse_cb), entry);
    gtk_box_pack_start(GTK_BOX(hbox), browse, false, false, 0);

    gtk_box_pack_start(GTK_BOX(content), hbox, false, false, 0);

    hint = gtk_label_new(
        forced
        ? "Opened because Option (Alt) was held at launch, or because "
          "PUTTY_CONFIG_RELOCATE is set."
        : "To change this later, hold Option (Alt) while launching PuTTY, "
          "or set PUTTY_CONFIG_RELOCATE=1.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), true);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
    gtk_widget_set_margin_top(hint, 8);
    gtk_box_pack_start(GTK_BOX(content), hint, false, false, 0);

    gtk_widget_show_all(content);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    /*
     * Present explicitly: when the dialog is opened from a GtkApplication
     * it can otherwise end up behind whichever window happened to be
     * frontmost at the time.
     */
    gtk_window_present(GTK_WINDOW(dlg));

    while (true) {
        gint response = gtk_dialog_run(GTK_DIALOG(dlg));

        if (response != GTK_RESPONSE_ACCEPT)
            break;

        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        char *err = config_location_set(text);
        if (!err) {
            changed = true;
            break;
        }

        if (cfg_debug())
            fprintf(stderr, "[config-location] %s\n", err);
        show_error(GTK_WINDOW(dlg), err);
        sfree(err);
    }

    if (!changed) {
        /*
         * Remember the current location so we do not ask again. This also
         * creates the sessions subdirectory that marks the configuration as
         * present.
         */
        char *err = config_location_set(current);
        if (err) {
            if (cfg_debug())
                fprintf(stderr, "[config-location] keeping %s failed: %s\n",
                        current, err);
            sfree(err);
        }
    }

    if (cfg_debug())
        fprintf(stderr, "[config-location] dialog done: base=%s current=%s "
                "changed=%d\n", base, config_location_current(), changed);

    gtk_widget_destroy(dlg);
    sfree(msg);
    sfree(initial);
    sfree(base);
    sfree(current);

    return changed;
}

/*
 * Make sure <configuration folder>/logs exists, so that the default log
 * file name (see platform_default_filename in window.c) can be opened.
 * Failure is not fatal: logging just reports the error when it is used.
 */
static void ensure_logs_dir(void)
{
    char *dir = config_location_current();
    char *logs = dupprintf("%s/logs", dir);
    char *err = make_dir_path(logs, 0700);

    if (err) {
        if (cfg_debug())
            fprintf(stderr, "[config-location] cannot create %s: %s\n",
                    logs, err);
        sfree(err);
    }

    sfree(logs);
    sfree(dir);
}

void config_location_init(void)
{
    static bool already_done = false;
    const char *env;
    bool forced, configured;

    if (already_done)
        return;
    already_done = true;

    env = getenv("PUTTY_CONFIG_RELOCATE");
    forced = (env && *env && strcmp(env, "0") != 0);
    if (!forced)
        forced = option_key_held();

    configured = config_location_is_configured();

    if (cfg_debug()) {
        char *cur = config_location_current();
        fprintf(stderr, "[config-location] init: current=%s configured=%d "
                "forced=%d\n", cur, configured, forced);
        sfree(cur);
    }

    if (!forced && configured) {
        ensure_logs_dir();
        return;
    }

    ask_dialog(forced);
    ensure_logs_dir();
}