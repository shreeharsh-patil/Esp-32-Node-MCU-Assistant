"""Convert actual LVGL captures into state, boot and audio-reactive previews.

Requires Pillow. The captures use simulated states and audio levels; they are
not photographs or proof of panel orientation, acoustic playback or service use.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
output = ROOT / 'docs/pocket/ui-preview'
for path in output.glob('*.ppm'):
    with Image.open(path) as capture:
        capture.save(path.with_suffix('.png'))
names = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'confused',
         'sleepy', 'error', 'setup', 'pairing-layout', 'curious', 'shy',
         'surprised', 'processing']
font_path = Path('C:/Windows/Fonts/segoeui.ttf')
font = ImageFont.truetype(str(font_path), 20) if font_path.exists() else ImageFont.load_default()
small = ImageFont.truetype(str(font_path), 16) if font_path.exists() else ImageFont.load_default()
sheet = Image.new('RGB', (1510, 880), '#efe5d6')
draw = ImageDraw.Draw(sheet)
draw.text((25, 12), 'Companion UI — actual LVGL renderer, simulated states', fill='#3d3631', font=font)
for index, name in enumerate(names):
    x, y = 20 + (index % 5) * 298, 54 + (index // 5) * 270
    with Image.open(output / (name + '.png')) as capture:
        sheet.paste(capture, (x, y))
    label = 'Wi-Fi instructions' if name == 'pairing-layout' else name.replace('-', ' ').capitalize()
    draw.text((x + 4, y + 243), label, font=small, fill='#64544a')
sheet.save(output / 'companion-preview.png')
frames = [Image.open(output / f'boot-{time}.png') for time in range(0, 1189, 99)]
frames[0].save(output / 'boot-preview.gif', save_all=True, append_images=frames[1:],
               duration=[99] * 12 + [1000], loop=0)
for frame in frames:
    frame.close()
frames = []
for index in range(160):
    with Image.open(output / f'reactive-{index}.png') as capture:
        frame = Image.new('RGB', (280, 274), '#000000')
        frame.paste(capture)
    label = ('Listening: simulated mic' if index < 38 else
             'Thinking: upward gaze' if index < 68 else
             'Replying: simulated PCM' if index < 130 else 'Quiet: mouth closes')
    ImageDraw.Draw(frame).text((8, 246), label, font=small, fill='#cfc7bf')
    frames.append(frame)
frames[0].save(output / 'audio-reactive-preview.gif', save_all=True,
               append_images=frames[1:], duration=66, loop=0)
print('Native LVGL screenshots, state sheet, boot and audio-reactive animations saved')
