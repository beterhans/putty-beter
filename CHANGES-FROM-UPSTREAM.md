# What this fork changes, compared with PuTTY 0.85

Baseline: the official PuTTY 0.85 source tarball,
<https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz>.

Everything listed here was written for this fork and is MIT-licensed under the
same terms as the rest of the tree (`LICENCE`). **No new external
dependencies**, and no GPL/LGPL code has been added: the one GPL-2.0 tool this
project uses, `gtk-mac-bundler`, is a build-time-only helper and is
deliberately not vendored (`tools/README.md`).

Everything not mentioned below is unmodified upstream PuTTY.

---

## Reproducing this diff

```sh
curl -O https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz
tar xzf putty-0.85.tar.gz

# which upstream files differ, and what has been added
diff -rq putty-0.85 . -x '.git' -x 'build-*' -x '*.old' -x '_agent' -x 'issue' -x 'tools'

# a specific file
diff -u putty-0.85/unix/window.c unix/window.c
```

The first commit in this repository (`16c7c5b`) already contains the source
fixes, so `git log` alone shows only the later work.

---

## Summary

| Area | Change |
|---|---|
| macOS code paths | The CMake build never defined `OSX_GTK`, so every macOS-specific code path was silently compiled out. Now enabled, which is what makes clipboard support work at all. |
| Clipboard | Copy/paste in the terminal and in dialogs, with Command and with Ctrl+Ins/Shift+Ins, on any keyboard. |
| Keyboard input | IME pre-edit cursor offset fixed (upstream had an out-of-bounds read here). |
| Configuration folder | The configuration directory can be moved, with a one-time dialog and a redirect file. |
| Configuration box | Resizable, size remembered in the configuration folder, and the saved-session list grows with the window. |
| Session folders | Sessions can be kept in folders, shown as a tree and moved by dragging. |
| Defaults | Logging, function keys, Backspace, window size, keepalive and IP version defaults changed. |
| Build tooling | `scripts/build-macos.sh`, `scripts/bundle-macos.sh`, `CMakePresets.json`, a self-contained `PuTTY.app`. |
| Diagnostics | `PUTTY_DIAG=1` and `PUTTY_CONFIG_DEBUG=1` runtime traces. |

---

## Modified upstream files

Line counts are `+added / -removed` against the 0.85 tarball.

| File | +/- | What changed |
|---|---|---|
| `conf.h` | +19 / -7 | Seven default values changed (see below) |
| `config.c` | +294 / -30 | New tunnels in Connection → Tunnels preselect IPv4 instead of Auto; the Session panel reordered so the saved-session list is last and can grow; the Load/Save/Delete buttons keep their natural width |
| `dialog.h` | +62 / -0 | `vexpand` flag on `struct controlset`, and `vexpand` / `force_left` flags on `struct dlgcontrol` (per front end; Windows ignores them) |
| `dialog.c` | +3 / -0 | Initialise those flags in `ctrl_getset()` / `ctrl_new()` |
| `settings.c` | +42 / -4 | `CtrlShiftCV` default comes from `CLIPUI_DEFAULT_CV` instead of a hardcoded `CLIPUI_NONE`, so it can differ per platform; `sessioncmp` sorts folders before sessions at every level |
| `terminal/terminal.c` | +66 / -1 | IME pre-edit cursor fix; `PUTTY_DIAG` tracing |
| `test/test_conf.c` | +7 / -7 | Expected defaults updated to match the seven changed ones |
| `unix/CMakeLists.txt` | +14 / -1 | Build `unix/config-location.c` into `guiterminal`; link `ApplicationServices` on Apple |
| `unix/columns.c` | +54 / -1 | Optional "content-sized last column": a column of natural-width buttons no longer keeps its percentage of the width |
| `unix/columns.h` | +11 / -0 | The flag and the last-column width it needs |
| `unix/dialog.c` | +519 / -14 | `dialog_clipboard_key()`: Command/Ctrl + C/V/X/A and Ctrl+Ins/Shift+Ins in dialog text fields; resizable configuration box whose size is remembered in `gui-state`; `vexpand` / `force_left` support in the layout |
| `unix/main-gtk-application.c` | +8 / -0 | Call `config_location_init()` on application activation |
| `unix/main-gtk-simple.c` | +9 / -0 | Same, before `conf_new()` so the default log file name lands in the right folder |
| `unix/osxlaunch.c` | +17 / -0 | Set `GDK_PIXBUF_MODULE_FILE` and `GTK_IM_MODULE_FILE` to the bundled caches |
| `unix/platform.h` | +51 / -3 | `#ifdef OSX_GTK` → `#if defined(OSX_GTK) \|\| defined(__APPLE__)` in both blocks; macOS clipboard defaults; declarations for the configuration-folder and GUI-state APIs |
| `unix/putty.bundle` | +20 / -0 | Bundle gdk-pixbuf loaders (so `loaders.cache` points inside the app); bundle `icons/PuTTY.icns`; the Adwaita theme block is commented out (GTK ≥ 3.20 has it built in) |
| `unix/storage.c` | +880 / -25 | Configuration-folder redirection, the `config_location_*` API, and the `gui-state` key=value store |
| `unix/window.c` | +196 / -8 | Default log file name; Finder drag-and-drop; `dup_mb_to_wc_c` fix; Command-key pass-through; `is_insert_key()` for Ctrl+Ins/Shift+Ins; `PUTTY_DIAG` |
| `windows/platform.h` | +2 / -0 | Keep `CLIPUI_DEFAULT_CV = CLIPUI_NONE` on Windows |
| `windows/controls.c` | +53 / -0 | `dlg_listbox_selected_id()` / `dlg_listbox_select_id()` for the Windows front end (no tree support there, so an id is a row index) |
| `windows/storage.c` | +18 / -0 | Folder operations return "not supported" (a Windows session is a registry key) |
| `storage.h` | +19 / -0 | Declarations for the folder operations and the name checks |
| `CMakeLists.txt` | +7 / -0 | `test_sessions` target |
| `.gitignore` | +14 / -0 | Ignore build trees, `*.old` backups, `_agent/`, `issue/`, `tools/gtk-mac-bundler/`, `.DS_Store` |
| `README` → `README.md` | — | Replaced the generic upstream README with one about this macOS build; renamed so GitHub renders Markdown |

## New files

| File | Purpose |
|---|---|
| `CMakePresets.json` | `arm64`, `x86_64`, `debug` presets |
| `scripts/build-macos.sh` | Per-architecture build, one command |
| `scripts/bundle-macos.sh` | Builds `PuTTY.app` with gtk-mac-bundler, then has to fix up the result: normalise Cellar paths to `opt`, resolve dangling references, regenerate `loaders.cache`, ad-hoc re-sign |
| `unix/config-location.c` | The "where should the configuration live?" dialog |
| `tools/README.md` | Why `gtk-mac-bundler` is not vendored, and how to fetch it |
| `test/test_sessions.c` | Tests for the session-folder storage layer |
| `PLAN.md` | Roadmap and the reasoning behind it |
| `icons/PuTTY.icns`, `icons/putty-*.pam` | Generated icon assets; the `.icns` is what `unix/putty.plist` asks for |

---

## 1. macOS code paths were disabled

`unix/platform.h` had two `#ifdef OSX_GTK` blocks, and the CMake build never
defined `OSX_GTK` (a leftover from the old Makefile-based build). Every
macOS-specific tweak was compiled out and the generic X11 code was used
instead. Both blocks now test `#if defined(OSX_GTK) || defined(__APPLE__)`.

Consequences of the fix:

| Tweak | Was off, so... | Now |
|---|---|---|
| `CLIPUI_DEFAULT_MOUSE`, `CLIPUI_DEFAULT_INS` | Mouse paste and Ctrl/Shift+Ins targeted the X11 `PRIMARY` selection, which does not exist on macOS | Target the system clipboard |
| `OSX_META_KEY_CONFIG` | No Command-key pass-through: the terminal ate `Cmd+C`/`Cmd+V` before the menu accelerators ran | Command is handled as a system modifier and passed to GTK |
| `JUST_USE_GTK_CLIPBOARD_UTF8` | The low-level `gdk_selection_*` code was used, which the source itself says fails on macOS | The `GtkClipboard` path, i.e. the macOS pasteboard |

This also uncovered a latent compile error in `clipboard_text_received` that had
never been built: `dup_mb_to_wc(…)` with four arguments, where four-argument
form is `dup_mb_to_wc_c(…)`.

## 2. Clipboard

**Terminal.** `Cmd+C`/`Cmd+V` via menu accelerators; `Ctrl+Shift+C`/`Ctrl+Shift+V`
(now defaulted to the system clipboard rather than to "do nothing");
`Ctrl+Ins`/`Shift+Ins`; and Ctrl+right-click → Copy/Paste/Copy All.

The `Ctrl+Ins` pair did not work upstream on macOS even with the setting turned
on: `key_event()` tested `event->keyval == GDK_KEY_Insert`, but macOS has no
Insert key — a PC keyboard's Insert arrives as virtual keycode 0x72
(`kVK_Help`, the Mac's "Help" key, which GDK reports as `GDK_KEY_Help`), and a
keypad Insert arrives as `GDK_KEY_KP_Insert`. `is_insert_key()` now accepts any
keyval whose GDK name contains `Insert`, plus `GDK_KEY_Help` and hardware
keycode 0x72 on macOS.

**Dialogs.** Nothing could be pasted into the configuration box's text fields,
because GTK binds entry copy/paste to Control while macOS users press Command
(`GDK_MOD2_MASK`). `dialog_clipboard_key()`, called from `win_key_press()`, now
handles Ctrl or Cmd + `C`/`V`/`X`/`A` and Ctrl+Ins/Shift+Ins on the focused
`GtkEditable` (and copy from selectable message labels). `GDK_MOD2` is only
treated as Command under `__APPLE__`, since on other Unix systems it is Num Lock.

## 3. IME pre-edit cursor offset

`term_set_preedit_text()` allocated the pre-edit line unconditionally, so an
empty pre-edit string (which GTK on macOS supplies on ordinary keystrokes) left
it non-`NULL` with zero columns. `do_paint()` then computed the cursor column as
`preedit_end - 1` — one cell too far left — and read `chars[-1]` out of bounds.
Fixed in `terminal/terminal.c`.

## 4. Relocatable configuration folder

PuTTY has always used `~/.putty`. This build offers, once, to keep the
configuration somewhere else (`~/Documents/putty-beter-config` by default).

* Base location resolution is unchanged: `$PUTTYDIR`, then an **existing**
  `$XDG_CONFIG_HOME/putty` or `~/.config/putty`, then `~/.putty`, then
  `pw_dir/.putty`, then `/.putty`.
* A base location may contain `config-location`, a small text file whose first
  non-comment line is the real configuration directory. `unix/storage.c` reads
  it, so `putty`, `plink`, `pscp`, `psftp` and `puttygen` all follow the same
  directory and saved host keys stay consistent. An invalid or missing target is
  ignored (with a trace under `PUTTY_CONFIG_DEBUG=1`).
* The dialog appears when there is no configuration yet (no `sessions`
  subdirectory), or when Option/Alt is held at launch, or when
  `PUTTY_CONFIG_RELOCATE=1`. On acceptance the existing `sessions`,
  `sshhostkeys`, `sshhostkeys.tmp`, `sshhostcas` and `randomseed` are **moved**;
  `rename()` is used where possible, `EXDEV` falls back to a recursive copy, and
  the source is only removed once the copy succeeded.
* Nothing is overwritten: an item that already exists in the destination is left
  alone and the old copy stays where it was. Cancelling records the current
  folder as configured, so the dialog is genuinely one-shot. Choosing the base
  location again removes the redirect.
* The Option-key test uses `CGEventSourceFlagsState()` (CoreGraphics, linked
  through `guiterminal`), because GDK cannot report a modifier before the first
  key event of the process.

## 5. Changed defaults

`conf.h` unless noted. These apply to **new** sessions; a saved session that
stores a key keeps its own value.

| Setting | Upstream | Here |
|---|---|---|
| Session logging | None | Printable output |
| Log file name | `putty.log` | `<config folder>/logs/&Y-&M-&D_&T-&H.txt` (`unix/window.c`) |
| If the log file exists | Ask | Always append |
| Backspace key | Control-? | Control-H |
| Function keys | `ESC[n~` (Linux) | VT100+ |
| Window size | 80×24 | 128×36 |
| Keepalive interval | 0 (off) | 120 s |
| Internet protocol version | Auto | IPv4 |
| Tunnels → IP version, new tunnel | Auto | IPv4 (`config.c`) |

Two follow-on changes were needed:

* `conf_new()` captures the log file name, so `config_location_init()` runs
  before it in `main-gtk-simple.c`; otherwise the folder would be the
  pre-relocation one.
* PuTTY never creates a missing directory for a log file, so startup creates
  `<config folder>/logs`.

`pterm` keeps `putty.log`: it has no saved sessions and no configuration folder.

## 6. Configuration box layout and size

The Session panel is reordered so the panel ends with the saved-session list
instead of "Close window on exit", and the configuration box is resizable with
its size remembered between runs. With a long list of saved sessions, and a
window that stays the size you made it, the list is the natural thing to grow.

* `config.c`: "Close window on exit" is created before the saved-sessions
  controlset, so it appears above the list. The saved-sessions controlset is
  marked `vexpand`, and its listbox is the control that takes the spare height;
  seven rows remain the minimum.
* `dialog.h` / `dialog.c`: two flags on `dlgcontrol` (`vexpand`,
  `force_left`) and one on `controlset` (`vexpand`). Front ends that do not
  implement them ignore them.
* `unix/dialog.c`: a controlset marked `vexpand` is packed with `expand` in its
  panel, and a control marked `vexpand` becomes the `columns_vexpand` child of
  its `Columns`. `force_left` uses the existing `columns_force_left_align`, so
  a control keeps its natural width — this is what stops the Load/Save/Delete
  buttons turning into full-width bars when the window is wide.
* `unix/columns.c`, `unix/columns.h`: `columns_content_last_col()` sizes the
  last column of each block to its widest left-aligned child instead of its
  percentage. Without it the button column would still reserve 20% of the
  width and leave a gap; with it, the list reaches the buttons and the three
  buttons stay the same width, flush with the right edge, at any window size
  (checked headlessly at 600, 1000 and 2000 px: `list` 525/925/1925 px wide,
  buttons always 71 px).
* Window size: `create_config_box()` is now explicitly resizable, restores a
  remembered size with `gtk_window_set_default_size()`, tracks configure events,
  and writes the final size on `destroy`.
* `unix/storage.c`: `config_state_get_int()` / `config_state_set_int()`, a tiny
  key=value store used as `<configuration folder>/gui-state`:

  ```
  # PuTTY-beter: remembered GUI state.
  width=1200
  height=1000
  ```

  Reading is best-effort; writing rewrites the file through a temporary and a
  `rename`, keeping any other keys. `pterm` is excluded: it has no
  configuration folder, so `saved_sessions` is checked before reading or writing.

## 7. Build and packaging

* `scripts/build-macos.sh <arm64|x86_64>` — one CMake build per architecture.
* `scripts/bundle-macos.sh <arch>` — runs gtk-mac-bundler and then repairs its
  output: Homebrew `Cellar` paths are normalised to `opt`, dangling references
  resolved, `loaders.cache` regenerated (the bundler wrote it before the
  libraries were in place), and everything ad-hoc re-signed because
  `install_name_tool` invalidates signatures.
* `unix/osxlaunch.c` points `GDK_PIXBUF_MODULE_FILE` and `GTK_IM_MODULE_FILE` at
  the caches inside the bundle. Without that, gdk-pixbuf and GTK fall back to
  build-time absolute paths, which loads a **second copy of GLib** next to the
  bundled one — duplicate Objective-C classes and undefined behaviour.
* `unix/putty.bundle` lists the gdk-pixbuf loaders so the generated
  `loaders.cache` points inside the app, and bundles `icons/PuTTY.icns`.
* Intel (x86_64) needs an Intel Homebrew in `/usr/local`; the arm64 Homebrew in
  `/opt/homebrew` cannot produce an x86_64 GTK. **Not yet verified.** Note that
  Apple is retiring Rosetta (macOS 27 is the last release with it).

## 8. Diagnostics

Both are runtime switches and cost nothing when unset.

* `PUTTY_DIAG=1` — key events (keyval, name, modifier state), clipboard
  activity, font metrics, cursor placement.
* `PUTTY_CONFIG_DEBUG=1` — configuration folder resolution and relocation, and
  the resulting default log file name.

---

## 9. Session folders

Sessions can be kept in folders, shown as a collapsible tree in the
configuration box, and moved between folders by dragging.

* A session name may contain `/`, which separates folder names: `Work/beta`
  is the file `<sessions>/Work/beta`, and `Work` is a **real directory**. So
  folders can be empty, and the structure is visible in the Finder.
  (`make_session_filename()` already percent-encoded `/` as `%2F`, so names
  with a slash were representable but meant nothing; the encoder now treats
  `/` as a separator and encodes each component separately.)
* `get_sesslist()` reports a folder as a name with a trailing `/`, which is
  how the front end tells folders from sessions. Enumeration is recursive
  (depth-limited, and symlinks are not followed).
* Names are validated: an empty, `.` or `..` component is refused, so a
  session name cannot escape the sessions directory. `open_settings_w()`
  rejects such a name outright rather than writing something odd.
* `session_mkdir()`, `session_move()` (sessions *and* folders, refusing to
  move a folder inside itself) and `session_rmdir()` (empty folders only)
  live in `unix/storage.c`; `session_move_target_name()` works out the name a
  drag would give, so that the fiddly part is testable without a front end.
* The dialog layer gained a `tree` flag on list boxes, a `drag_handler` hook,
  and `dlg_listbox_selected_id()` / `dlg_listbox_select_id()` /
  `dlg_listbox_selected_ids()`: with a tree, a row index is ambiguous, so the
  configuration box identifies rows by id (the index of the name in the
  session list). The plural one is what makes a drag move the whole
  selection, and what lets Delete remove several rows at once.
* The session list is a multi-select list, so a group of sessions can be
  dragged into a folder together, or deleted together. `dlg_listbox_selected_id()`
  falls back to the row under the cursor when several are selected, so Load
  and Save still act on something sensible.
* Dragging several rows needs one more workaround: GtkTreeView's own press
  handler calls `gtk_tree_view_real_set_cursor()` with `CLEAR_AND_SELECT` for
  an unmodified press, so pressing on one of several selected rows throws the
  rest of the selection away before any drag starts. `button-press-event` is
  `G_SIGNAL_RUN_LAST` and the tree view's gesture runs from the class closure
  in the *bubble* phase, so a plain `g_signal_connect()` handler sees the
  selection intact: `listbox_press_event()` in `unix/dialog.c` snapshots it
  there. The snapshot is put back by `listbox_restore_selection()` at
  **drag-begin** — not only at the drop, since the collapse is otherwise
  visible for the whole gesture — and again in the drop handler, which is what
  the control's `drag_handler` reads. `drag-end` (or the next press) clears it;
  `listbox_release_event()` deliberately does *not*, because the release that
  ends a drag can arrive before the drop does.
  The snapshot is only restored if the press was on one of the selected rows,
  so dragging an unselected row still moves just that row.
* `sessioncmp()` puts **folders before sessions at every level** (then
  compares component by component, so a folder's contents stay together),
  which is what makes the tree show folders at the top of each level.
  "Default Settings" still comes first.
* In tree mode the GTK front end builds the hierarchy from the `/` in the
  names, keeps a folder flag in an extra, undisplayed model column, shows
  folders in bold, expands them as they gain children, and drags rows onto
  folder rows.
* The tree view is the drag *source*, but the drop target is deliberately its
  **scrolled window**, not the tree view. Two reasons, both found by reading
  GTK's `gtktreeview.c`:
  - the tree view's own drag class closures (`gtk_tree_view_drag_motion`,
    `gtk_tree_view_drag_drop`) run first and, because the signal accumulator
    stops emission when a handler returns TRUE, they consume the drag signals
    — so a handler of ours could never see where the drop happened;
  - GTK's model drop inserts the dragged row as a *sibling* of the row it was
    dropped on (the parent of the destination path), so a drop on a top-level
    folder lands outside it.
  With the scrolled window as the target, GTK's model drop never runs, the
  model is untouched, and `listbox_drag_received()` turns the drop
  coordinates into a row with `gtk_tree_view_get_path_at_pos()` — not
  `get_dest_row_at_pos()`, which is drag-specific and offsets the position to
  allow for the dragged row being drawn under the cursor. The row it finds
  decides the destination: a folder row means that folder, any other row
  means the folder that row is in, and empty space below the list means the
  top level. Storage then performs the real move, and the list is rebuilt
  from disk — always, even for a no-op, so the display cannot drift.
* Deleting a folder that still has sessions in it asks first, with a
  "Delete All" / "Cancel" message box (`dlg_confirm()`, a new small core API
  implemented with the GTK message-box machinery). Escape takes the safe
  answer, and no button is the Return default, since the destructive answer
  should not be one keypress away. `session_rmdir_force()` removes the folder
  and its contents, and refuses anything that is not a folder.
* The Windows front end shows a flat list and gets `dlg_listbox_selected_id()`
  / `dlg_listbox_select_id()` implemented in terms of row indices, plus
  folder operations that report "not supported".

**Verified** by `test/test_sessions.c` (a real test target, run with
`./build-arm64/test_sessions`): plain sessions, empty folders, nesting, names
with spaces, moving sessions in and out of folders, moving whole folders
(including out of and back into a parent), refusing to clobber an existing
name, refusing to move a folder inside itself, removing empty and non-empty
folders, `../evil` being refused, and the drag-target name computation.

**Not verified:** the drag gesture itself needs a human; everything either
side of it (the move in storage, the target-name computation, the tree
display) is covered. The same goes for the delete confirmation, which needs a
click on Delete.

**Not implemented yet:** renaming a folder, remembering which folders are
expanded, and per-folder metadata (comments, saved passwords) — see
`PLAN.md` Phase 2.

---

## Worth reporting upstream

1. **`OSX_GTK` is never defined by the CMake build**, so every macOS-specific
   code path in `unix/platform.h` is compiled out on macOS. This disables
   clipboard support and Command-key handling in any CMake-built macOS PuTTY.
2. **IME pre-edit out-of-bounds read and cursor offset**: an empty pre-edit
   string leaves a zero-width pre-edit line, after which `do_paint()` reads
   `chars[-1]` and places the cursor one cell to the left.
3. **Ctrl+Ins/Shift+Ins cannot work on macOS** as written, because the code
   matches `GDK_KEY_Insert` and macOS reports that key as `GDK_KEY_Help`.
4. **Dialogs cannot be pasted into on macOS**: GTK's copy/paste bindings are
   Control-based and nothing handles Command.

## Deliberately not changed

* Still GTK 3, as upstream. Only AppKit would be "universal" without bundling
  every dependency, and that is a rewrite, not a fork.
* No ZMODEM, session folders, saved passwords or MCP server yet — see `PLAN.md`.
* Copy-on-select (`CLIPUI_DEFAULT_AUTOCOPY`) is still off, matching the macOS
  Human Interface Guidelines.
