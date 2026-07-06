from PIL import Image
from collections import Counter

img = Image.open('assets/assets/main.png')
pixels = img.load()
w, h = img.size

counts = Counter()
for y in range(0, h, 10):
    for x in range(0, w, 10):
        counts[pixels[x, y]] += 1

for color, count in counts.most_common(10):
    print(f"Color {color}: {count} pixels")
