from PIL import Image, ImageDraw

img = Image.open('assets/assets/fmc.png')
draw = ImageDraw.Draw(img)

lsk_centers = [0.132500, 0.182500, 0.229500, 0.277500, 0.325500, 0.375500]
rect_h = img.height
rect_w = img.width

for i, yc in enumerate(lsk_centers):
    y = int(yc * rect_h)
    draw.line([(0, y), (rect_w, y)], fill='red', width=2)
    draw.text((rect_w // 2, y), f'LSK {i+1} Center {yc:.3f}', fill='red')

img.save('fmc_debug.png')
