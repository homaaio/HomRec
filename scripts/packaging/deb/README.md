# scripts/packaging/deb/

Empty on purpose — placeholder for `.deb` packaging metadata, for whenever
there's an actual Linux build of HomRec to package. This is where a
`debian/` control tree would go (`control`, `postinst`/`prerm` scripts,
`.desktop` file, changelog, etc.) once `dpkg-deb`/`debuild` gets wired up
in `scripts/release-linux.sh`.

This is deliberately *separate* from `src/platform/linux/` — `.deb` vs.
Arch's `PKGBUILD` vs. a Flatpak manifest is a packaging-format choice,
which has nothing to do with which display-server backend (X11/Wayland,
see `src/platform/linux/`) the binary inside the package was built
against. A single Linux build of HomRec could in principle ship as a
`.deb`, a `PKGBUILD`, and a Flatpak all at once, from the same source
tree — that's why packaging lives here under `scripts/`, next to
whatever format-specific subfolder it needs (add `arch/`, `flatpak/`,
etc. here later the same way, if/when they're actually built).
