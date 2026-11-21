# `CONFIG_BOX_CRASH.md` — the startup crash ("putty unable to open") — **FIXED**

**Status: fixed and verified, 2026-11-21.** Root cause was a 1-byte `bool` being
handed to `gtk_tree_model_get()` for a `G_TYPE_BOOLEAN` column, so **GLib wrote 4
bytes into a 1-byte object**, smashing 3 bytes of the adjacent stack slot.

The fix is one type change, at two sites, in `unix/dialog.c`:

```diff
-    bool is_folder = false;
+    gboolean is_folder = false;      /* G_TYPE_BOOLEAN is a 4-byte gboolean */
     gtk_tree_model_get(model, iter, folder_col, &is_folder, -1);
```

* `listbox_folder_weight()` (`dialog.c:801`) — the cell data func GTK calls for
  **every session-list row**, i.e. during the size validation that crashed.
* the drag-and-drop hit test (`dialog.c:1088`).

The pre-fix form is in git history — `git show ec9b70d:unix/dialog.c` (the commit
before the fix). The local safety copy `unix/dialog.c-12.old` was written while
diagnosing, and deleted with the other build leftovers on 2026-11-22; `*-NN.old`
files are git-ignored and never ship.

**Verification** (16 launches, Intel Mac, x86_64):

| | before the fix | after |
|---|---|---|
| plain `build-x86_64/putty`, 8 launches | 3 crashed, 25 GLib criticals | **0 crashed, 0 criticals** |
| bundled `PuTTY.app`, 8 launches | (same bug; 3 of 8 earlier) | **0 crashed, 0 criticals, 0 crash reports** |
| `open PuTTY.app` (Finder), 4 launches | ~1 in 3 died | **4 of 4 running** |

Before the fix the bug **fired on every launch** — it either died at once (0
criticals, the memory gone) or survived with exactly 5 criticals. Only the outcome
was random, which is why a short sample could look like "it opens every time".

**The arm64 build needs the same fix.** The source is shared, so the arm app has
the same bug; rebuild it after this change.

---

## Post-mortem detail (kept for the next time a crash looks like this)

### Symptom

The app died during startup ~1 launch in 3, printing 5
`GLib-GObject-CRITICAL: g_object_thaw_notify: assertion 'G_IS_OBJECT (object)'
failed` when it survived, or nothing when it did not. 10 crash reports for one day,
the oldest hours before any of the day's bundling work — so it was **pre-existing**,
not a packaging problem. The plain, unbundled binary showed the same criticals,
which ruled out bundling, signing, quarantine and the MCP work in one step.

### Stack (identical in every report)

```
 0 g_type_check_instance_is_fundamentally_a
 1 g_object_thaw_notify
 2 apply_cell_attributes            <- GHFunc; key = a GtkCellRenderer
 3 g_hash_table_foreach             <- walks GtkCellAreaBox cell_info
 4 gtk_cell_area_real_apply_attributes
12 validate_row
14 gtk_tree_view_get_preferred_width
25 columns_get_preferred_width     <- unix/columns.c
76 set_window_icon                  <- unix/window.c (gtk_widget_realize)
77 create_config_box
78 initial_config_box
87 main
```

### The two clues that found it

1. **The bogus pointer's shape.** `rdi` at frame 0 (the bad object — the
   `cell_info` key) was always `0x7fXXXXXX000000`: a 4 GB-aligned address with the
   **low three bytes zeroed**. That is the signature of a 4-byte store landing on a
   1-byte variable whose neighbour is a pointer: the three extra bytes zero the
   pointer's low three bytes.
2. **GTK's contract** (`gtk-3-24/gtk/gtkcellarea.c`): `cell_info` is created with
   `g_hash_table_new_full(..., NULL, cell_info_free)` — the renderer key is held
   **without a reference and without a weak-notify**, and only
   `gtk_cell_area_remove()` takes it out. So a renderer that has been overwritten
   or destroyed while registered leaves the area holding a dangling key, and the
   next `validate_row` dereferences it.

### Why it fired on every launch but crashed only sometimes

`listbox_folder_weight()` runs inside GTK's `apply_cell_attributes`, in the same
frame chain that then calls `g_object_thaw_notify(renderer)`. The 3-byte overflow
landed on a neighbouring pointer, so the next use of that pointer was either a
mapped address holding garbage (assertion critical, carry on) or an unmapped one
(SIGSEGV). Same bug, different dice.

### Ruled out, with evidence

| Suspect | Verdict |
|---|---|
| Bundling / signing / `install_name_tool` | No — plain `build-x86_64/putty` showed the same criticals. |
| `com.apple.quarantine` | No — reproduced with zero quarantine attributes. |
| The MCP bridge, or the SMB symlink that broke bundling | No — crash reports predate MCP bundling; the symlink problem was separate and fixed. |
| Stale/mismatched bundled GTK vs `/usr/local` | No — same with either. (Comparing dylib hashes is meaningless: `install_name_tool` rewrote them all.) |
| `get_listitemheight()`'s `ref/ref_sink/unref` (upstream) | Leaks a renderer; cannot produce a dead one. |
| A second thread racing GTK | No — `unix/` starts no threads but puttygen's keygen. |
| `gtk_tree_model_get(..., 0, &ret, -1)` (`dialog.c:1261`) | Fine — `ret` is a correct `int` for a `G_TYPE_INT` column. |

### Lesson worth keeping

PuTTY's `bool` is C99 `_Bool` (1 byte); GLib's `gboolean` is 4. **Any** GLib
varargs out-parameter for a `G_TYPE_BOOLEAN` — `gtk_tree_model_get()`,
`g_object_get()` — needs a `gboolean`, or GLib writes 4 bytes into 1. A quick way
to sweep for it:

```sh
grep -n 'gtk_tree_model_get\|g_object_get' unix/*.c   # then check each &var's type
```
