# JetKVM display UI snapshot

`ui/` is derived from `internal/native/eez/src/ui` in the JetKVM `kvm`
repository at commit `c775979ccbf5a4784d78d689f4583ab171118f87`. It contains
the generated direct-LVGL screens, fonts, and images. The screen roots, common
page margins, and Home status flow are adapted for the P4 board's native
240x240 panel. EEZ Flow is not enabled or linked.

The derived UI is distributed under the MIT license in the repository's
`LICENSE`. Images, object names, and all unmodified generated sources remain
directly auditable against that upstream revision.

The fonts in `ui/fonts/` and `assets/font_bold20.c` are generated from Inter
(`ui/fonts/inter/`, SIL Open Font License 1.1, see `ui/fonts/inter/OFL.txt`)
by `ui/fonts/regenerate.sh`. They keep the upstream file and symbol names.
