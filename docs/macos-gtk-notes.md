# macOS and GTK: what they do that you would not expect

Every entry here was measured on a real machine in this tree, with the numbers
that were measured. They are the things that cost time to work out, and the
reason each one is written down is that **the symptom does not point at the
cause**: a window on the wrong monitor, a `-geometry` that is accepted and
ignored, a title bar that goes see-through.

The changes that came out of them are described in
[`CHANGES-FROM-UPSTREAM.md`](../CHANGES-FROM-UPSTREAM.md) and
[`../README.md`](../README.md); this page is the other half — what the platform
does, and how to check it rather than guess.

---

## A new window opens on the monitor at (0,0), not the primary one

**Symptom.** Every session window opened on the same "wrong" display, no matter
where the configuration box was — reported as "it open at my 2nd monitor no
matter what".

**Cause.** Left alone, macOS puts a new window at the origin of the monitor
whose frame starts at `(0,0)`. That is not necessarily the primary monitor, and
it is not where the pointer is:

| Display | GDK geometry | `gdk_monitor_is_primary` |
|---|---|---|
| LG UltraFine 4K | `1920,0 1920x1080` | **1** |
| DELL U2422H | `0,0 1920x1080` | 0 |

With that layout the configuration box appeared at `2518,194` (the primary) and
**every** session window at `0,30` (the display at the origin).

**What does *not* fix it:**

* `gtk_window_set_position(window, GTK_WIN_POS_CENTER)` — centres on the
  **primary** monitor, wherever the pointer is. Measured: with the pointer
  warped to `200,300`, i.e. onto the display at `0,0`, and hard against a
  freshly created window, the session still opened on the primary — so the box
  had still been recorded as being there. That is why the configuration box is
  always on the primary display, whatever you were doing when you launched
  PuTTY.
* Any idea of asking the platform again — there is no GTK "same screen as the
  parent" setting, and a toplevel that is not positioned gets the `(0,0)`
  treatment above.

**What works** (`unix/window.c`): remember the monitor of the last PuTTY window
the user moved or resized — `gtk_remember_window_monitor()`, fed by the session
windows' own configure handler and by `config_box_configure()` in
`unix/dialog.c` — and `gtk_window_move()` the new window to the centre of it.
Nothing remembered means the primary monitor (via
`gdk_display_get_primary_monitor()`), and a window larger than the monitor is
clamped to its top-left corner instead of being given a negative offset. See
[`CHANGES-FROM-UPSTREAM.md`](../CHANGES-FROM-UPSTREAM.md) §16.

**`gtk_window_move()` before the window is shown does stick** on this backend —
no need to move it after `gtk_widget_show()`, and so no visible jump. (Asked for
`2360,233`; `gtk_window_get_position()` then reported `2360,201`, the same
monitor, the `y` differing by the drawn title bar.)

---

## `-geometry` does nothing at all

`putty -geometry 80x24+100+100` is accepted, then ignored — **size included**.
Measured: with `-geometry 40x10+0+0` the window still came up 1040x614 pixels,
which is the default 128x36.

The parse is inside `#ifndef NOT_X_WINDOWS` in `unix/window.c` and uses
`XParseGeometry`; `NOT_X_WINDOWS` is defined for `__APPLE__` in
`unix/platform.h`, so on macOS the whole block — size, position and gravity —
is compiled out, and `inst->gotpos` is never set. Upstream's, not this fork's;
`CHANGES-FROM-UPSTREAM.md` carries it in "Worth reporting upstream".

Consequence worth knowing when editing: the `if (inst->gotpos)` branch in
`new_session_window()` is **dead code on macOS**, and the monitor choice above is
its `else`. An explicit position would still win if `-geometry` were ever made to
work.

---

## A native title bar is painted from the window's own background

Asking for a non-opaque window makes the *native* title bar transparent too,
because macOS draws it from the window's background and GDK clears that to fully
transparent. Measured with a probe reading AppKit's own state:
`[NSWindow isOpaque]` 1 → 0 and `backgroundColor` alpha 1 → 0 at transparency 64,
while `titlebarAppearsTransparent` stayed 0 and `styleMask` unchanged — so it is
not a flag anything set. The fix is a GTK-drawn title bar while the window is
see-through ([`../README.md`](../README.md) §12b,
[`CHANGES-FROM-UPSTREAM.md`](../CHANGES-FROM-UPSTREAM.md) §15).

`gtk_window_set_titlebar()` does **not** show the bar by itself: it shows it only
if the window is already visible, and this window is shown with
`gtk_widget_show()` rather than `show_all()`.

---

## GTK windows and menus are invisible to AppleScript

Handy for reproducing a bug and for driving a modal, with one real limitation:

* **Works:** `keystroke` and `key code` to the frontmost app
  (`tell application "System Events" to tell process "putty" to set frontmost to
  true`, then a keystroke), and scripted dialogs can be answered that way.
* **Does not work:** anything that walks the window or menu tree. Measured:
  `tell application "System Events" to tell process "putty" to set position of
  window 1 to ...` → `Can't get window 1 of process "putty". Invalid index.
  (-1719)`. GTK on macOS exposes no accessibility hierarchy at all, so a window
  cannot be moved and a menu item cannot be clicked from a script.

So a multi-monitor or window-move test **cannot** be scripted that way. Two
things that do work instead:

* Move the window from **inside** the session. The terminal implements the
  xterm request `ESC [ 3 ; x ; y t` (`terminal/terminal.c`, case 3, gated on
  `no_remote_resize`), so a session that prints `\033[3;150;150t` moves its own
  window: verified by tracing `gtkwin_move()` — it is called with the requested
  `x,y` — and `gtk_window_move()` does take effect on this backend (see above).
  `ESC [ 4 ; h ; w t` (resize) is **not** implemented; the source says so.
* Warp the **pointer** with a two-line CoreGraphics program —
  `CGWarpMouseCursorPosition(CGPointMake(x, y))`, built with
  `clang -framework ApplicationServices`, kept out of the tree. Worth knowing it
  changes nothing for `GTK_WIN_POS_CENTER` (see above), so it is only useful for
  things that genuinely follow the pointer.

There is no way to get a second window into one process from the command line,
either: the `puttyapp` / `ptermapp` builds reject `-load` and `-e`, and each
launch is its own process. The in-window menu is the only route, and it is the
one AppleScript cannot reach.

---

## Looking at this without a screenshot

The recipes that actually answered the questions above, all private to the
terminal (no screen capture):

* **Monitor layout:** `gdk_display_get_n_monitors()`, then
  `gdk_display_get_monitor()` → `gdk_monitor_get_geometry()` and
  `gdk_monitor_is_primary()` for each.
* **Where a window really is:** `gtk_window_get_position()` — which is how the
  difference between what `gtk_window_move()` was asked for and what it got
  (`2360,233` vs `2360,201`) showed up.
* **What the terminal painted:** render the backing surface offscreen
  (`cairo_surface_write_to_png()`, composited over magenta to make the alpha
  visible) rather than capturing the screen — that is what found the cursor
  ghost (`CHANGES-FROM-UPSTREAM.md` §15).
* **In the shipped build:** `PUTTY_DIAG=1`, which now reports the monitor, the
  window size and the final position:
  `new session window: monitor 1920,0 1920x1080, window 1040x614, placing at
  2360,233`.

---

## Two small compile traps

* `gdk_device_get_position()` returns **void** in GTK 3, not `gboolean` — unlike
  `gdk_device_get_window_at_position()`. `if (gdk_device_get_position(...))` is a
  compile error.
* `GtkWidget` is only a known type in `unix/platform.h` under
  `#ifdef MAY_REFER_TO_GTK_IN_HEADERS`, which `unix/window.c` and
  `unix/dialog.c` define before including `putty.h`. A GTK-typed prototype added
  outside that guard breaks every other translation unit that includes the
  header (`plink`, `pscp`, the tests).
