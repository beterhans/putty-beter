# tools/

Third-party tools needed to build the macOS application bundle.

## gtk-mac-bundler (required for `PuTTY.app`)

`scripts/bundle-macos.sh` needs
[gtk-mac-bundler](https://gitlab.gnome.org/GNOME/gtk-mac-bundler) to assemble
`PuTTY.app`. It is the tool that copies GTK and all of its dependencies into the
bundle and rewrites their install names so the app is self-contained.

It is **not** included in this repository:

* it is a separate GNOME project, not part of PuTTY;
* it is licensed **GPL-2.0**, while this tree is MIT — keeping it out of the
  source history avoids mixing licences for something that is only a
  build-time tool.

### Fetch it

```sh
git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git \
    tools/gtk-mac-bundler
```

That is the location `scripts/bundle-macos.sh` prefers. You can also keep a
checkout elsewhere and point the script at it:

```sh
BUNDLER_DIR=/path/to/gtk-mac-bundler ./scripts/bundle-macos.sh arm64
```

### Search order used by the script

1. `$BUNDLER_DIR` — explicit override
2. `tools/gtk-mac-bundler` — in-project checkout (preferred)
3. `~/gtk-mac-bundler` — fallback

### Other requirements

* `gtk+3` (via Homebrew), `cmake`, `ninja`, `pkg-config`
* `adwaita-icon-theme` (provides `share/icons/Adwaita` used by the bundle)
* Python 3 for gtk-mac-bundler