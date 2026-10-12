"""Recreate the flash-resident clock subset with the pinned LVGL font converter.

The LVGL component supplies Montserrat Bold. Its SIL license is distributed
alongside the generated board font. No runtime font download or allocation.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai'
if __name__ == '__main__':
    converter = shutil.which('npx.cmd') or shutil.which('npx')
    if not converter:
        raise SystemExit('Node.js/npx is required to regenerate the optional clock font.')
    subprocess.run([converter, '--yes', '--package', 'lv_font_conv@1.5.3', 'lv_font_conv',
                    '--font', 'managed_components/lvgl__lvgl/tests/src/test_files/fonts/Montserrat-Bold.ttf',
                    '--size', '64', '--bpp', '4', '--symbols', '0123456789:-',
                    '--no-compress', '--no-kerning', '--format', 'lvgl',
                    '--lv-font-name', 'pocket_clock_digits_64', '--output',
                    'main/boards/custom-esp32-nodemcu-pocket-ai/clock_digits.c'],
                   cwd=ROOT, check=True)
    target = BOARD / 'clock_digits.c'
    source = target.read_text(encoding='utf-8').replace(' * Size: 64 px',
        ' * Montserrat Bold, SIL Open Font License (see CLOCK_FONT_LICENSE.txt).\n'
        ' * Generated with lv_font_conv@1.5.3; see scripts/pocket_clock_font.py.\n * Size: 64 px')
    target.write_text(source.rstrip() + '\n', encoding='utf-8')
