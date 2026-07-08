from PIL import Image, ImageDraw, ImageFont
import json

img = Image.open('assets/assets/fmc.png')
draw = ImageDraw.Draw(img)

with open('location-FMC.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

lsk_centers = []
for btn in data['fmc_buttons']:
    if btn['label'].startswith('LSK_') and btn['label'].endswith('L'):
        y_center = (btn['bbox'][1] + btn['bbox'][3]) / 2.0
        lsk_centers.append(y_center)

rect_h = img.height
rect_w = img.width

# draw horizontal lines at LSK centers
for yc in lsk_centers:
    y = int(yc * rect_h)
    draw.line([(0, y), (rect_w, y)], fill='red', width=2)
    # also draw text
    draw.text((rect_w // 2, y), f'Center {yc:.3f}', fill='red')

img.save('fmc_debug.png')
