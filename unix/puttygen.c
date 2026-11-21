/*
 * puttygen.c: the GTK front end for PuTTYgen on Unix/macOS.
 *
 * PuTTY-beter: a port of windows/puttygen.c (PLAN.md section 4m). Windows
 * puttygen.c is treated as the specification, not transliterated: the
 * cryptographic work (keygen(), import.c, the ppk_* reader/writer, Argon2) is
 * all linked in unchanged, and this file is only the window around it. As a
 * port it deliberately drops two things the Windows version has, both recorded
 * in PLAN.md section 4m "Open decisions":
 *
 *   - SSH-1 keys. There are no SSH-1 controls, no RSA1 key type and no SSH-1
 *     save. The command-line puttygen still has them.
 *   - OpenSSH certificates (add/remove/cert-info). A loaded key which carries a
 *     certificate is displayed and can be re-saved, but not created here.
 *
 * Everything that asks for a file goes through our_file_chooser_run(), the
 * application's one file chooser (with the visible "Path:" box), exactly as the
 * main PuTTY does. Nothing here builds its own GtkFileChooserDialog.
 *
 * Debugging: set PUTTYGEN_DEBUG=1 in the environment for a trace of every
 * action on stderr.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"
#include "ssh.h"
#include "sshkeygen.h"
#include "gtkcompat.h"
#include "gtkmisc.h"
#include "licence.h"

#if !GTK_CHECK_VERSION(3,0,0)
#error The GTK PuTTYgen front end only works in GTK 3
#endif

/* ----------------------------------------------------------------------
 * Debug trace.
 */

static bool pg_debug_on(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTYGEN_DEBUG") != NULL);
    return cached;
}

/*
 * These three are normally provided by unix/console.c (for the command-line
 * tools) or unix/dialog.c (for the terminal front ends). Puttygenapp links
 * neither, so it supplies GUI versions of its own. There is no window known at
 * the time modalfatalbox() can be called, hence parentless dialogs.
 */
NORETURN void modalfatalbox(const char *fmt, ...)
{
    char *msg;
    va_list ap;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    fprintf(stderr, "puttygen: fatal error: %s\n", msg);

    {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Fatal error: %s", msg);
        gtk_window_set_title(GTK_WINDOW(dlg), "PuTTYgen Fatal Error");
        gtk_dialog_fix_macos_titlebar(dlg);   /* opaque title bar, see gtkcompat.h */
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
    sfree(msg);
    exit(1);
}

void nonfatal(const char *fmt, ...)
{
    char *msg;
    va_list ap;
    GtkWidget *dlg;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    fprintf(stderr, "puttygen: error: %s\n", msg);

    dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                 GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                 "%s", msg);
    gtk_dialog_fix_macos_titlebar(dlg);       /* opaque title bar, see gtkcompat.h */
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    sfree(msg);
}

void old_keyfile_warning(void)
{
    GtkWidget *dlg = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
        "%s",
        "You are loading an SSH-2 private key which has an\n"
        "old version of the file format. This means your key\n"
        "file is not fully tamperproof. Future versions of\n"
        "PuTTY may stop supporting this private key format,\n"
        "so we recommend you convert your key to the new\n"
        "format.\n"
        "\n"
        "Once the key is loaded into PuTTYgen, you can perform\n"
        "this conversion simply by saving it again.");
    gtk_window_set_title(GTK_WINDOW(dlg), "PuTTY Key File Warning");
    gtk_dialog_fix_macos_titlebar(dlg);       /* opaque title bar, see gtkcompat.h */
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void pg_debug(const char *fmt, ...)
{
    va_list ap;

    if (!pg_debug_on())
        return;

    fputs("[puttygen] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ----------------------------------------------------------------------
 * Key types this front end offers.
 */

typedef enum { KT_RSA, KT_DSA, KT_ECDSA, KT_EDDSA } keytype;

/*
 * Which private-key output a "Save" means: SAVE_PRIVATE writes PuTTY's own
 * .ppk; the others are the conversions.
 */
enum save_kind { SAVE_PRIVATE, SAVE_OPENSSH_AUTO, SAVE_OPENSSH_NEW, SAVE_SSHCOM };

static const char *keytype_name(keytype kt)
{
    switch (kt) {
      case KT_RSA:   return "RSA";
      case KT_DSA:   return "DSA";
      case KT_ECDSA: return "ECDSA";
      case KT_EDDSA: return "EdDSA";
      default:       return "?";
    }
}

/* Curve names, for the label beside the curve picker. */
static const char *curve_name(int bits)
{
    switch (bits) {
      case 256: return "NIST P-256";
      case 384: return "NIST P-384";
      case 521: return "NIST P-521";
      case 255: return "Ed25519";
      case 448: return "Ed448";
      default:  return "?";
    }
}

/* ----------------------------------------------------------------------
 * ProgressReceiver implementation: same weighted-phase arithmetic as
 * windows/puttygen.c, but the "position" is a fraction 0..1 that the main
 * thread polls and copies into the GtkProgressBar. The worker thread never
 * touches a widget.
 */

struct progressphase {
    double startpoint, total;
    /* For probabilistic phases */
    double exp_probability, exp_current_value;
};

struct progress {
    size_t nphases, phasessize;
    struct progressphase *phases, *currphase;

    struct puttygen_state *st;

    ProgressReceiver rec;
};

struct puttygen_state {
    GtkWidget *window;

    /* Controls */
    GtkWidget *keytype_combo;
    GtkWidget *bits_label, *bits_spin;
    GtkWidget *curve_label, *curve_combo;
    GtkWidget *policy_label, *policy_combo;
    GtkWidget *strong_check;
    GtkWidget *generate_btn, *cancel_btn;
    GtkWidget *open_btn, *save_btn, *savepub_btn;
    GtkWidget *export_openssh_btn, *export_sshcom_btn, *params_btn;
    GtkWidget *progress, *status_label;
    GtkWidget *details_frame;
    GtkWidget *comment_entry;
    GtkWidget *pass1_entry, *pass2_entry;
    GtkWidget *fptype_combo, *fingerprint_label;
    GtkWidget *keydisplay_view;
    GtkTextBuffer *keydisplay_buf;

    /* Menu actions we enable/disable with the state */
    GSimpleAction *act_open, *act_save, *act_savepub;
    GSimpleAction *act_export_openssh, *act_export_sshcom, *act_params;

    /* Pristine copies of the action set, so we can grey the menu out */
    /* (GTK keeps no list we can walk, so we hold the pointers above.) */

    /* The key */
    bool key_exists;
    bool have_private;
    /*
     * Whether ssh2key.key points into the `keys` union below (a key we
     * generated: the union owns it) or to a heap ssh_key handed over by
     * ppk_load_f()/import_ssh2() (a key we loaded: we must free it when it is
     * replaced). Generating a second key, or loading a key over a generated
     * one, used to overwrite the pointer without freeing the loadable one.
     */
    bool key_in_union;
    keytype ktype;
    int key_bits;                       /* RSA/DSA bit count */
    int curve_bits;                     /* ECDSA/EdDSA curve length */
    const PrimeGenerationPolicy *primepolicy;
    bool rsa_strong;
    FingerprintType fptype;
    ssh2_userkey ssh2key;
    union {
        RSAKey rsa;
        struct dsa_key dsa;
        struct ecdsa_key ec;
        struct eddsa_key ed;
    } keys;

    /* Generation, and the handover from the worker thread */
    GMutex mutex;
    gint generation_running;            /* atomic */
    gint cancel_requested;              /* atomic */
    bool generation_done;               /* under mutex */
    bool cancelled;                     /* under mutex */
    double progress_value;              /* under mutex, 0..1 */
    guint poll_source;
    GThread *thread;
    jmp_buf abort_jmp;
    bool jmp_active;

    struct ppk_save_parameters save_params;
};

static struct progress *progress_from_rec(ProgressReceiver *prog)
{
    return container_of(prog, struct progress, rec);
}

/* Defined further down; used by the generation-completion path. */
static bool selftest_active(void);
static void selftest_finish(struct puttygen_state *st);
static gboolean selftest_quit_cb(gpointer vwin);
static void release_current_key(struct puttygen_state *st);

static struct puttygen_state *progress_state(ProgressReceiver *prog)
{
    return progress_from_rec(prog)->st;
}

/*
 * Cancellation. There is no cancellation hook in sshkeygen.h: the user's
 * Cancel button therefore unwinds the generator with a longjmp back to the
 * worker thread's entry point. The memory the generator had allocated at that
 * moment is leaked (once, per cancel), which is acceptable for a GUI that is
 * about to throw the half-made key away; what must not happen is a partially
 * written key file, and none is written until the key is complete.
 */
static void progress_check_cancel(struct progress *p)
{
    struct puttygen_state *st = p->st;

    if (st->jmp_active && g_atomic_int_get(&st->cancel_requested))
        longjmp(st->abort_jmp, 1);
}

static ProgressPhase pg_progress_add_linear(ProgressReceiver *prog,
                                            double overall_cost)
{
    struct progress *p = progress_from_rec(prog);
    sgrowarray(p->phases, p->phasessize, p->nphases);
    int phase = p->nphases++;
    p->phases[phase].total = overall_cost;
    ProgressPhase ph = { .n = phase };
    return ph;
}

static ProgressPhase pg_progress_add_probabilistic(ProgressReceiver *prog,
                                                   double cost_per_attempt,
                                                   double probability)
{
    struct progress *p = progress_from_rec(prog);
    sgrowarray(p->phases, p->phasessize, p->nphases);
    int phase = p->nphases++;
    p->phases[phase].exp_probability = 1.0 - probability;
    p->phases[phase].exp_current_value = 1.0;
    p->phases[phase].total = cost_per_attempt / probability;
    ProgressPhase ph = { .n = phase };
    return ph;
}

static void pg_progress_ready(ProgressReceiver *prog)
{
    struct progress *p = progress_from_rec(prog);
    double total = 0;
    for (size_t i = 0; i < p->nphases; i++) {
        p->phases[i].startpoint = total;
        total += p->phases[i].total;
    }
    g_mutex_lock(&p->st->mutex);
    p->st->progress_value = 0;
    g_mutex_unlock(&p->st->mutex);
}

static void pg_progress_start_phase(ProgressReceiver *prog,
                                    ProgressPhase phase)
{
    struct progress *p = progress_from_rec(prog);
    assert(phase.n < p->nphases);
    p->currphase = &p->phases[phase.n];
    progress_check_cancel(p);
}

static void pg_progress_update(struct progress *p, double phasepos)
{
    struct puttygen_state *st = p->st;
    double total = 0;

    for (size_t i = 0; i < p->nphases; i++)
        total += p->phases[i].total;

    if (total <= 0)
        total = 1;

    double position = p->currphase->startpoint + p->currphase->total * phasepos;
    position /= total;
    if (position < 0)
        position = 0;
    if (position > 1)
        position = 1;

    g_mutex_lock(&st->mutex);
    st->progress_value = position;
    g_mutex_unlock(&st->mutex);
}

static void pg_progress_report(ProgressReceiver *prog, double progress)
{
    struct progress *p = progress_from_rec(prog);
    progress_check_cancel(p);
    pg_progress_update(p, progress);
}

static void pg_progress_report_attempt(ProgressReceiver *prog)
{
    struct progress *p = progress_from_rec(prog);
    progress_check_cancel(p);
    p->currphase->exp_current_value *= p->currphase->exp_probability;
    pg_progress_update(p, 1.0 - p->currphase->exp_current_value);
}

static void pg_progress_report_phase_complete(ProgressReceiver *prog)
{
    struct progress *p = progress_from_rec(prog);
    progress_check_cancel(p);
    pg_progress_update(p, 1.0);
}

static const ProgressReceiverVtable pg_progress_vt = {
    .add_linear = pg_progress_add_linear,
    .add_probabilistic = pg_progress_add_probabilistic,
    .ready = pg_progress_ready,
    .start_phase = pg_progress_start_phase,
    .report = pg_progress_report,
    .report_attempt = pg_progress_report_attempt,
    .report_phase_complete = pg_progress_report_phase_complete,
};

/* ----------------------------------------------------------------------
 * Small GUI helpers.
 */

static void gui_message(struct puttygen_state *st, GtkMessageType type,
                        const char *title, const char *fmt, ...)
{
    GtkWidget *dlg;
    char *msg;
    va_list ap;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    pg_debug("message (%s): %s", title, msg);

    dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
                                 GTK_DIALOG_MODAL |
                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                 type, GTK_BUTTONS_OK, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    /* Opaque title bar on macOS; see gtkcompat.h. */
    gtk_dialog_fix_macos_titlebar(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    sfree(msg);
}

static bool gui_confirm(struct puttygen_state *st, const char *title,
                        const char *fmt, ...)
{
    GtkWidget *dlg;
    char *msg;
    va_list ap;
    int response;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);

    pg_debug("confirm (%s): %s", title, msg);

    dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
                                 GTK_DIALOG_MODAL |
                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                 GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
                                 "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_fix_macos_titlebar(dlg);       /* opaque title bar, see gtkcompat.h */
    response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    sfree(msg);

    return response == GTK_RESPONSE_YES;
}

/*
 * Ask for a passphrase (used when loading an encrypted key). The comment, if
 * we know it, is shown so the user can tell two keys apart. Returns a
 * newly-allocated passphrase, or NULL if the user cancelled.
 */
static char *ask_passphrase(struct puttygen_state *st, const char *comment,
                            const char *prompt)
{
    GtkWidget *dlg, *content, *label, *entry;
    char *result = NULL;
    int response;

    dlg = gtk_dialog_new_with_buttons(
        "PuTTYgen: passphrase", GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        STANDARD_CANCEL_LABEL, GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_ACCEPT, (const gchar *)NULL);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
    /* Opaque title bar on macOS; see gtkcompat.h. */
    gtk_dialog_fix_macos_titlebar(dlg);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    if (comment && *comment) {
        char *l = dupprintf("Key comment: %s", comment);
        label = gtk_label_new(l);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
        sfree(l);
    }

    label = gtk_label_new(prompt);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);

    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);

    response = gtk_dialog_run(GTK_DIALOG(dlg));
    if (response == GTK_RESPONSE_ACCEPT)
        result = dupstr(gtk_entry_get_text(GTK_ENTRY(entry)));

    gtk_widget_destroy(dlg);
    pg_debug("passphrase prompt %s", result ? "accepted" : "cancelled");
    return result;
}

/* ----------------------------------------------------------------------
 * The .ppk v3 save parameters dialog (port of PPKParamsProc).
 */

static void params_sync_widgets(GtkWidget *dlg)
{
    struct ppk_save_parameters *params =
        g_object_get_data(G_OBJECT(dlg), "params");
    GtkWidget *mem = g_object_get_data(G_OBJECT(dlg), "mem");
    GtkWidget *time = g_object_get_data(G_OBJECT(dlg), "time");
    GtkWidget *auto_yes = g_object_get_data(G_OBJECT(dlg), "auto_yes");
    GtkWidget *auto_no = g_object_get_data(G_OBJECT(dlg), "auto_no");
    GtkWidget *kdf_stack = g_object_get_data(G_OBJECT(dlg), "kdf_stack");
    char buf[64];

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem), params->argon2_mem);
    if (params->argon2_passes_auto)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(time),
                                  params->argon2_milliseconds);
    else
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(time),
                                  params->argon2_passes);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
        params->argon2_passes_auto ? auto_yes : auto_no), TRUE);

    gtk_widget_set_sensitive(time, params->fmt_version == 3);
    gtk_widget_set_sensitive(kdf_stack, params->fmt_version == 3);
    /* Label the time field's units, which differ with the auto flag. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem),
                              params->argon2_mem);
    snprintf(buf, sizeof(buf), "%u", params->argon2_mem);
    (void)buf;
}

static void params_type_changed(GtkToggleButton *btn, gpointer vdlg)
{
    GtkWidget *dlg = GTK_WIDGET(vdlg);
    struct ppk_save_parameters *params =
        g_object_get_data(G_OBJECT(dlg), "params");
    GtkWidget *ver3 = g_object_get_data(G_OBJECT(dlg), "ver3");

    params->fmt_version = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ver3)) ? 3 : 2;
    pg_debug("ppk fmt_version = %u", params->fmt_version);
    params_sync_widgets(dlg);
}

static void params_auto_changed(GtkToggleButton *btn, gpointer vdlg)
{
    GtkWidget *dlg = GTK_WIDGET(vdlg);
    struct ppk_save_parameters *params =
        g_object_get_data(G_OBJECT(dlg), "params");
    GtkWidget *auto_yes = g_object_get_data(G_OBJECT(dlg), "auto_yes");

    params->argon2_passes_auto =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_yes));
    pg_debug("ppk auto = %d", (int)params->argon2_passes_auto);
    params_sync_widgets(dlg);
}

static void params_spin_changed(GtkSpinButton *spin, gpointer vdlg)
{
    GtkWidget *dlg = GTK_WIDGET(vdlg);
    struct ppk_save_parameters *params =
        g_object_get_data(G_OBJECT(dlg), "params");
    GtkWidget *mem = g_object_get_data(G_OBJECT(dlg), "mem");
    GtkWidget *time = g_object_get_data(G_OBJECT(dlg), "time");
    GtkWidget *parallel = g_object_get_data(G_OBJECT(dlg), "parallel");
    const GtkWidget *which = GTK_WIDGET(spin);

    params->argon2_mem = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mem));
    if (params->argon2_passes_auto)
        params->argon2_milliseconds =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(time));
    else
        params->argon2_passes =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(time));
    params->argon2_parallelism =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(parallel));

    pg_debug("ppk params mem=%u time=%u auto=%d parallel=%u",
             params->argon2_mem,
             params->argon2_passes_auto ? params->argon2_milliseconds :
             params->argon2_passes,
             (int)params->argon2_passes_auto,
             params->argon2_parallelism);
    (void)which;
}

static void show_params_dialog(struct puttygen_state *st)
{
    GtkWidget *dlg, *content, *grid;
    GtkWidget *ver2, *ver3, *kdf_combo, *mem, *time, *parallel;
    GtkWidget *auto_yes, *auto_no;
    int response;

    dlg = gtk_dialog_new_with_buttons(
        "PuTTYgen: private key file parameters", GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        STANDARD_CANCEL_LABEL, GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_ACCEPT, (const gchar *)NULL);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
    /* Opaque title bar on macOS; see gtkcompat.h. */
    gtk_dialog_fix_macos_titlebar(dlg);

    content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    /* Format version */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Format:"),
                    0, 0, 1, 1);
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        ver2 = gtk_radio_button_new_with_label(NULL, "v2 (insecure)");
        ver3 = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(ver2), "v3");
        gtk_box_pack_start(GTK_BOX(row), ver2, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), ver3, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), row, 1, 0, 1, 1);
    }

    /* KDF flavour */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Key derivation:"),
                    0, 1, 1, 1);
    kdf_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kdf_combo),
                                   "Argon2id");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kdf_combo),
                                   "Argon2i");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kdf_combo),
                                   "Argon2d");
    switch (st->save_params.argon2_flavour) {
      case Argon2i:  gtk_combo_box_set_active(GTK_COMBO_BOX(kdf_combo), 1); break;
      case Argon2d:  gtk_combo_box_set_active(GTK_COMBO_BOX(kdf_combo), 2); break;
      default:       gtk_combo_box_set_active(GTK_COMBO_BOX(kdf_combo), 0); break;
    }
    gtk_grid_attach(GTK_GRID(grid), kdf_combo, 1, 1, 1, 1);

    /* Memory */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Memory (Kbyte):"),
                    0, 2, 1, 1);
    mem = gtk_spin_button_new_with_range(1, 1u << 22, 64);
    gtk_grid_attach(GTK_GRID(grid), mem, 1, 2, 1, 1);

    /* Parallelism */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Parallelism:"),
                    0, 3, 1, 1);
    parallel = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_grid_attach(GTK_GRID(grid), parallel, 1, 3, 1, 1);

    /* Time / passes */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Cost:"), 0, 4, 1, 1);
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto_yes = gtk_radio_button_new_with_label(NULL, "Time (ms):");
        auto_no = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(auto_yes), "Passes:");
        gtk_box_pack_start(GTK_BOX(row), auto_yes, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), auto_no, FALSE, FALSE, 0);
        time = gtk_spin_button_new_with_range(1, 1000000, 1);
        gtk_box_pack_start(GTK_BOX(row), time, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), row, 1, 4, 1, 1);
        gtk_widget_set_sensitive(auto_yes, TRUE);

        /*
         * The "Key derivation:" controls only apply to v3; keep the two
         * v3-only rows in a stack so they can be greyed out together.
         */
        g_object_set_data(G_OBJECT(dlg), "kdf_stack", grid);
    }

    g_object_set_data(G_OBJECT(dlg), "params", &st->save_params);
    g_object_set_data(G_OBJECT(dlg), "mem", mem);
    g_object_set_data(G_OBJECT(dlg), "time", time);
    g_object_set_data(G_OBJECT(dlg), "parallel", parallel);
    g_object_set_data(G_OBJECT(dlg), "auto_yes", auto_yes);
    g_object_set_data(G_OBJECT(dlg), "auto_no", auto_no);
    g_object_set_data(G_OBJECT(dlg), "ver3", ver3);

    g_signal_connect(ver2, "toggled", G_CALLBACK(params_type_changed), dlg);
    g_signal_connect(ver3, "toggled", G_CALLBACK(params_type_changed), dlg);
    g_signal_connect(auto_yes, "toggled", G_CALLBACK(params_auto_changed), dlg);
    g_signal_connect(auto_no, "toggled", G_CALLBACK(params_auto_changed), dlg);
    g_signal_connect(mem, "value-changed",
                     G_CALLBACK(params_spin_changed), dlg);
    g_signal_connect(time, "value-changed",
                     G_CALLBACK(params_spin_changed), dlg);
    g_signal_connect(parallel, "value-changed",
                     G_CALLBACK(params_spin_changed), dlg);

    if (st->save_params.fmt_version >= 3)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ver3), TRUE);
    else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ver2), TRUE);

    params_sync_widgets(dlg);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);

    response = gtk_dialog_run(GTK_DIALOG(dlg));
    if (response == GTK_RESPONSE_ACCEPT) {
        /* Adopt the controls' values (signals have already done most of it). */
        st->save_params.argon2_flavour =
            gtk_combo_box_get_active(GTK_COMBO_BOX(kdf_combo)) == 1 ? Argon2i :
            gtk_combo_box_get_active(GTK_COMBO_BOX(kdf_combo)) == 2 ? Argon2d :
            Argon2id;

        char *err = ppk_params_bad(&st->save_params, true, 64);
        if (err) {
            gui_message(st, GTK_MESSAGE_ERROR, "Save parameters invalid",
                        "%s", err);
            sfree(err);
        }
    }

    gtk_widget_destroy(dlg);
}

/* ----------------------------------------------------------------------
 * Updating the window from the current key.
 */

static void set_status(struct puttygen_state *st, const char *text)
{
    gtk_label_set_text(GTK_LABEL(st->status_label), text);
}

static void show_fingerprint(struct puttygen_state *st)
{
    char *fp;

    if (!st->ssh2key.key) {
        gtk_label_set_text(GTK_LABEL(st->fingerprint_label), "");
        return;
    }

    fp = ssh2_fingerprint(st->ssh2key.key, st->fptype);
    gtk_label_set_text(GTK_LABEL(st->fingerprint_label), fp);
    gtk_label_set_selectable(GTK_LABEL(st->fingerprint_label), TRUE);
    sfree(fp);
}

static void show_pubkey(struct puttygen_state *st)
{
    char *text;

    if (!st->ssh2key.key) {
        gtk_text_buffer_set_text(st->keydisplay_buf, "", -1);
        return;
    }

    if (ssh_key_alg(st->ssh2key.key)->is_certificate) {
        text = dupstr("This public key contains an OpenSSH certificate.\n"
                      "(Certificates cannot be created by this GUI; the "
                      "puttygen command line can add or remove one.)");
    } else {
        text = ssh2_pubkey_openssh_str(&st->ssh2key);
    }

    gtk_text_buffer_set_text(st->keydisplay_buf, text, -1);
    sfree(text);
}

static void refresh_key_display(struct puttygen_state *st)
{
    show_fingerprint(st);
    show_pubkey(st);
}

/*
 * NULL-tolerant widget helpers: set_state() runs while the window is being
 * built, when some controls (e.g. the Open button) may not exist yet.
 */
static void wsens(GtkWidget *w, bool v)
{
    if (w)
        gtk_widget_set_sensitive(w, v);
}

static void wvis(GtkWidget *w, bool v)
{
    if (w)
        gtk_widget_set_visible(w, v);
}

/*
 * Turn controls on and off as a whole. status: 0 = no key, 1 = generating a
 * key, 2 = a key is loaded.
 */
static void set_state(struct puttygen_state *st, int status)
{
    bool idle = (status != 1);
    bool havekey = (status == 2);

    wsens(st->generate_btn, idle);
    wsens(st->open_btn, idle);
    wsens(st->keytype_combo, idle);
    wsens(st->bits_spin, idle);
    wsens(st->curve_combo, idle);
    wsens(st->policy_combo, idle);
    wsens(st->strong_check, idle && st->ktype == KT_RSA);
    wvis(st->cancel_btn, !idle);
    wvis(st->progress, !idle);
    wvis(st->status_label, !idle);

    wsens(st->details_frame, havekey);
    wsens(st->save_btn, havekey && st->have_private);
    wsens(st->savepub_btn, havekey);
    wsens(st->export_openssh_btn, havekey && st->have_private);
    wsens(st->export_sshcom_btn, havekey && st->have_private);
    wsens(st->params_btn, havekey && st->have_private);

    if (st->act_open)
        g_simple_action_set_enabled(st->act_open, idle);
    if (st->act_save)
        g_simple_action_set_enabled(st->act_save, havekey && st->have_private);
    if (st->act_savepub)
        g_simple_action_set_enabled(st->act_savepub, havekey);
    if (st->act_export_openssh)
        g_simple_action_set_enabled(st->act_export_openssh,
                                    havekey && st->have_private);
    if (st->act_export_sshcom)
        g_simple_action_set_enabled(st->act_export_sshcom,
                                    havekey && st->have_private);
    if (st->act_params)
        g_simple_action_set_enabled(st->act_params,
                                    havekey && st->have_private);
}

/*
 * The key-type combo lists Ed25519, Ed448, ECDSA, RSA, DSA - which is not the
 * order of the keytype enum, so map it explicitly.
 */
static keytype combo_to_keytype(int idx, int *curve_bits)
{
    switch (idx) {
      case 0: *curve_bits = 255; return KT_EDDSA;
      case 1: *curve_bits = 448; return KT_EDDSA;
      case 2: return KT_ECDSA;
      case 3: return KT_RSA;
      case 4: return KT_DSA;
      default: *curve_bits = 255; return KT_EDDSA;
    }
}

/*
 * Show only the size control that the current key type uses, and give the
 * curve picker the right contents (NIST curves for ECDSA, Edwards curves for
 * EdDSA).
 */
static void update_key_type_ctrls(struct puttygen_state *st)
{
    bool bits = (st->ktype == KT_RSA || st->ktype == KT_DSA);
    bool curve = (st->ktype == KT_ECDSA || st->ktype == KT_EDDSA);

    gtk_widget_set_visible(st->bits_label, bits);
    gtk_widget_set_visible(st->bits_spin, bits);
    gtk_widget_set_visible(st->curve_label, curve);
    gtk_widget_set_visible(st->curve_combo, curve);

    bool prime = (st->ktype == KT_RSA || st->ktype == KT_DSA);
    gtk_widget_set_visible(st->policy_label, prime);
    gtk_widget_set_visible(st->policy_combo, prime);
    gtk_widget_set_visible(st->strong_check, st->ktype == KT_RSA);

    if (st->ktype == KT_DSA) {
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(st->bits_spin),
                                  1024, 3072);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->bits_spin), 1024);
    } else if (st->ktype == KT_RSA) {
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(st->bits_spin),
                                  512, 16384);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->bits_spin), 2048);
    }

    if (curve) {
        const int *lens = (st->ktype == KT_ECDSA ? ec_nist_curve_lengths :
                           ec_ed_curve_lengths);
        size_t n = (st->ktype == KT_ECDSA ? n_ec_nist_curve_lengths :
                    n_ec_ed_curve_lengths);
        int want = st->curve_bits;
        int sel = 0;

        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(st->curve_combo));
        for (size_t i = 0; i < n; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->curve_combo),
                                           curve_name(lens[i]));
            if (lens[i] == want)
                sel = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(st->curve_combo), sel);
        st->curve_bits = lens[sel];
    }
}

static void keytype_changed(GtkComboBox *combo, gpointer vst)
{
    struct puttygen_state *st = vst;
    st->ktype = combo_to_keytype(gtk_combo_box_get_active(combo),
                                 &st->curve_bits);
    pg_debug("key type -> %s", keytype_name(st->ktype));
    update_key_type_ctrls(st);
}

/* ----------------------------------------------------------------------
 * Generating a key, on a worker thread.
 */

struct gen_params {
    struct puttygen_state *st;
    keytype ktype;
    int key_bits;
    int curve_bits;
    const PrimeGenerationPolicy *primepolicy;
    bool rsa_strong;
};

static gpointer generate_key_thread(gpointer vparams)
{
    struct gen_params *params = vparams;
    struct puttygen_state *st = params->st;
    struct progress prog;
    bool cancelled = false;

    prog.nphases = prog.phasessize = 0;
    prog.phases = prog.currphase = NULL;
    prog.st = st;
    prog.rec.vt = &pg_progress_vt;

    /*
     * A cancel request arrives asynchronously; from here until the generator
     * returns, unwinding is done with longjmp back to this point.
     */
    if (setjmp(st->abort_jmp)) {
        cancelled = true;
        pg_debug("key generation cancelled");
        goto finished;
    }
    st->jmp_active = true;

    {
        PrimeGenerationContext *pgc =
            primegen_new_context(params->primepolicy);

        if (params->ktype == KT_DSA) {
            dsa_generate(&st->keys.dsa, params->key_bits, pgc, &prog.rec);
        } else if (params->ktype == KT_ECDSA) {
            ecdsa_generate(&st->keys.ec, params->curve_bits);
        } else if (params->ktype == KT_EDDSA) {
            eddsa_generate(&st->keys.ed, params->curve_bits);
        } else {
            rsa_generate(&st->keys.rsa, params->key_bits, params->rsa_strong,
                         pgc, &prog.rec);
        }

        primegen_free_context(pgc);
    }

    st->jmp_active = false;

  finished:
    st->jmp_active = false;
    sfree(prog.phases);

    g_mutex_lock(&st->mutex);
    st->cancelled = cancelled;
    st->generation_done = true;
    g_mutex_unlock(&st->mutex);

    g_atomic_int_set(&st->generation_running, 0);

    sfree(params);
    return NULL;
}

/*
 * Called on the main thread every 50 ms while a key is being generated: it is
 * the only place the worker's progress is read, and the only place widgets are
 * touched.
 */
static gboolean poll_generation(gpointer vst)
{
    struct puttygen_state *st = vst;
    double value;
    bool done;

    g_mutex_lock(&st->mutex);
    value = st->progress_value;
    done = st->generation_done;
    g_mutex_unlock(&st->mutex);

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->progress), value);

    if (!done)
        return G_SOURCE_CONTINUE;

    st->poll_source = 0;

    if (st->cancelled) {
        if (selftest_active()) {
            fprintf(stderr, "puttygen: self-test: cancelled\n");
            g_idle_add(selftest_quit_cb, st->window);
            return G_SOURCE_REMOVE;
        }
        st->key_exists = false;
        st->have_private = false;
        release_current_key(st);
        sfree(st->ssh2key.comment);
        st->ssh2key.comment = NULL;
        set_status(st, "");
        set_state(st, 0);
        return G_SOURCE_REMOVE;
    }

    /*
     * Wire the freshly generated key into the userkey wrapper, as Windows
     * puttygen does at WM_DONEKEY. Whatever key was there before is let go
     * first, so generating twice does not leak the first one.
     */
    release_current_key(st);
    if (st->ktype == KT_DSA)
        st->ssh2key.key = &st->keys.dsa.sshk;
    else if (st->ktype == KT_ECDSA)
        st->ssh2key.key = &st->keys.ec.sshk;
    else if (st->ktype == KT_EDDSA)
        st->ssh2key.key = &st->keys.ed.sshk;
    else
        st->ssh2key.key = &st->keys.rsa.sshk;
    st->key_in_union = true;

    {
        char comment[80];
        struct tm tm = ltime();
        if (st->ktype == KT_DSA)
            strftime(comment, sizeof(comment), "dsa-key-%Y%m%d", &tm);
        else if (st->ktype == KT_ECDSA)
            strftime(comment, sizeof(comment), "ecdsa-key-%Y%m%d", &tm);
        else if (st->ktype == KT_EDDSA)
            strftime(comment, sizeof(comment), "eddsa-key-%Y%m%d", &tm);
        else
            strftime(comment, sizeof(comment), "rsa-key-%Y%m%d", &tm);

        sfree(st->ssh2key.comment);
        st->ssh2key.comment = dupstr(comment);
    }

    st->key_exists = true;
    st->have_private = true;

    gtk_entry_set_text(GTK_ENTRY(st->comment_entry), st->ssh2key.comment);
    gtk_entry_set_text(GTK_ENTRY(st->pass1_entry), "");
    gtk_entry_set_text(GTK_ENTRY(st->pass2_entry), "");

    refresh_key_display(st);
    set_status(st, "");
    set_state(st, 2);
    pg_debug("key generation finished");

    if (selftest_active()) {
        selftest_finish(st);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

static void start_generating_key(struct puttygen_state *st)
{
    struct gen_params *params;

    if (g_atomic_int_get(&st->generation_running)) {
        pg_debug("generate ignored: already running");
        return;
    }

    /* Read the controls now: they are disabled until the key is done. */
    st->key_bits = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(st->bits_spin));

    if (st->ktype == KT_ECDSA || st->ktype == KT_EDDSA) {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(st->curve_combo));
        const int *lens = (st->ktype == KT_ECDSA ? ec_nist_curve_lengths :
                           ec_ed_curve_lengths);
        size_t n = (st->ktype == KT_ECDSA ? n_ec_nist_curve_lengths :
                    n_ec_ed_curve_lengths);
        if (idx < 0 || (size_t)idx >= n)
            idx = 0;
        st->curve_bits = lens[idx];
    }
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(st->policy_combo))) {
      case 1:  st->primepolicy = &primegen_provable_maurer_simple; break;
      case 2:  st->primepolicy = &primegen_provable_maurer_complex; break;
      default: st->primepolicy = &primegen_probabilistic; break;
    }
    st->rsa_strong = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(st->strong_check));

    pg_debug("generate: %s bits=%d curve=%d policy=%p strong=%d",
             keytype_name(st->ktype), st->key_bits, st->curve_bits,
             (void *)st->primepolicy, (int)st->rsa_strong);

    if (!selftest_active() &&
        (st->ktype == KT_RSA || st->ktype == KT_DSA)) {
        if (st->key_bits < 512) {
            gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                        "Key size must be at least 512 bits.");
            return;
        }
        if (st->key_bits < 2048)
            gui_message(st, GTK_MESSAGE_WARNING, "PuTTYgen Warning",
                        "%s keys shorter than 2048 bits are probably not "
                        "secure.", keytype_name(st->ktype));
    }

    /*
     * Seed the random number generator from the OS, and hand it a fresh seed
     * every time a key is generated. Unlike the Windows version there is no
     * mouse-wiggling collection step: /dev/urandom is available here and is a
     * better source than mouse movements.
     *
     * The PRNG itself is created once, at startup (see puttygen_window_new);
     * it must not be created again here. random_setup_special() calls
     * random_setup_custom(), which calls random_create(), which asserts that no
     * PRNG exists yet - so calling it a second time aborted the process. That
     * was the "generating a second key crashes" bug: the first generation made
     * the PRNG and it then stayed for the life of the process.
     */
    {
        size_t seedbytes = (random_seed_bits() + 7) / 8;
        char *entropy = get_random_data(seedbytes, NULL);
        if (!entropy) {
            gui_message(st, GTK_MESSAGE_ERROR, "Key generation error",
                        "Failed to collect entropy; could not generate key.");
            return;
        }
        random_reseed(make_ptrlen(entropy, seedbytes));
        smemclr(entropy, seedbytes);
        sfree(entropy);
    }

    params = snew(struct gen_params);
    params->st = st;
    params->ktype = st->ktype;
    params->key_bits = st->key_bits;
    params->curve_bits = st->curve_bits;
    params->primepolicy = st->primepolicy;
    params->rsa_strong = st->rsa_strong;

    g_mutex_lock(&st->mutex);
    st->generation_done = false;
    st->cancelled = false;
    st->progress_value = 0;
    g_mutex_unlock(&st->mutex);
    g_atomic_int_set(&st->cancel_requested, 0);
    g_atomic_int_set(&st->generation_running, 1);

    set_status(st, "Please wait while a key is generated...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(st->progress), 0);
    set_state(st, 1);

    if (st->thread) {
        g_thread_unref(st->thread);
        st->thread = NULL;
    }
    st->thread = g_thread_new("puttygen-keygen", generate_key_thread, params);
    st->poll_source = g_timeout_add(50, poll_generation, st);
}

static void cancel_generation(struct puttygen_state *st)
{
    if (!g_atomic_int_get(&st->generation_running))
        return;
    pg_debug("cancel requested");
    g_atomic_int_set(&st->cancel_requested, 1);
}

/*
 * Let go of the key we are currently holding, whichever kind it is: a key we
 * generated lives in the `keys` union and only its internals are heap
 * allocated, while a key we loaded is a heap object that must be freed. Called
 * before a new key is put in place, so nothing is leaked or double-freed.
 */
static void release_current_key(struct puttygen_state *st)
{
    if (!st->ssh2key.key)
        return;

    if (!st->key_in_union)
        ssh_key_free(st->ssh2key.key);

    /*
     * A generated key is left alone in the union (its internals stay valid
     * until the union is overwritten by the next generation); we only drop our
     * reference to it.
     */
    st->ssh2key.key = NULL;
    st->key_in_union = false;
}

/*
 * Self-test hook. With PUTTYGEN_SELFTEST set (e.g. to "ed25519", "ed448",
 * "ecdsa", "rsa" or "dsa", with PUTTYGEN_SELFTEST_BITS to override the RSA/DSA
 * size), the window generates a key as soon as it is built, writes the
 * fingerprint and public key to stdout, optionally saves the private key to
 * PUTTYGEN_SELFTEST_OUT and quits. That is how this GUI is checked against the
 * command-line puttygen without a human at the keyboard.
 *
 * PUTTYGEN_SELFTEST_REPEAT=<n> generates n keys in a row before quitting, and
 * PUTTYGEN_SELFTEST_CANCEL=<ms> presses Cancel that many milliseconds into the
 * first one.
 */
static bool selftest_active(void)
{
    return getenv("PUTTYGEN_SELFTEST") != NULL ||
        getenv("PUTTYGEN_SELFTEST_LOAD") != NULL;
}

/*
 * How many keys a repeated self-test still has to generate. Kept here rather
 * than read from the environment each time so that the count survives across
 * key generations.
 */
static int selftest_runs_left = -1;

static bool selftest_generate_another(void)
{
    if (selftest_runs_left < 0) {
        const char *r = getenv("PUTTYGEN_SELFTEST_REPEAT");
        selftest_runs_left = r ? atoi(r) : 1;
        if (selftest_runs_left < 1)
            selftest_runs_left = 1;
    }

    if (selftest_runs_left <= 1)
        return false;

    selftest_runs_left--;
    return true;
}

static void load_key_file(struct puttygen_state *st, const char *path);

static gboolean selftest_quit_cb(gpointer vwin)
{
    gtk_widget_destroy(GTK_WIDGET(vwin));
    if (g_application_get_default())
        g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

/* Used by PUTTYGEN_SELFTEST_CANCEL: press Cancel after N milliseconds. */
static gboolean selftest_cancel_cb(gpointer vst)
{
    fprintf(stderr, "puttygen: self-test: pressing Cancel\n");
    cancel_generation((struct puttygen_state *)vst);
    return G_SOURCE_REMOVE;
}

static void selftest_finish(struct puttygen_state *st)
{
    char *fp = ssh2_fingerprint(st->ssh2key.key, st->fptype);
    char *pub = ssh2_pubkey_openssh_str(&st->ssh2key);
    const char *out = getenv("PUTTYGEN_SELFTEST_OUT");

    printf("type=%s\n", keytype_name(st->ktype));
    if (st->ktype == KT_RSA || st->ktype == KT_DSA)
        printf("bits=%d\n", st->key_bits);
    else
        printf("curve=%d\n", st->curve_bits);
    printf("comment=%s\n", st->ssh2key.comment ? st->ssh2key.comment : "");
    printf("fingerprint=%s\n", fp);
    printf("public=%s\n", pub);

    if (out && *out) {
        Filename *fn = filename_from_str(out);
        bool ok = ppk_save_f(fn, &st->ssh2key, NULL,
                             &st->save_params);
        printf("saved=%s\n", ok ? out : "(failed)");
        filename_free(fn);
    }

    fflush(stdout);
    sfree(fp);
    sfree(pub);

    /*
     * A repeated self-test (PUTTYGEN_SELFTEST_REPEAT) generates the next key
     * straight away, which is what exercises the "second key generation" path
     * that the once-only PRNG bug used to abort.
     */
    if (getenv("PUTTYGEN_SELFTEST") && selftest_generate_another()) {
        fprintf(stderr, "puttygen: self-test: generating another key (%d to go)\n",
                selftest_runs_left);
        start_generating_key(st);
        return;
    }

    /*
     * Quit from an idle callback rather than destroying the window right here:
     * in the load self-test this runs during construction, and the caller
     * (activate()) still holds the window pointer until it returns.
     */
    g_idle_add(selftest_quit_cb, st->window);
}

static void selftest_start(struct puttygen_state *st)
{
    const char *which = getenv("PUTTYGEN_SELFTEST");
    const char *bitsenv = getenv("PUTTYGEN_SELFTEST_BITS");
    const char *loadpath = getenv("PUTTYGEN_SELFTEST_LOAD");
    int bits = bitsenv ? atoi(bitsenv) : 0;
    int idx = 0;

    /*
     * Loading is checked first: with PUTTYGEN_SELFTEST_LOAD set, the given key
     * file is opened through exactly the path the Open Key button uses.
     */
    if (loadpath && *loadpath) {
        fprintf(stderr, "puttygen: self-test: loading %s\n", loadpath);
        load_key_file(st, loadpath);
        selftest_finish(st);
        return;
    }

    if (!which) {
        fprintf(stderr, "puttygen: self-test: neither PUTTYGEN_SELFTEST nor "
                "PUTTYGEN_SELFTEST_LOAD is set\n");
        exit(2);
    }

    if (!strcmp(which, "ed25519")) {
        idx = 0;
    } else if (!strcmp(which, "ed448")) {
        idx = 1;
    } else if (!strcmp(which, "ecdsa")) {
        idx = 2;
    } else if (!strcmp(which, "rsa")) {
        idx = 3;
    } else if (!strcmp(which, "dsa")) {
        idx = 4;
    } else {
        fprintf(stderr, "puttygen: unknown PUTTYGEN_SELFTEST value '%s'\n",
                which);
        exit(2);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(st->keytype_combo), idx);
    if (bits > 0 && (st->ktype == KT_RSA || st->ktype == KT_DSA))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->bits_spin), bits);
    if (bits > 0 && (st->ktype == KT_ECDSA || st->ktype == KT_EDDSA)) {
        const int *lens = (st->ktype == KT_ECDSA ? ec_nist_curve_lengths :
                           ec_ed_curve_lengths);
        size_t n = (st->ktype == KT_ECDSA ? n_ec_nist_curve_lengths :
                    n_ec_ed_curve_lengths);
        for (size_t i = 0; i < n; i++)
            if (lens[i] == bits)
                gtk_combo_box_set_active(GTK_COMBO_BOX(st->curve_combo), i);
    }

    fprintf(stderr, "puttygen: self-test: generating %s\n", which);
    start_generating_key(st);

    {
        const char *cancel_ms = getenv("PUTTYGEN_SELFTEST_CANCEL");
        if (cancel_ms)
            g_timeout_add(atoi(cancel_ms), selftest_cancel_cb, st);
    }
}

/*
 * The buttons are "clicked" signals while the menu items are GActions; these
 * little adaptors keep both routes pointing at the same code. The save
 * helpers are defined further down, hence the forward declarations.
 */
static void save_private_key(struct puttygen_state *st, enum save_kind kind);
static void save_public_key(struct puttygen_state *st);
static void load_key_file(struct puttygen_state *st, const char *path);

static void start_generating_key_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    start_generating_key((struct puttygen_state *)vst);
}

static void cancel_generation_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    cancel_generation((struct puttygen_state *)vst);
}

static void save_btn_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    save_private_key((struct puttygen_state *)vst, SAVE_PRIVATE);
}

static void savepub_btn_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    save_public_key((struct puttygen_state *)vst);
}

static void export_openssh_btn_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    save_private_key((struct puttygen_state *)vst, SAVE_OPENSSH_AUTO);
}

static void export_sshcom_btn_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    save_private_key((struct puttygen_state *)vst, SAVE_SSHCOM);
}

static void params_btn_clicked(GtkButton *b, gpointer vst)
{
    (void)b;
    show_params_dialog((struct puttygen_state *)vst);
}

static void open_btn_clicked(GtkButton *b, gpointer vst)
{
    struct puttygen_state *st = vst;
    char *path = our_file_chooser_run(GTK_WINDOW(st->window),
                                      "Load private key:", false, false, NULL);
    (void)b;
    if (path) {
        load_key_file(st, path);
        g_free(path);
    }
}

/* ----------------------------------------------------------------------
 * Loading a key.
 */

static void set_loaded_key(struct puttygen_state *st, ssh2_userkey *key,
                           bool have_private, keytype kt)
{
    release_current_key(st);
    sfree(st->ssh2key.comment);

    /* Keygen paths free their own storage when cancelled; a loaded key is
     * owned by us from here on, so key->key and key->comment are moved. */
    st->ssh2key.key = key->key;
    st->ssh2key.comment = key->comment ? key->comment : dupstr("");
    st->key_in_union = false;          /* a heap key: free it when replaced */
    key->key = NULL;
    key->comment = NULL;

    st->have_private = have_private;
    st->key_exists = true;
    st->ktype = kt;

    gtk_entry_set_text(GTK_ENTRY(st->comment_entry), st->ssh2key.comment);
    gtk_entry_set_text(GTK_ENTRY(st->pass1_entry), "");
    gtk_entry_set_text(GTK_ENTRY(st->pass2_entry), "");
}

/*
 * Decide which of our key types a loaded ssh_key is, so the key-type control
 * can be left showing something sensible (it is disabled/greyed only while
 * generating, but a loaded key does not have to match the picker).
 */
static keytype keytype_of_key(ssh_key *key)
{
    const char *name = ssh_key_alg(ssh_key_base_key(key))->ssh_id;
    if (!strncmp(name, "ssh-rsa", 7) || !strncmp(name, "rsa-sha", 7))
        return KT_RSA;
    if (!strncmp(name, "ssh-dss", 7))
        return KT_DSA;
    if (!strncmp(name, "ecdsa-", 6))
        return KT_ECDSA;
    return KT_EDDSA;
}

static void load_key_file(struct puttygen_state *st, const char *path)
{
    Filename *fn = filename_from_str(path);
    int type, realtype;
    bool needs_pass = false;
    const char *errmsg = NULL;
    char *comment = NULL;
    char *passphrase = NULL;
    ssh2_userkey *newkey = NULL;
    int ret = 0;
    bool have_private = true;
    strbuf *pubbuf = NULL;
    char *algname = NULL;

    type = realtype = key_type(fn);
    pg_debug("load %s: key_type=%s", path, key_type_to_str(type));

    if (type == SSH_KEYTYPE_SSH1 || type == SSH_KEYTYPE_SSH1_PUBLIC) {
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "This GUI does not support SSH-1 keys. Use the "
                    "puttygen command line to convert an SSH-1 key.");
        filename_free(fn);
        return;
    }

    /*
     * import_possible() answers only "is this a foreign format the import
     * machinery can convert?" - it is false for PuTTY's own .ppk and for
     * public-key-only files, which we load natively.
     */
    bool natively_loadable =
        type == SSH_KEYTYPE_SSH2 ||
        type == SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 ||
        type == SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH;

    if (type == SSH_KEYTYPE_UNOPENABLE || type == SSH_KEYTYPE_UNKNOWN ||
        (!natively_loadable && !import_possible(type))) {
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "Couldn't load this key file (%s)", key_type_to_str(type));
        filename_free(fn);
        return;
    }

    realtype = type;                    /* remember the file's own format */
    /*
     * Every foreign format imports into an SSH-2 key (there are no known
     * foreign SSH-1 formats), which is the only target this GUI can hold, so
     * import_target_type() would always give SSH_KEYTYPE_SSH2 here.
     */

    switch (realtype) {
      case SSH_KEYTYPE_SSH2:
        needs_pass = ppk_encrypted_f(fn, &comment);
        break;
      case SSH_KEYTYPE_SSH2_PUBLIC_RFC4716:
      case SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH:
        needs_pass = false;
        break;
      default:
        needs_pass = import_encrypted(fn, realtype, &comment);
        break;
    }

    for (;;) {
        burnstr(passphrase);
        passphrase = NULL;

        if (needs_pass) {
            passphrase = ask_passphrase(st, comment,
                                        "Enter passphrase to load key:");
            if (!passphrase)
                goto cleanup;          /* user cancelled */
        } else {
            passphrase = dupstr("");
        }

        if (realtype == SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 ||
            realtype == SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH) {
            /* Public-only: build a public key object, no private half. */
            pubbuf = strbuf_new();
            if (ppk_loadpub_f(fn, &algname, BinarySink_UPCAST(pubbuf),
                              &comment, &errmsg)) {
                const ssh_keyalg *alg = find_pubkey_alg(algname);
                if (!alg) {
                    ret = 0;
                    errmsg = "unsupported public key algorithm";
                } else {
                    newkey = snew(ssh2_userkey);
                    newkey->key = ssh_key_new_pub(
                        alg, ptrlen_from_strbuf(pubbuf));
                    newkey->comment = dupstr(comment ? comment : "");
                    ret = newkey->key ? 1 : 0;
                    have_private = false;
                }
            } else {
                ret = 0;
            }
            break;
        }

        if (realtype == SSH_KEYTYPE_SSH2) {
            newkey = ppk_load_f(fn, passphrase, &errmsg);
            if (newkey == SSH2_WRONG_PASSPHRASE) {
                ret = -1;
            } else if (!newkey) {
                ret = 0;
            } else {
                ret = 1;
            }
        } else {
            newkey = import_ssh2(fn, realtype, passphrase, &errmsg);
            if (newkey == SSH2_WRONG_PASSPHRASE) {
                ret = -1;
            } else if (!newkey) {
                ret = 0;
            } else {
                ret = 1;
            }
        }

        if (ret != -1)
            break;

        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "Wrong passphrase.");
    }

    if (ret != 1) {
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "Couldn't load private key (%s)",
                    errmsg ? errmsg : "unknown error");
        goto cleanup;
    }

    set_loaded_key(st, newkey, have_private, keytype_of_key(newkey->key));

    /*
     * If we imported a foreign format, tell the user they must re-save it in
     * PuTTY's own format to use it. (The self-test cannot answer a modal
     * dialog, so it just notes the import on stderr.)
     */
    if (realtype != SSH_KEYTYPE_SSH2 &&
        realtype != SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 &&
        realtype != SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH) {
        if (selftest_active())
            fprintf(stderr, "puttygen: self-test: imported foreign key (%s)\n",
                    key_type_to_str(realtype));
        else
            gui_message(st, GTK_MESSAGE_INFO, "PuTTYgen Note",
                        "Successfully imported key (%s).\n\n"
                        "To use this key with PuTTY you need to use \"Save "
                        "private key\" to save it in PuTTY's own format.",
                        key_type_to_str(realtype));
    }

    refresh_key_display(st);
    set_state(st, 2);

  cleanup:
    burnstr(passphrase);
    sfree(comment);
    sfree(algname);
    if (pubbuf)
        strbuf_free(pubbuf);
    filename_free(fn);
}

/* ----------------------------------------------------------------------
 * Saving.
 */

/*
 * Fetch and validate the two passphrase boxes. Returns a newly-allocated
 * passphrase (possibly ""), or NULL if the user declined (bad match, or
 * refusing the "save without a passphrase" warning).
 */
static char *get_save_passphrase(struct puttygen_state *st)
{
    const char *p1 = gtk_entry_get_text(GTK_ENTRY(st->pass1_entry));
    const char *p2 = gtk_entry_get_text(GTK_ENTRY(st->pass2_entry));
    char *passphrase;

    if (strcmp(p1, p2)) {
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "The two passphrases given do not match.");
        return NULL;
    }

    if (!*p1) {
        if (!gui_confirm(st, "PuTTYgen Warning",
                         "Are you sure you want to save this key without a "
                         "passphrase to protect it?"))
            return NULL;
    }

    passphrase = dupstr(p1);
    return passphrase;
}

/*
 * Choose where to write something, through the one file chooser. Returns a
 * newly-allocated path (free with g_free) or NULL.
 */
static char *ask_save_path(struct puttygen_state *st, const char *title,
                           const char *suggest)
{
    return our_file_chooser_run(GTK_WINDOW(st->window), title,
                                false, true, suggest);
}

static void save_private_key(struct puttygen_state *st, enum save_kind kind)
{
    char *passphrase, *path, *suggest;
    Filename *fn;
    bool ret = false;
    char *err = NULL;
    int type;

    if (!st->key_exists || !st->have_private) {
        pg_debug("save ignored: no private key");
        return;
    }

    passphrase = get_save_passphrase(st);
    if (!passphrase)
        return;

    switch (kind) {
      case SAVE_OPENSSH_AUTO: type = SSH_KEYTYPE_OPENSSH_AUTO; break;
      case SAVE_OPENSSH_NEW:  type = SSH_KEYTYPE_OPENSSH_NEW;  break;
      case SAVE_SSHCOM:       type = SSH_KEYTYPE_SSHCOM;       break;
      default:                type = SSH_KEYTYPE_SSH2;         break;
    }

    suggest = dupcat(st->ssh2key.comment && *st->ssh2key.comment ?
                     st->ssh2key.comment : "private",
                     kind == SAVE_PRIVATE ? ".ppk" :
                     kind == SAVE_SSHCOM ? ".sshcom" : ".openssh", NULL);
    path = ask_save_path(st,
                         kind == SAVE_PRIVATE ? "Save private key as:" :
                         kind == SAVE_SSHCOM ? "Export ssh.com key as:" :
                         "Export OpenSSH key as:",
                         suggest);
    sfree(suggest);
    if (!path) {
        burnstr(passphrase);
        return;
    }

    fn = filename_from_str(path);

    /*
     * Writing a key file needs the random number generator: .ppk v3 salts its
     * passphrase hash, and the foreign formats need an IV or padding. No
     * random_ref() is needed here - puttygen_window_new() took the process's
     * one reference at startup, so the PRNG exists whether the key was
     * generated or loaded.
     */

    if (kind == SAVE_PRIVATE) {
        err = ppk_params_bad(&st->save_params, *passphrase != '\0',
                             strlen(passphrase));
        if (err) {
            gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                        "PPK parameters invalid: %s", err);
            sfree(err);
            sfree(path);
            filename_free(fn);
            burnstr(passphrase);
            return;
        }
        ret = ppk_save_f(fn, &st->ssh2key, *passphrase ? passphrase : NULL,
                         &st->save_params);
    } else {
        ret = export_ssh2(fn, type, &st->ssh2key,
                          *passphrase ? passphrase : NULL);
    }

    if (!ret)
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "Unable to save key file.");
    else
        pg_debug("saved private key to %s", path);

    filename_free(fn);
    burnstr(passphrase);
    g_free(path);
}

static void save_public_key(struct puttygen_state *st)
{
    char *path, *suggest;
    Filename *fn;
    FILE *fp;

    if (!st->key_exists)
        return;

    suggest = dupcat(st->ssh2key.comment && *st->ssh2key.comment ?
                     st->ssh2key.comment : "public", ".pub", NULL);
    path = ask_save_path(st, "Save public key as:", suggest);
    sfree(suggest);
    if (!path)
        return;

    fn = filename_from_str(path);
    fp = f_open(fn, "w", false);
    if (!fp) {
        gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                    "Unable to open key file for writing.");
    } else {
        strbuf *blob = strbuf_new();
        ssh_key_public_blob(st->ssh2key.key, BinarySink_UPCAST(blob));
        ssh2_write_pubkey(fp, st->ssh2key.comment, blob->u, blob->len,
                          SSH_KEYTYPE_SSH2_PUBLIC_RFC4716);
        strbuf_free(blob);
        if (fclose(fp) < 0)
            gui_message(st, GTK_MESSAGE_ERROR, "PuTTYgen Error",
                        "Unable to save key file.");
        else
            pg_debug("saved public key to %s", path);
    }

    filename_free(fn);
    g_free(path);
}

/* ----------------------------------------------------------------------
 * About and licence.
 *
 * Both are plain GtkWindows built through our_dialog_new(), the same way the
 * main PuTTY's About box is built (unix/dialog.c). That matters on macOS: a
 * real GtkDialog gets an RGBA visual and therefore a see-through title bar,
 * and a GtkWindow does not - see gtk_dialog_fix_macos_titlebar() in
 * gtkcompat.h.
 */

static GtkWidget *pg_aboutbox;
static GtkWidget *pg_licencebox;

static void about_window_destroyed(GtkWidget *w, gpointer data)
{
    pg_aboutbox = NULL;
}

static void about_close_clicked(GtkButton *button, gpointer data)
{
    gtk_widget_destroy(pg_aboutbox);
}

static void about_key_press(GtkWidget *widget, GdkEventKey *event,
                            gpointer data)
{
    if (event->keyval == GDK_KEY_Escape && pg_aboutbox) {
        gtk_widget_destroy(pg_aboutbox);
        pg_aboutbox = NULL;
    }
}

static void show_licence(struct puttygen_state *st);

static void about_licence_clicked(GtkButton *button, gpointer data)
{
    show_licence(NULL);
}

static void show_about(struct puttygen_state *st)
{
    GtkWidget *w;
    GtkBox *action_area;
    char *buildinfo_text, *label_text;

    if (pg_aboutbox) {
        gtk_widget_grab_focus(pg_aboutbox);
        return;
    }

    pg_aboutbox = our_dialog_new();
    gtk_container_set_border_width(GTK_CONTAINER(pg_aboutbox), 10);
    gtk_window_set_title(GTK_WINDOW(pg_aboutbox), "About PuTTYgen");
    g_signal_connect(G_OBJECT(pg_aboutbox), "destroy",
                     G_CALLBACK(about_window_destroyed), NULL);

    w = gtk_button_new_with_label("Close");
    gtk_widget_set_can_default(w, true);
    gtk_window_set_default(GTK_WINDOW(pg_aboutbox), w);
    action_area = our_dialog_make_action_hbox(GTK_WINDOW(pg_aboutbox));
    gtk_box_pack_end(action_area, w, false, false, 0);
    g_signal_connect(G_OBJECT(w), "clicked",
                     G_CALLBACK(about_close_clicked), NULL);
    gtk_widget_show(w);

    w = gtk_button_new_with_label("View Licence");
    gtk_widget_set_can_default(w, true);
    gtk_box_pack_end(action_area, w, false, false, 0);
    g_signal_connect(G_OBJECT(w), "clicked",
                     G_CALLBACK(about_licence_clicked), NULL);
    gtk_widget_show(w);

    buildinfo_text = buildinfo("\n");
    label_text = dupprintf("%s\n\n%s\n\n%s\n\n%s\n%s",
                           "PuTTYgen", ver, buildinfo_text,
                           "Original author: Copyright "
                           SHORT_COPYRIGHT_DETAILS ". All rights reserved",
                           "This fork: Copyright "
                           PUTTY_BETER_COPYRIGHT_HOLDER
                           ". All rights reserved");
    w = gtk_label_new(label_text);
    gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(w), true);
    sfree(label_text);
    sfree(buildinfo_text);
    gtk_widget_show(w);
    our_dialog_add_to_content_area(GTK_WINDOW(pg_aboutbox), w, true, true, 0);
    /*
     * A selectable label takes initial focus and selects its whole text, which
     * on macOS draws the box as a solid blue block. PuTTY's own About box
     * clears the selection the same way (unix/dialog.c).
     */
    gtk_widget_grab_focus(w);
    gtk_label_select_region(GTK_LABEL(w), 0, 0);

    /*
     * The fork's home, as a real link. The label above is plain text (GTK
     * markup is off), and nothing else in this box names the fork at all.
     * See PUTTY_BETER_HOMEPAGE in gtkmisc.h.
     */
    w = gtk_link_button_new_with_label(PUTTY_BETER_HOMEPAGE,
                                       PUTTY_BETER_HOMEPAGE_TEXT);
    gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
    gtk_widget_show(w);
    our_dialog_add_to_content_area(GTK_WINDOW(pg_aboutbox), w, false, false, 0);

    g_signal_connect(G_OBJECT(pg_aboutbox), "key-press-event",
                     G_CALLBACK(about_key_press), NULL);
    gtk_widget_show(pg_aboutbox);
    gtk_window_present(GTK_WINDOW(pg_aboutbox));
}

static void licence_window_destroyed(GtkWidget *w, gpointer data)
{
    pg_licencebox = NULL;
}

static void licence_close_clicked(GtkButton *button, gpointer data)
{
    gtk_widget_destroy(pg_licencebox);
}

static void show_licence(struct puttygen_state *st)
{
    GtkWidget *sw, *view, *w;
    GtkTextBuffer *buf;
    GtkBox *action_area;

    if (pg_licencebox) {
        gtk_widget_grab_focus(pg_licencebox);
        return;
    }

    pg_licencebox = our_dialog_new();
    gtk_container_set_border_width(GTK_CONTAINER(pg_licencebox), 10);
    gtk_window_set_title(GTK_WINDOW(pg_licencebox), "PuTTY Licence");
    gtk_window_set_default_size(GTK_WINDOW(pg_licencebox), 640, 480);
    g_signal_connect(G_OBJECT(pg_licencebox), "destroy",
                     G_CALLBACK(licence_window_destroyed), NULL);

    sw = gtk_scrolled_window_new(NULL, NULL);
    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(sw), view);
    our_dialog_add_to_content_area(GTK_WINDOW(pg_licencebox), sw, true, true, 0);
    gtk_widget_show(sw);
    gtk_widget_show(view);

    buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, LICENCE_TEXT("\n\n"), -1);

    w = gtk_button_new_with_label("Close");
    gtk_widget_set_can_default(w, true);
    gtk_window_set_default(GTK_WINDOW(pg_licencebox), w);
    action_area = our_dialog_make_action_hbox(GTK_WINDOW(pg_licencebox));
    gtk_box_pack_end(action_area, w, false, false, 0);
    g_signal_connect(G_OBJECT(w), "clicked",
                     G_CALLBACK(licence_close_clicked), NULL);
    gtk_widget_show(w);

    gtk_widget_show(pg_licencebox);
    gtk_window_present(GTK_WINDOW(pg_licencebox));
}

/* ----------------------------------------------------------------------
 * Menu / button actions.
 */

static void act_generate(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    start_generating_key((struct puttygen_state *)user_data);
}

static void act_cancel(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    cancel_generation((struct puttygen_state *)user_data);
}

static void act_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    char *path = our_file_chooser_run(GTK_WINDOW(st->window),
                                      "Load private key:", false, false, NULL);
    if (path) {
        load_key_file(st, path);
        g_free(path);
    }
}

static void act_save(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    save_private_key((struct puttygen_state *)user_data, SAVE_PRIVATE);
}

static void act_savepub(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    save_public_key((struct puttygen_state *)user_data);
}

static void act_export_openssh(GSimpleAction *a, GVariant *p, gpointer u)
{
    save_private_key((struct puttygen_state *)u, SAVE_OPENSSH_AUTO);
}

static void act_export_sshcom(GSimpleAction *a, GVariant *p, gpointer u)
{
    save_private_key((struct puttygen_state *)u, SAVE_SSHCOM);
}

static void act_params(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    show_params_dialog((struct puttygen_state *)user_data);
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    show_about((struct puttygen_state *)user_data);
}

static void act_licence(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    show_licence((struct puttygen_state *)user_data);
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    gtk_widget_destroy(st->window);
}

/*
 * Copy and Paste act on whatever widget has the focus, so that the Edit menu
 * works for both the comment/passphrase entries and the key box.
 */
static void act_copy(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));

    if (focus && GTK_IS_EDITABLE(focus))
        gtk_editable_copy_clipboard(GTK_EDITABLE(focus));
    else if (focus && GTK_IS_TEXT_VIEW(focus))
        gtk_text_buffer_copy_clipboard(
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus)),
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
}

static void act_paste(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(st->window));

    if (focus && GTK_IS_EDITABLE(focus))
        gtk_editable_paste_clipboard(GTK_EDITABLE(focus));
    else if (focus && GTK_IS_TEXT_VIEW(focus))
        gtk_text_buffer_paste_clipboard(
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus)),
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), NULL, FALSE);
}

static void comment_changed(GtkEditable *e, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    if (!st->key_exists)
        return;
    sfree(st->ssh2key.comment);
    st->ssh2key.comment = dupstr(gtk_entry_get_text(GTK_ENTRY(e)));
    /* The public key line carries the comment, so keep it in step. */
    show_pubkey(st);
}

static void fptype_changed(GtkComboBox *combo, gpointer user_data)
{
    struct puttygen_state *st = user_data;
    st->fptype = (gtk_combo_box_get_active(combo) == 1 ?
                  SSH_FPTYPE_MD5 : SSH_FPTYPE_SHA256);
    pg_debug("fptype -> %d", (int)st->fptype);
    show_fingerprint(st);
}

static const GActionEntry win_action_entries[] = {
    { "generate", act_generate },
    { "cancel", act_cancel },
    { "openkey", act_open },
    { "savepriv", act_save },
    { "savepub", act_savepub },
    { "exportopenssh", act_export_openssh },
    { "exportsshcom", act_export_sshcom },
    { "params", act_params },
    { "copy", act_copy },
    { "paste", act_paste },
    { "about", act_about },
    { "licence", act_licence },
    { "quit", act_quit },
};

/* ----------------------------------------------------------------------
 * Building the window.
 */

static GtkWidget *labelled_control(GtkGrid *grid, int row, const char *label,
                                   GtkWidget *control)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(grid, l, 0, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(grid, control, 1, row, 1, 1);
    return l;
}

static GtkWidget *make_key_display(GtkTextBuffer **buf_out)
{
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *view = gtk_text_view_new();
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);

    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_widget_set_size_request(sw, -1, 110);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_IN);

    *buf_out = buf;
    return sw;
}

/*
 * Build the contents of the main window. Exported to main-gtk-puttygen.c,
 * which owns the GtkApplication and its menu bar.
 */
GtkWidget *puttygen_window_new(GtkApplication *app)
{
    struct puttygen_state *st = snew(struct puttygen_state);
    GtkWidget *win, *vbox, *grid, *frame, *row, *sw, *l;
    GtkWidget *view;
    int r;

    memset(st, 0, sizeof(*st));
    g_mutex_init(&st->mutex);
    st->save_params = ppk_save_default_parameters;
    st->fptype = SSH_FPTYPE_DEFAULT;
    st->ktype = KT_EDDSA;
    st->primepolicy = &primegen_probabilistic;
    st->key_bits = 2048;
    st->curve_bits = 255;

    /*
     * Create the process's random number generator once, here, and hold the one
     * reference for the life of the program - the same shape as
     * windows/puttygen.c, which calls random_setup_special() in WinMain.
     *
     * This must happen exactly once: random_setup_special() ends up in
     * random_create(), which asserts that no PRNG exists yet. Creating it per
     * key generation aborted the process the second time a key was generated.
     * Key generation only re-seeds it (random_reseed(), see
     * start_generating_key), and saving a key just draws from it.
     */
    random_setup_special();

    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "PuTTYgen");
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 680);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    st->window = win;

    /* Register the window actions, keeping the ones we grey out. */
    g_action_map_add_action_entries(G_ACTION_MAP(win), win_action_entries,
                                    G_N_ELEMENTS(win_action_entries), st);
    st->act_open = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "openkey"));
    st->act_save = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "savepriv"));
    st->act_savepub = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "savepub"));
    st->act_export_openssh = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "exportopenssh"));
    st->act_export_sshcom = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "exportsshcom"));
    st->act_params = G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(win), "params"));

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* ---- Key generation ------------------------------------------- */
    frame = gtk_frame_new("Key");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    r = 0;
    st->keytype_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->keytype_combo),
                                   "Ed25519");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->keytype_combo),
                                   "Ed448");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->keytype_combo),
                                   "ECDSA (NIST)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->keytype_combo),
                                   "RSA");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->keytype_combo),
                                   "DSA");
    labelled_control(GTK_GRID(grid), r++, "Key type:", st->keytype_combo);

    st->bits_spin = gtk_spin_button_new_with_range(512, 16384, 64);
    st->bits_label = labelled_control(GTK_GRID(grid), r++, "Number of bits:",
                                      st->bits_spin);

    st->curve_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->curve_combo),
                                   "Ed25519 (255 bits)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->curve_combo),
                                   "Ed448 (448 bits)");
    st->curve_label = labelled_control(GTK_GRID(grid), r++, "Curve:",
                                       st->curve_combo);

    st->policy_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->policy_combo),
                                   "Probabilistic (fast)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->policy_combo),
                                   "Proven primes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->policy_combo),
                                   "Proven primes (even distribution)");
    st->policy_label = labelled_control(GTK_GRID(grid), r++,
                                        "Prime generation:",
                                        st->policy_combo);

    st->strong_check = gtk_check_button_new_with_label(
        "Use strong RSA primes (slow)");
    gtk_grid_attach(GTK_GRID(grid), st->strong_check, 1, r++, 1, 1);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_grid_attach(GTK_GRID(grid), row, 0, r++, 2, 1);
    /*
     * "Generate" and "Cancel", not "_Generate"/"_Cancel": no underscore,
     * because gtk_button_new_with_label() takes the text literally (only
     * gtk_button_new_with_mnemonic() parses it), so an underscore would be
     * drawn on the button.
     */
    st->generate_btn = gtk_button_new_with_label("Generate");
    gtk_box_pack_start(GTK_BOX(row), st->generate_btn, FALSE, FALSE, 0);
    st->cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_box_pack_start(GTK_BOX(row), st->cancel_btn, FALSE, FALSE, 0);
    st->progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(row), st->progress, TRUE, TRUE, 0);

    st->status_label = gtk_label_new("");
    gtk_widget_set_halign(st->status_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), st->status_label, 0, r++, 2, 1);

    g_signal_connect(st->generate_btn, "clicked",
                     G_CALLBACK(start_generating_key_clicked), st);
    g_signal_connect(st->cancel_btn, "clicked",
                     G_CALLBACK(cancel_generation_clicked), st);
    g_signal_connect(st->keytype_combo, "changed",
                     G_CALLBACK(keytype_changed), st);

    /* ---- Key details --------------------------------------------- */
    frame = gtk_frame_new("Key details");
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    st->details_frame = frame;

    r = 0;
    st->comment_entry = gtk_entry_new();
    labelled_control(GTK_GRID(grid), r++, "Key comment:",
                     st->comment_entry);

    st->pass1_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(st->pass1_entry), FALSE);
    labelled_control(GTK_GRID(grid), r++, "Key passphrase:",
                     st->pass1_entry);

    st->pass2_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(st->pass2_entry), FALSE);
    labelled_control(GTK_GRID(grid), r++, "Confirm passphrase:",
                     st->pass2_entry);

    st->fptype_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->fptype_combo),
                                   "SHA-256");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st->fptype_combo),
                                   "MD5");
    gtk_combo_box_set_active(GTK_COMBO_BOX(st->fptype_combo), 0);
    labelled_control(GTK_GRID(grid), r++, "Fingerprint type:",
                     st->fptype_combo);

    st->fingerprint_label = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(st->fingerprint_label), TRUE);
    gtk_widget_set_halign(st->fingerprint_label, GTK_ALIGN_START);
    l = gtk_label_new("Fingerprint:");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), st->fingerprint_label, 1, r++, 1, 1);

    l = gtk_label_new("Public key for pasting into an OpenSSH "
                      "authorized_keys file:");
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r++, 2, 1);

    sw = make_key_display(&st->keydisplay_buf);
    gtk_grid_attach(GTK_GRID(grid), sw, 0, r++, 2, 1);

    view = gtk_bin_get_child(GTK_BIN(sw));   /* the GtkTextView */

    g_signal_connect(st->comment_entry, "changed",
                     G_CALLBACK(comment_changed), st);
    g_signal_connect(st->fptype_combo, "changed",
                     G_CALLBACK(fptype_changed), st);

    /* ---- Actions -------------------------------------------------- */
    frame = gtk_frame_new("Actions");
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row), 8);
    gtk_container_add(GTK_CONTAINER(frame), row);

    st->open_btn = gtk_button_new_with_label("Open key\u2026");
    st->save_btn = gtk_button_new_with_label("Save private key");
    st->savepub_btn = gtk_button_new_with_label("Save public key");
    st->export_openssh_btn = gtk_button_new_with_label("Export OpenSSH key");
    st->export_sshcom_btn = gtk_button_new_with_label("Export ssh.com key");
    st->params_btn = gtk_button_new_with_label("Parameters\u2026");
    {
        GtkWidget *btns[] = { st->open_btn, st->save_btn, st->savepub_btn,
                              st->export_openssh_btn, st->export_sshcom_btn,
                              st->params_btn };
        for (size_t i = 0; i < G_N_ELEMENTS(btns); i++)
            gtk_box_pack_start(GTK_BOX(row), btns[i], FALSE, FALSE, 0);
    }

    g_signal_connect(st->open_btn, "clicked",
                     G_CALLBACK(open_btn_clicked), st);
    g_signal_connect(st->save_btn, "clicked",
                     G_CALLBACK(save_btn_clicked), st);
    g_signal_connect(st->savepub_btn, "clicked",
                     G_CALLBACK(savepub_btn_clicked), st);
    g_signal_connect(st->export_openssh_btn, "clicked",
                     G_CALLBACK(export_openssh_btn_clicked), st);
    g_signal_connect(st->export_sshcom_btn, "clicked",
                     G_CALLBACK(export_sshcom_btn_clicked), st);
    g_signal_connect(st->params_btn, "clicked",
                     G_CALLBACK(params_btn_clicked), st);

    /* ---- Initial state ------------------------------------------- */
    gtk_combo_box_set_active(GTK_COMBO_BOX(st->keytype_combo), 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(st->policy_combo), 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(st->fptype_combo), 0);

    /* Ed25519 / 255 bits is the default; keep state and picker in step. */
    st->ktype = combo_to_keytype(
        gtk_combo_box_get_active(GTK_COMBO_BOX(st->keytype_combo)),
        &st->curve_bits);

    gtk_widget_show_all(win);
    /*
     * show_all() makes every child visible, including the controls that belong
     * to other key types and the "generating" widgets, so the state has to be
     * applied after it, not before.
     */
    update_key_type_ctrls(st);
    set_state(st, 0);

    g_object_set_data_full(G_OBJECT(win), "puttygen-state", st,
                           g_free);
    (void)view;

    pg_debug("window built");

    if (selftest_active())
        selftest_start(st);

    return win;
}
