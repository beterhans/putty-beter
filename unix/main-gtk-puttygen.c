/*
 * main-gtk-puttygen.c: the GtkApplication main() for the GTK PuTTYgen front
 * end.
 *
 * PuTTY-beter (PLAN.md section 4m): this is the macOS application wrapper
 * around unix/puttygen.c. It owns the GtkApplication, the menu bar (which
 * macOS shows in the system bar) and nothing else; the window and every action
 * live in puttygen.c. The pattern is the same as
 * unix/main-gtk-application.c, and the action names here ("win.openkey" and
 * friends) match the GActionEntry table in puttygen.c.
 */

#include <stdlib.h>
#include <unistd.h>

#include <gtk/gtk.h>

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"

#if !GTK_CHECK_VERSION(3,0,0)
#error This front end only works in GTK 3
#endif

/*
 * Stubs. x_get_default() is called by unix/storage.c (linked in through
 * puttygen-common), and buildinfo_gtk_relevant tells version.c that this
 * program really does link GTK, so buildinfo() should report the version.
 */
char *x_get_default(const char *key) { return NULL; }
const bool buildinfo_gtk_relevant = true;

/* Defined in puttygen.c: build the one PuTTYgen window. */
GtkWidget *puttygen_window_new(GtkApplication *app);

static void startup(GApplication *app, gpointer user_data)
{
    GMenu *menubar, *menu, *section;

    menubar = g_menu_new();

    menu = g_menu_new();
    g_menu_append_submenu(menubar, "File", G_MENU_MODEL(menu));

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Open Key\u2026", "win.openkey");

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Save Private Key\u2026", "win.savepriv");
    g_menu_append(section, "Save Public Key\u2026", "win.savepub");

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Quit", "win.quit");

    menu = g_menu_new();
    g_menu_append_submenu(menubar, "Edit", G_MENU_MODEL(menu));

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Copy", "win.copy");
    g_menu_append(section, "Paste", "win.paste");

    menu = g_menu_new();
    g_menu_append_submenu(menubar, "Key", G_MENU_MODEL(menu));

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Generate", "win.generate");
    g_menu_append(section, "Cancel", "win.cancel");

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "Private Key File Parameters\u2026", "win.params");
    g_menu_append(section, "Export OpenSSH Key\u2026", "win.exportopenssh");
    g_menu_append(section, "Export ssh.com Key\u2026", "win.exportsshcom");

    menu = g_menu_new();
    g_menu_append_submenu(menubar, "Help", G_MENU_MODEL(menu));

    section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_menu_append(section, "About PuTTYgen", "win.about");
    g_menu_append(section, "Licence", "win.licence");

#if GTK_CHECK_VERSION(3,12,0)
#define SET_ACCEL(app, command, accel) do                       \
    {                                                           \
        static const char *const accels[] = { accel, NULL };    \
        gtk_application_set_accels_for_action(                  \
            GTK_APPLICATION(app), command, accels);             \
    } while (0)
#else
#define SET_ACCEL(app, command, accel) \
    gtk_application_add_accelerator(GTK_APPLICATION(app), accel, \
                                    command, NULL)
#endif

    SET_ACCEL(app, "win.generate", "<Primary>g");
    SET_ACCEL(app, "win.openkey", "<Primary>o");
    SET_ACCEL(app, "win.savepriv", "<Primary>s");
    SET_ACCEL(app, "win.copy", "<Primary>c");
    SET_ACCEL(app, "win.paste", "<Primary>v");

#undef SET_ACCEL

    gtk_application_set_menubar(GTK_APPLICATION(app), G_MENU_MODEL(menubar));
}

static void activate(GApplication *app, gpointer user_data)
{
    GtkWidget *win = puttygen_window_new(GTK_APPLICATION(app));
    /* puttygen_window_new() has already shown the window and fixed up the
     * widgets that only belong to the "generating" state. */
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    int status;
    GtkApplication *app;

    /*
     * The macOS launcher (unix/osxlaunch.c, installed as Contents/MacOS/PuTTYGen)
     * always appends one extra argument - an unused environment-variable prefix
     * that the terminal front ends use to restore a clean environment for their
     * shell subprocesses (main-gtk-application.c's pty_osx_envrestore_prefix).
     * PuTTYgen has no subprocesses and takes no command line, so the value is
     * not needed; but it must still be consumed, or GtkApplication treats it as
     * a file name and refuses to start with "This application can not open
     * files."
     */
    if (argc > 1)
        argc--;
    argv[argc] = NULL;

    /*
     * Do not let the file chooser's "Path:" box start somewhere random: the
     * same chdir($HOME) the main PuTTY front end does.
     */
    {
        const char *home = getenv("HOME");
        if (home) {
            if (chdir(home)) {}
        }
    }

    app = gtk_application_new("org.tartarus.projects.putty.macputtygen",
                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
