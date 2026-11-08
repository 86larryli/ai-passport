#!/usr/bin/env bash
# Regenerate the Claude Code Companion fonts in assets/fonts/ from Noto Sans SC.
#
# Usage: tools/gen_cc_fonts.sh <NotoSansSC-Medium.otf> [preview-dir]
#
# Source font: notofonts/noto-cjk, Sans/SubsetOTF/SC/NotoSansSC-Medium.otf
# (SIL Open Font License 1.1). It is not committed; assets/README.md records the
# exact file hash this repository's fonts were generated from. Requires uv; the
# generator pins freetype-py and verifies every glyph after writing.
set -euo pipefail

font="${1:?usage: $0 <NotoSansSC-Medium.otf> [preview-dir]}"
preview_dir="${2:-}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

generate() {
    local name="$1" size="$2" sets="$3" line_height="$4" base_line="$5" sample="$6"
    local extra=()
    if [[ -n "${preview_dir}" ]]; then
        extra=(--preview "${preview_dir}/${name}.png" --sample "${sample}")
    fi
    uv run --no-project --with freetype-py==2.5.1 python -I tools/gen_lvgl_font.py \
        --font "${font}" --size "${size}" --sets "${sets}" --name "${name}" \
        --line-height "${line_height}" --base-line "${base_line}" \
        --output "assets/fonts/${name}.c" "${extra[@]+"${extra[@]}"}"
}

# Dynamic content (prompts, questions, paths, Chinese text): full GB2312 + symbols.
generate cc_font_cjk_16 16 ascii,cjk-dynamic 20 4 \
    'ai-passport #1  Explore\n$ idf.py build --ccache\n修复构建错误，然后运行测试。gjpqy\n“需要确认”：删除 build/ 目录？□'
# Fixed English UI labels and hints.
generate cc_font_ui_12 12 ascii,ui-symbols 15 3 \
    'WORKING  NEEDS APPROVAL  3m12s\n▲▼ select   OK open   hold OK menu ✓ × • …'
# Titles (project names fall back to cc_font_cjk_16 for Chinese).
generate cc_font_ui_20 20 ascii,ui-symbols 25 5 \
    'ai-passport #2 Settings\nNo active sessions gjpqy'
# Pairing passkey and other large digits.
generate cc_font_num_40 40 digits 48 9 '123 456\n14:32'
