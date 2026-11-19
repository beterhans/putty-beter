# Saved-session search: how it works

How the search box in the Session panel is put together, for whoever works on it
next. The *user-facing* description is [README §9b](../README.md#9b-searching-saved-sessions);
the *why* is `PLAN.md` §4j, including what building it actually turned up. This
file is the *how*: the rule, the files, the cache, and how to drive it by hand.

Status: **done** (2026-11-19). Type `/` (or `\`) in the **Saved Sessions** box
and the list below filters live by session name, host name/IP or comment.

---

## 1. The rule

The **Saved Sessions** box is two controls in one. Its contents decide which:

| Typed | Meaning |
|---|---|
| `Work/beta` | an ordinary session name (what it always was) |
| `/beta` | *search*. Everything after the prefix is the query |
| `\beta` | the same search, for anyone who thinks in backslashes |
| `/` | search mode with an empty query, which matches everything |

A session matches when the query appears anywhere in

* its **name**, including the folder path — so `/Work` finds `Work/beta` and the
  folder row `Work/` itself, and a query that names a folder behaves the way
  "show me that folder" would;
* its **HostName**, which is where an IP address lives;
* its **SessionComment**.

Matching is a plain substring test, **case-insensitive**, and folded one byte at
a time (`A`–`Z` only: right for names and hosts, good enough for comments, and no
Unicode case mapping to get subtly wrong). A comment is matched in the form it is
stored in, so a query containing `\n` matches a literal backslash-n in a comment;
ordinary word searches never notice.

A leading `/` is free because a session name is a `/`-separated path and no valid
path begins with `/` (`session_name_is_valid()` rejects an empty component), so
nothing a user could legitimately save is taken away, and no session can be
shadowed by it. `\` cannot appear in a session name at all, so it is
unambiguous — but it is not the shape of "search" anywhere else, so `/` is what
the docs, the placeholder and the README use, and `\` is an alias.

## 2. What the user sees

* The list filters as they type: rows are added to the same tree list box, so a
  filtered list is still a tree. **Only sessions are filtered in**; the rows for
  their parent folders are made by the existing tree code
  (`tree_get_iter_for_path()`), and a parent row is not loadable, exactly as it
  is not loadable today.
* The box's **label** becomes `Search (matched/total)` — via the existing
  `dlg_label_change()`, which already had a `CTRL_EDITBOX` case — and goes back
  to `Saved Sessions` when the search ends.
* Rows keep their existing **ids** (their index in `ssd->sesslist`), so
  **Load**, **Delete**, **drag** and **double-click-to-open** all keep working on
  a filtered list. Finding a session and then loading it is two steps, not three.
* The cursor lands on the **first match**.
* No match is not an error: the list is empty, the label says `Search (0/n)`,
  and there is no beep (a beep on every keystroke would be unbearable).
* **Save** and **New Folder** beep and do nothing while a search is on: `/beta`
  is not a session name, and letting it through would only draw an
  "Invalid session name" out of `save_settings()`.
* Clearing the prefix — or the box — puts the whole list back and restores the
  old behaviour, including the binary-search jump to the nearest name as you
  type an ordinary name.

An empty box carries the hint **Name/folder, or / to search** as greyed-out
placeholder text. It is deliberately short: the box is only as wide as the panel
and the front end clips what will not fit.

## 3. The files

| File | What is in it |
|---|---|
| `utils/sesssearch.c` | the whole rule: `sesssearch_query()`, `sesssearch_haystack()`, `sesssearch_match()`. **No I/O, no GTK, no globals** — which is what makes it testable |
| `utils/sesssearch.h` | its interface, the prefix characters, and `PUTTY_SESS_SEARCH_DEBUG` |
| `test/test_sesssearch.c` | 33 checks over the above. Links only `utils` |
| `config.c` | the dialog's half: the match cache, `sessionsaver_fill_list()`, the `EVENT_VALCHANGE` branch, the Save / New Folder guards, and the placeholder string |
| `dialog.h`, `dialog.c` | the edit box's `placeholder` field, and its initialisation in `ctrl_editbox()` / `ctrl_combobox()` |
| `unix/dialog.c` | `gtk_entry_set_placeholder_text()` for that field, under `#if GTK_CHECK_VERSION(3,0,0)` |

The split is the point: everything that can be decided about a string without
touching the disk or a widget lives in `utils/`, so it is tested without a front
end. Everything that needs the session list, the files, or the list box lives in
`config.c`, where it can only be tested by hand.

## 4. Where the searched text comes from, and the cache

For each session, `sesssearch_build()` reads

* the name, from `ssd->sesslist` (so it includes the folder path), and
* `HostName` and `SessionComment` from the session's own file, with
  `open_settings_r()` and `read_setting_s()` — the same two keys `settings.c`
  writes. A session whose file cannot be opened (a folder row, say) is still
  searchable by name.

Those three are joined with newlines and lowercased once, by
`sesssearch_haystack()`, and kept in `ssd->hay` — one entry per row of the
session list. The point is that a **keystroke does no I/O**: without the cache,
every character typed would re-read every saved session file.

The cache is dropped by `sesssearch_forget()` wherever the session list is
rebuilt — `delete_names()`, `sessionsaver_drag()`, the Save handler, the New
Folder handler — and in `sessionsaver_data_free()`. It is rebuilt lazily, on the
next search. Forgetting it is what makes "delete a session while filtering" show
the truth rather than a stale match; `PUTTY_SESS_SEARCH_DEBUG=1` prints
`read search text for N sessions` precisely so that a missing invalidation is
visible.

`ssd->searched` remembers whether the previous keystroke was a search, so that
leaving search mode can refill the list **once** and then hand back to the
untouched jump-to-nearest-name code.

## 5. Debugging it

```sh
PUTTY_SESS_SEARCH_DEBUG=1 putty
```

Every search then traces to stderr, in the same shape as `PUTTY_CONFIG_DEBUG`:

```
sesssearch: read search text for 8 sessions     <- the cache was rebuilt
sesssearch: query "work": 3 of 8 list entries   <- one line per keystroke
```

A missing `read search text` line where the session list changed is the
invalidation bug; a wrong count is the matching rule. Nothing is printed when
the switch is unset.

## 6. Testing it

```sh
ninja -C build-arm64 test_sesssearch && ./build-arm64/test_sesssearch
```

33 checks: the prefix rule (`/x` and `\x` are searches, `Work/x` is not, a
prefix that is not first is not), host and comment hits, case folding, a
folder-name query reaching `Folder/child`, NULL host/comment, an empty query
matching, and no false matches.

The dialog half has no test, so it wants a human. It can be driven without one,
which is how it was checked: a throwaway configuration directory plus synthetic
keystrokes.

```sh
TMPD=~/Downloads/temp/sesstest                     # never a real config folder
mkdir -p "$TMPD/sessions/Work"
printf 'HostName=10.0.1.7\nProtocol=ssh\nPresent=1\nSessionComment=prod box\n' \
    > "$TMPD/sessions/Router_Prod.conf"
printf 'HostName=beta.internal\nProtocol=ssh\nPresent=1\n' \
    > "$TMPD/sessions/Work/beta.conf"

PUTTYDIR="$TMPD" PUTTY_SESS_SEARCH_DEBUG=1 ./build-arm64/putty &
# from the Host Name box, the Saved Sessions entry is 5 forward tabs
osascript -e 'tell application "System Events" to tell process "putty" to set frontmost to true' \
        -e 'tell application "System Events"' -e 'repeat 5 times' -e 'key code 48' \
        -e 'delay 0.25' -e 'end repeat' -e 'keystroke "/beta"' -e 'end tell'
screencapture -x /tmp/shot.png
sips -c 1900 2300 --cropOffset 60 1150 /tmp/shot.png --out /tmp/win.png   # the window
```

Three things worth knowing before trying that, learned the hard way:

* `osascript`'s `click at {x,y}` fails with **-25204** and mnemonics (`Option+e`)
  never fire, so keyboard traversal is the only way in.
* Tab counting is the only way to be sure what has focus — and **a probing loop
  that types to find out writes into whatever control is focused at the time**.
  That is how `/beta` once ended up in the Host Name box and looked like a bug.
* `strings` on `PuTTY.app/Contents/MacOS/PuTTY` finds nothing, because that file
  is only the launcher; the strings are in **`PuTTY-bin`**.

## 7. Limits, and things deliberately left out

* **Not** highlighted: the matched substring. The tree list box stores plain
  text and would need a markup cell renderer.
* **Not** matched: the port number, `sshhostkeys`, or the host-CA list.
* **Not** supported: regex or glob queries, and remembering the last query
  between dialog boxes.
* **ASCII** case folding only, and comments are matched in their escaped form.
* On **Windows** the front end reports `EVENT_VALCHANGE` on focus-out rather than
  per keystroke, so the search there would take effect when the box is left.
  This is a macOS-first fork and the plan does not add per-keystroke notification
  to the Windows front end.
* Refilling the list on every keystroke is a GTK list rebuild — the same cost
  `dlg_refresh()` already pays on Save, and fine at a few hundred sessions. The
  cache is what keeps it from also being a few hundred file reads.

### One thing that is not ours

Pressing **Load** prints

```
Gtk-CRITICAL: gtk_tree_selection_get_selected: assertion 'priv->type != GTK_SELECTION_MULTIPLE' failed
```

to stderr. It is pre-existing — it reproduces with no search active — because
`dlg_listbox_selected_id()` in `unix/dialog.c` calls
`gtk_tree_selection_get_selected()` on a list box that is multi-select, and has a
cursor-row fallback for exactly that case. Harmless noise to a terminal, and left
for a change of its own rather than smuggled into this one.
