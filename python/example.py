"""Animated example: a gradient with a bouncing ball, sent to vcam camera 1.

    python example.py            # needs numpy
"""

import math
import time

import numpy as np

import vcam


def main():
    with vcam.Camera(fmt="rgb", fps=30) as cam:
        print("Streaming to the camera. Open it in any app (Camera, Teams, Zoom, OBS...). Ctrl+C stops.")
        # Render at whatever size apps currently ask for (1280x720 until one does).
        width, height = cam.width or 1280, cam.height or 720
        y, x = np.mgrid[0:height, 0:width]
        base = np.zeros((height, width, 3), np.uint8)
        base[..., 0] = (x * 255 // max(width - 1, 1)).astype(np.uint8)
        base[..., 2] = (y * 255 // max(height - 1, 1)).astype(np.uint8)
        start = time.perf_counter()
        while True:
            t = time.perf_counter() - start
            frame = base.copy()
            cx = int((0.5 + 0.4 * math.sin(t * 1.3)) * width)
            cy = int((0.5 + 0.4 * math.cos(t * 1.7)) * height)
            r = height // 10
            mask = (x - cx) ** 2 + (y - cy) ** 2 < r * r
            frame[mask] = (255, 255, 255)
            cam.send(frame)
            cam.sleep_until_next_frame()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
