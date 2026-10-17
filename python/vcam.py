"""Feed frames to a vcam virtual camera from Python.

    import numpy as np
    import vcam

    with vcam.Camera(fmt="rgb") as cam:           # camera 1
        print("apps see", cam.width, "x", cam.height)
        frame = np.zeros((720, 1280, 3), np.uint8)
        while True:
            frame[:] = ...                         # draw something
            cam.send(frame)                        # any size; vcam scales it
            cam.sleep_until_next_frame()

Frames are numpy arrays (or anything exposing the buffer protocol together
with explicit width/height): (H, W, 3) for "rgb"/"bgr", (H, W, 4) for
"rgba"/"bgra", (H, W) for "gray", (H * 3 // 2, W) for "nv12"/"i420" and
(H, W * 2) for "yuy2"/"uyvy". OpenCV images are "bgr".

Needs vcam installed (`vcam install`), which puts vcam.dll next to vcam.exe;
set VCAM_DLL to use another copy. Python's own bitness must be 64-bit.
"""

import ctypes
import os
import sys
import time

__all__ = ["Camera", "VcamError", "FORMATS"]

FORMATS = {
    "bgra": 1, "rgba": 2, "bgr": 3, "rgb": 4, "gray": 5,
    "nv12": 6, "i420": 7, "yuy2": 8, "uyvy": 9,
}
_FORMAT_NAMES = {v: k for k, v in FORMATS.items()}
_SCALES = {"fit": 0, "fill": 1, "stretch": 2}
_COLORSPACES = {"bt601": 0, "bt709": 1, "bt601-full": 2, "bt709-full": 3}


class _Options(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("camera", ctypes.c_int32),
        ("scale", ctypes.c_int32),
        ("mirror", ctypes.c_int32),
        ("flip", ctypes.c_int32),
        ("background", ctypes.c_uint32),
    ]


class _Frame(ctypes.Structure):
    _fields_ = [
        ("format", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("planes", ctypes.c_void_p * 3),
        ("pitches", ctypes.c_int32 * 3),
        ("colorspace", ctypes.c_int32),
        ("timestamp", ctypes.c_int64),
    ]


class _Status(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("connected", ctypes.c_int32),
        ("consumer_active", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("format", ctypes.c_int32),
        ("fps_numerator", ctypes.c_int32),
        ("fps_denominator", ctypes.c_int32),
        ("frames_sent", ctypes.c_uint64),
        ("frames_delivered", ctypes.c_uint64),
        ("frames_repeated", ctypes.c_uint64),
    ]


def _packed_size(fmt, w, h):
    name = _FORMAT_NAMES[fmt]
    cw, ch = (w + 1) // 2, (h + 1) // 2
    per_pixel = {"bgra": 4, "rgba": 4, "bgr": 3, "rgb": 3, "gray": 1}
    if name in per_pixel:
        return w * h * per_pixel[name]
    if name in ("nv12", "i420"):
        return w * h + 2 * cw * ch
    return cw * 4 * h


class VcamError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _find_dll():
    candidates = []
    if os.environ.get("VCAM_DLL"):
        candidates.append(os.environ["VCAM_DLL"])
    here = os.path.dirname(os.path.abspath(__file__))
    candidates += [os.path.join(here, "vcam.dll"), os.path.join(here, "..", "sdk", "vcam.dll")]
    if sys.platform == "win32":
        try:
            import winreg

            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\vcam", 0,
                                 winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            install_dir, _ = winreg.QueryValueEx(key, "InstallDir")
            candidates.append(os.path.join(install_dir, "vcam.dll"))
        except OSError:
            pass
    for path in candidates:
        if os.path.isfile(path):
            return os.path.abspath(path)
    raise VcamError(-3, "vcam.dll not found: install vcam (`vcam install`) or set VCAM_DLL")


_lib = None


def _library(path=None):
    global _lib
    if _lib is not None and path is None:
        return _lib
    lib = ctypes.CDLL(path or _find_dll())
    lib.vcam_open.argtypes = [ctypes.POINTER(_Options), ctypes.POINTER(ctypes.c_void_p)]
    lib.vcam_send.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Frame)]
    lib.vcam_get_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Status)]
    lib.vcam_wait_for_consumer.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.vcam_close.argtypes = [ctypes.c_void_p]
    lib.vcam_close.restype = None
    lib.vcam_error_string.argtypes = [ctypes.c_int]
    lib.vcam_error_string.restype = ctypes.c_char_p
    if path is None:
        _lib = lib
    return lib


class Camera:
    """A producer feeding one vcam camera.

    camera:     which camera (1-based), as installed with `vcam install --cameras N`
    fmt:        pixel format of the frames you send (see FORMATS)
    scale:      "fit" (pad), "fill" (crop) or "stretch" when your frame's aspect differs
    mirror:     flip left/right; flip: upside down
    background: 0xRRGGBB padding colour for "fit"
    fps:        pace for sleep_until_next_frame()
    colorspace: for YUV input: "bt601", "bt709", "bt601-full" or "bt709-full"
    """

    def __init__(self, camera=1, fmt="rgb", scale="fit", mirror=False, flip=False,
                 background=0x000000, fps=30.0, colorspace="bt601", dll=None):
        if fmt not in FORMATS:
            raise ValueError("fmt must be one of " + ", ".join(FORMATS))
        if scale not in _SCALES:
            raise ValueError("scale must be fit, fill or stretch")
        self._lib = _library(dll)
        self._fmt = FORMATS[fmt]
        self._colorspace = _COLORSPACES[colorspace]
        self.fps = float(fps)
        self._next = None
        options = _Options(ctypes.sizeof(_Options), int(camera), _SCALES[scale], int(bool(mirror)),
                           int(bool(flip)), int(background) & 0xFFFFFF)
        handle = ctypes.c_void_p()
        self._check(self._lib.vcam_open(ctypes.byref(options), ctypes.byref(handle)))
        self._handle = handle

    def _check(self, code):
        if code != 0:
            raise VcamError(code, self._lib.vcam_error_string(code).decode())

    def send(self, frame, width=None, height=None, timestamp=0):
        """Sends one frame (numpy array, or buffer + width/height)."""
        if self._handle is None:
            raise VcamError(-1, "camera is closed")
        f = _Frame()
        f.format = self._fmt
        f.colorspace = self._colorspace
        f.timestamp = int(timestamp)
        if hasattr(frame, "__array_interface__"):
            import numpy as np

            if frame.dtype != np.uint8:
                raise ValueError("frames must be uint8")
            # Rows may have any pitch, but the pixels inside a row must be contiguous.
            if frame.ndim < 2 or frame.strides[0] <= 0 or not frame[0].flags["C_CONTIGUOUS"]:
                frame = np.ascontiguousarray(frame)
            keep = frame
            h, w = self._frame_size(frame.shape)
            f.width, f.height = w, h
            self._fill_planes(f, frame.__array_interface__["data"][0], frame.strides[0], w, h)
        else:
            if width is None or height is None:
                raise ValueError("width and height are needed for raw buffers")
            view = memoryview(frame).cast("B")
            needed = _packed_size(self._fmt, int(width), int(height))
            if view.nbytes < needed:
                raise ValueError("buffer holds %d bytes, a %dx%d %s frame needs %d"
                                 % (view.nbytes, width, height, _FORMAT_NAMES[self._fmt], needed))
            array = ctypes.c_char * view.nbytes
            keep = array.from_buffer_copy(view) if view.readonly else array.from_buffer(view)
            f.width, f.height = int(width), int(height)
            self._fill_planes(f, ctypes.addressof(keep), 0, f.width, f.height)
        self._check(self._lib.vcam_send(self._handle, ctypes.byref(f)))
        del keep

    def _frame_size(self, shape):
        name = _FORMAT_NAMES[self._fmt]
        if name in ("rgb", "bgr", "rgba", "bgra"):
            channels = 3 if name in ("rgb", "bgr") else 4
            if len(shape) != 3 or shape[2] != channels:
                raise ValueError("%s frames must have shape (H, W, %d)" % (name, channels))
            return shape[0], shape[1]
        if name == "gray":
            if len(shape) != 2:
                raise ValueError("gray frames must have shape (H, W)")
            return shape[0], shape[1]
        if name in ("nv12", "i420"):
            if len(shape) != 2 or shape[0] % 3:
                raise ValueError("%s frames must have shape (H * 3 // 2, W)" % name)
            return shape[0] * 2 // 3, shape[1]
        if len(shape) != 2 or shape[1] % 2:
            raise ValueError("%s frames must have shape (H, W * 2)" % name)
        return shape[0], shape[1] // 2

    def _fill_planes(self, f, base, pitch, w, h):
        name = _FORMAT_NAMES[self._fmt]
        f.planes[0] = base
        f.pitches[0] = pitch
        if name == "nv12":
            luma = pitch or w
            f.planes[1] = base + luma * h
            f.pitches[1] = luma
        elif name == "i420":
            luma = pitch or w
            chroma = luma // 2 if pitch else (w + 1) // 2
            f.planes[1] = base + luma * h
            f.planes[2] = f.planes[1] + chroma * ((h + 1) // 2)
            f.pitches[1] = f.pitches[2] = chroma

    def status(self):
        """What the camera is doing: connected, consumer_active, width, height, format, fps, counters."""
        s = _Status()
        s.struct_size = ctypes.sizeof(_Status)
        self._check(self._lib.vcam_get_status(self._handle, ctypes.byref(s)))
        fps = s.fps_numerator / s.fps_denominator if s.fps_denominator else 0.0
        return {
            "connected": bool(s.connected),
            "consumer_active": bool(s.consumer_active),
            "width": s.width,
            "height": s.height,
            "format": _FORMAT_NAMES.get(s.format),
            "fps": fps,
            "frames_sent": s.frames_sent,
            "frames_delivered": s.frames_delivered,
            "frames_repeated": s.frames_repeated,
        }

    @property
    def width(self):
        """Width apps currently receive (0 before an app opened the camera)."""
        return self.status()["width"]

    @property
    def height(self):
        return self.status()["height"]

    def wait_for_consumer(self, timeout=None):
        """Blocks until an app streams from the camera. Returns False on timeout."""
        ms = -1 if timeout is None else int(timeout * 1000)
        code = self._lib.vcam_wait_for_consumer(self._handle, ms)
        if code == -6:
            return False
        self._check(code)
        return True

    def sleep_until_next_frame(self):
        """Sleeps so that a send/sleep loop runs at `fps`, without drifting."""
        period = 1.0 / self.fps
        now = time.perf_counter()
        if self._next is None or now - self._next > period:
            self._next = now
        self._next += period
        delay = self._next - time.perf_counter()
        if delay > 0:
            time.sleep(delay)

    def close(self):
        if getattr(self, "_handle", None) is not None:
            self._lib.vcam_close(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()
