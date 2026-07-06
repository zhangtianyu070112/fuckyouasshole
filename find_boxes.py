from PIL import Image

img = Image.open('assets/assets/main.png')
pixels = img.load()
w, h = img.size

def is_target(val):
    # (21, 26, 29) or close to it
    return abs(val[0]-21) < 5 and abs(val[1]-26) < 5 and abs(val[2]-29) < 5

visited = set()
boxes = []

for y in range(0, h, 10):
    for x in range(0, w, 10):
        if is_target(pixels[x, y]) and (x, y) not in visited:
            # We found a potential screen. Let's trace its bounding box.
            # We can use BFS to find all connected pixels of similar color.
            # But BFS might be slow in python for such a huge image.
            # Let's just find the min_x, max_x, min_y, max_y by walking horizontally and vertically.
            
            # find left and right bounds at current y
            lx = x
            while lx > 0 and is_target(pixels[lx, y]): lx -= 1
            rx = x
            while rx < w-1 and is_target(pixels[rx, y]): rx += 1
            
            # find top and bottom bounds at current x
            uy = y
            while uy > 0 and is_target(pixels[x, uy]): uy -= 1
            dy = y
            while dy < h-1 and is_target(pixels[x, dy]): dy += 1
            
            wid = rx - lx
            hei = dy - uy
            
            if wid > 500 and hei > 500:
                # check if we already have this box
                cx, cy = lx + wid//2, uy + hei//2
                found = False
                for bx, by, bw, bh in boxes:
                    if abs(bx + bw//2 - cx) < 200 and abs(by + bh//2 - cy) < 200:
                        found = True
                        break
                if not found:
                    boxes.append((lx, uy, wid, hei))
                    # Mark area as visited
                    for vy in range(uy, dy+1, 50):
                        for vx in range(lx, rx+1, 50):
                            visited.add((vx, vy))

for i, b in enumerate(sorted(boxes, key=lambda x: (x[0]))):
    print(f"Box {i}: x={b[0]}, y={b[1]}, w={b[2]}, h={b[3]}")
