/*
 * PuTTY-beter: one file chooser for the whole application.
 *
 * GTK's own GtkFileChooserDialog does have a location bar, but it is hidden
 * behind Ctrl+L, which is not discoverable - so there is no obvious way to say
 * "use this exact path". Every place PuTTY asks for a file or a folder goes
 * through this function instead, and it puts a "Path:" entry across the top of
 * the dialog. Type a full path and press Return to use it; type a folder and
 * press Return to go there.
 *
 * It also gives the dialog a sane size and centres it on its parent. A dialog
 * opened from inside a backend read callback used to come up behind the
 * terminal with its title greyed and no keyboard focus; building, showing and
 * presenting it before the modal loop is entered is what fixes that.
 */

#include <gtk/gtk.h>

#include "putty.h"
#include "gtkcompat.h"
#include "gtkmisc.h"

/* Keep the path box showing where we are: the selected file, or the folder. */
static void path_show_file(GtkFileChooser *chooser, gpointer ventry)
{
    char *fn = gtk_file_chooser_get_filename(chooser);
    if (fn) {
        gtk_entry_set_text(GTK_ENTRY(ventry), fn);
        g_free(fn);
    }
}

static void path_show_folder(GtkFileChooser *chooser, gpointer ventry)
{
    char *dir = gtk_file_chooser_get_current_folder(chooser);
    if (dir) {
        gtk_entry_set_text(GTK_ENTRY(ventry), dir);
        g_free(dir);
    }
}

/*
 * Enter in the path box. A folder is navigated to; a file is chosen and the
 * dialog accepted, so typing a path and pressing Return is enough. A path that
 * does not exist yet is accepted only when we are choosing a file to write, in
 * which case that is exactly what the user means.
 */
static void path_activate(GtkEntry *entry, gpointer vdlg)
{
    GtkWidget *dlg = GTK_WIDGET(vdlg);
    GtkWidget *chooser = g_object_get_data(G_OBJECT(dlg), "our-chooser");
    gboolean writing = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(dlg), "our-writing"));
    const char *p = gtk_entry_get_text(entry);

    if (!chooser || !p || !*p)
        return;

    if (g_file_test(p, G_FILE_TEST_IS_DIR)) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), p);
    } else if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), p);
        gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    } else if (writing) {
        char *dir = g_path_get_dirname(p);
        if (g_file_test(dir, G_FILE_TEST_IS_DIR)) {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), dir);
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                              g_path_get_basename(p));
            gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
        }
        g_free(dir);
    }
}

char *our_file_chooser_run(GtkWindow *parent, const char *title,
                           bool for_directory, bool for_writing,
                           const char *initial)
{
    GtkFileChooserAction action;
    GtkWidget *dlg, *content, *row, *entry, *chooser;
    char *result = NULL;

    action = for_directory ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER :
        for_writing ? GTK_FILE_CHOOSER_ACTION_SAVE :
        GTK_FILE_CHOOSER_ACTION_OPEN;

    dlg = gtk_dialog_new_with_buttons(
        title, parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        STANDARD_CANCEL_LABEL, GTK_RESPONSE_CANCEL,
        for_directory ? "_Choose" :
        for_writing ? "_Save" : STANDARD_OPEN_LABEL, GTK_RESPONSE_ACCEPT,
        (const gchar *)NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 820, 560);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
    /* Or the title bar is drawn transparent; see gtkcompat.h. */
    gtk_dialog_fix_macos_titlebar(dlg);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(row), 6);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Path:"), FALSE, FALSE, 0);
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   "Type a full path and press Return,"
                                   " or browse below");
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);

    chooser = gtk_file_chooser_widget_new(action);
    gtk_box_pack_start(GTK_BOX(content), chooser, TRUE, TRUE, 0);

    /* Start where the current value points, if it points anywhere useful. */
    if (initial && *initial) {
        if (g_file_test(initial, G_FILE_TEST_IS_DIR)) {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                                initial);
        } else if (g_file_test(initial, G_FILE_TEST_IS_REGULAR)) {
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), initial);
        } else {
            /* A name that does not exist yet: open its folder, and prefill it. */
            char *dir = g_path_get_dirname(initial);
            if (g_file_test(dir, G_FILE_TEST_IS_DIR)) {
                char *base = g_path_get_basename(initial);
                gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                                    dir);
                gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                                  base);
                g_free(base);
            }
            g_free(dir);
        }
    }

    /*
     * GtkFileChooserDialog turns this on for itself in save mode; a bare
     * GtkFileChooserWidget does not, so say so explicitly rather than
     * silently start overwriting files.
     */
    if (for_writing)
        gtk_file_chooser_set_do_overwrite_confirmation(
            GTK_FILE_CHOOSER(chooser), true);

    g_object_set_data(G_OBJECT(dlg), "our-chooser", chooser);
    g_object_set_data(G_OBJECT(dlg), "our-writing",
                      GINT_TO_POINTER(for_writing));
    g_signal_connect(G_OBJECT(entry), "activate",
                     G_CALLBACK(path_activate), dlg);
    g_signal_connect(G_OBJECT(chooser), "selection-changed",
                     G_CALLBACK(path_show_file), entry);
    g_signal_connect(G_OBJECT(chooser), "current-folder-changed",
                     G_CALLBACK(path_show_folder), entry);
    path_show_folder(GTK_FILE_CHOOSER(chooser), entry);

    gtk_widget_show_all(dlg);
    gtk_window_present(GTK_WINDOW(dlg));

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        result = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

    gtk_widget_destroy(dlg);

    if (getenv("PUTTY_DIAG"))
        fprintf(stderr, "[filechooser] \"%s\" -> %s\n", title,
                result ? result : "(cancelled)");

    return result;
}
