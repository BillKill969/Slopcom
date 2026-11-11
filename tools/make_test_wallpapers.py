#!/usr/bin/env python3
import zlib, struct, os

def create_png(width, height, get_pixel, filename):
    raw_data = bytearray()
    for y in range(height):
        raw_data.append(0) # filter byte 0 (None)
        for x in range(width):
            r, g, b = get_pixel(x, y, width, height)
            raw_data.extend((int(r) & 0xff, int(g) & 0xff, int(b) & 0xff))
    
    def chunk(chunk_type, data):
        return struct.pack('>I', len(data)) + chunk_type + data + struct.pack('>I', zlib.crc32(chunk_type + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw_data), 6)
    
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')
    with open(filename, 'wb') as f:
        f.write(png)

def pixel_wp1(x, y, w, h):
    # Asymmetric corner markers (40x40):
    # Top-left: RED
    if x < 40 and y < 40: return (255, 0, 0)
    # Top-right: GREEN
    if x >= w - 40 and y < 40: return (0, 255, 0)
    # Bottom-left: BLUE
    if x < 40 and y >= h - 40: return (0, 0, 255)
    # Bottom-right: YELLOW
    if x >= w - 40 and y >= h - 40: return (255, 255, 0)
    # Background: dark slate with subtle grid
    if x % 40 == 0 or y % 40 == 0:
        return (60, 70, 80)
    return (25, 30, 36)

def pixel_wp2(x, y, w, h):
    # Cyan to magenta diagonal gradient
    r = int(255 * (x / w))
    g = int(120 * (1.0 - (x / w)))
    b = int(255 * (y / h))
    return (r, g, b)

def pixel_tile(x, y, w, h):
    # 37x23 odd-sized checkerboard
    return (220, 220, 220) if ((x // 6 + y // 6) % 2 == 0) else (30, 30, 50)

if __name__ == '__main__':
    create_png(640, 360, pixel_wp1, '/tmp/wallpaper1.png')
    create_png(640, 360, pixel_wp2, '/tmp/wallpaper2.png')
    create_png(37, 23, pixel_tile, '/tmp/wallpaper_tile.png')
    print("Test wallpapers generated in /tmp:")
    print("  /tmp/wallpaper1.png (640x360 asymmetric corner markers)")
    print("  /tmp/wallpaper2.png (640x360 gradient)")
    print("  /tmp/wallpaper_tile.png (37x23 odd-sized checkerboard)")
