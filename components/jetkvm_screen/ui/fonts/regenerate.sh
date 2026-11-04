#!/usr/bin/env bash
# Writes the display's LVGL fonts from the Inter TTF files in inter/:
# ui_font_font_book*.c from Inter Regular, ui_font_font_bold30.c and
# ../../assets/font_bold20.c from Inter Bold. Needs Node.js; npx fetches
# lv_font_conv 1.5.2.
#
# Inter is wider and taller than the font the UI was laid out for, so each
# font is generated smaller than its name: book16 is Inter at 15 px. bold30 is
# 24 px, so that every IPv4 address fits the 216 px content width of the home
# screen. The names stay, since the generated screens refer to them.
#
# lv_font_conv sets line_height from the tallest glyph, '|', which makes lines
# 1-4 px taller than the layout expects. Each font gets the line height of the
# font the layout was made for, and the base line lv_font_conv computes
# without '|'.
#
#   components/jetkvm_screen/ui/fonts/regenerate.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

convert() {
    local font="$1" size="$2" output="$3" line_height="$4" base_line="$5"
    npx -y lv_font_conv@1.5.2 --bpp 4 --size "$size" --no-compress \
        --font "$font" --range 32-127 --format lvgl -o "$output"
    sed -i -e "s/^\( *\.line_height = \)[0-9]*,/\1$line_height,/" \
        -e "s/^\( *\.base_line = \)[0-9]*,/\1$base_line,/" "$output"
}

convert inter/Inter-Regular.ttf 15 ui_font_font_book16.c 17 3
convert inter/Inter-Regular.ttf 17 ui_font_font_book18.c 19 4
convert inter/Inter-Regular.ttf 19 ui_font_font_book20.c 20 5
convert inter/Inter-Regular.ttf 23 ui_font_font_book24.c 25 5
convert inter/Inter-Bold.ttf 24 ui_font_font_bold30.c 31 5
# lv_font_conv names the font after the output file; the symbol is
# ui_font_font_bold20.
convert inter/Inter-Bold.ttf 19 ui_font_font_bold20.c 21 4
mv ui_font_font_bold20.c ../../assets/font_bold20.c

# The files in this folder keep the include block of the generated UI, which
# finds lvgl.h without LV_LVGL_H_INCLUDE_SIMPLE (the host renderer).
python3 - ui_font_font_*.c <<'EOF'
import sys
old = '''#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
'''
new = '''#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl.h"
#endif
'''
for path in sys.argv[1:]:
    text = open(path).read()
    if old not in text:
        sys.exit(f"{path}: lv_font_conv include block not found")
    open(path, "w").write(text.replace(old, new, 1))
EOF
