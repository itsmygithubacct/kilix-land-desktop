#!/usr/bin/env python3
"""kilix-mask, driven from here.

The region model, its PNG format and the rectangle decomposition all live
in third_party/kilix-mask as C.  This binds to the built shared library
rather than reimplementing any of it in Python, because a second
implementation of a file format is a second thing to keep in step, and the
one that drifts is always the one with fewer tests.

What stays on this side is everything that is about *this game* rather
than about masks: world.json, what a valid room is, and the walk-behind
mask format the engine reads at runtime.

Two coordinate spaces meet here and it is worth being precise about them.
world.json is in logical units (480x270) on a 6-unit grid, so 80x45 cells.
The plates are 1280x720.  A mask painted over a plate must therefore be
1280x720 with a cell of 16 - which is the same 80x45 grid, because
1280/16 == 480/6.  A cell boundary is a multiple of 6 logical and 16
plate, so the conversion between them is exact and no rect moves.
"""

import ctypes
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBRARY = os.path.join(REPO, "third_party", "kilix-mask", "build",
                       "libkilix-mask.so")
COMMAND = os.path.join(REPO, "third_party", "kilix-mask", "build",
                       "kilix-mask")

LOGICAL_W, LOGICAL_H = 480, 270
PLATE_W, PLATE_H = 1280, 720
LOGICAL_CELL = 6
PLATE_CELL = 16
COLS, ROWS = LOGICAL_W // LOGICAL_CELL, LOGICAL_H // LOGICAL_CELL

assert PLATE_W // PLATE_CELL == COLS and PLATE_H // PLATE_CELL == ROWS


class Rect(ctypes.Structure):
    """kmask_rect: source-pixel coordinates, always cell-aligned."""
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int),
                ("w", ctypes.c_int), ("h", ctypes.c_int)]


class MaskLibraryMissing(RuntimeError):
    """The submodule is present but has not been built."""


_lib = None


def library():
    """The shared library, loaded once.

    Built rather than installed: the submodule is pinned to a commit, and
    picking up whatever libkilix-mask happens to be on the system would
    silently edit masks with a different version of the format.
    """
    global _lib
    if _lib is not None:
        return _lib
    if not os.path.exists(LIBRARY):
        raise MaskLibraryMissing(
            f"{LIBRARY} is missing; run:\n"
            f"  git submodule update --init --recursive\n"
            f"  make -C third_party/kilix-mask")
    lib = ctypes.CDLL(LIBRARY)

    lib.kmask_create.argtypes = [ctypes.POINTER(ctypes.c_void_p),
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.kmask_create.restype = ctypes.c_bool
    lib.kmask_load.argtypes = [ctypes.POINTER(ctypes.c_void_p),
                               ctypes.c_char_p]
    lib.kmask_load.restype = ctypes.c_bool
    lib.kmask_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.kmask_save.restype = ctypes.c_bool
    lib.kmask_free.argtypes = [ctypes.c_void_p]
    lib.kmask_free.restype = None

    for name in ("kmask_source_width", "kmask_source_height", "kmask_cell",
                 "kmask_grid_width", "kmask_grid_height"):
        getattr(lib, name).argtypes = [ctypes.c_void_p]
        getattr(lib, name).restype = ctypes.c_int

    lib.kmask_get.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.kmask_get.restype = ctypes.c_uint8
    lib.kmask_set.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint8]
    lib.kmask_set.restype = None
    lib.kmask_fill_rect.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_uint8]
    lib.kmask_fill_rect.restype = None

    lib.kmask_region_set_name.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                          ctypes.c_char_p]
    lib.kmask_region_set_name.restype = ctypes.c_bool
    lib.kmask_region_set_attr.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                          ctypes.c_char_p, ctypes.c_char_p]
    lib.kmask_region_set_attr.restype = ctypes.c_bool
    lib.kmask_region_attr.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                      ctypes.c_char_p]
    lib.kmask_region_attr.restype = ctypes.c_char_p
    lib.kmask_region_set_color.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                           ctypes.c_uint32]
    lib.kmask_region_set_color.restype = None

    lib.kmask_expand.argtypes = [ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_uint8),
                                 ctypes.c_size_t]
    lib.kmask_expand.restype = ctypes.c_bool
    lib.kmask_import.argtypes = [ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.c_uint8),
                                 ctypes.c_size_t]
    lib.kmask_import.restype = ctypes.c_bool

    lib.kmask_decompose.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                    ctypes.POINTER(Rect),
                                    ctypes.POINTER(Rect), ctypes.c_size_t,
                                    ctypes.POINTER(ctypes.c_size_t)]
    lib.kmask_decompose.restype = ctypes.c_bool
    lib.kmask_apply.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                ctypes.POINTER(Rect), ctypes.POINTER(Rect),
                                ctypes.c_size_t]
    lib.kmask_apply.restype = ctypes.c_bool

    _lib = lib
    return lib


class Mask:
    """A kmask handle with a Python lifetime."""

    def __init__(self, handle):
        self._handle = handle

    @classmethod
    def create(cls, width, height, cell):
        handle = ctypes.c_void_p()
        if not library().kmask_create(ctypes.byref(handle), width, height,
                                      cell):
            raise RuntimeError(f"could not create a {width}x{height} mask")
        return cls(handle)

    @classmethod
    def load(cls, path):
        handle = ctypes.c_void_p()
        if not library().kmask_load(ctypes.byref(handle),
                                    str(path).encode()):
            raise RuntimeError(f"{path} is not a mask kilix-mask wrote")
        return cls(handle)

    def save(self, path):
        if not library().kmask_save(self._handle, str(path).encode()):
            raise RuntimeError(f"could not write {path}")

    def close(self):
        if self._handle:
            library().kmask_free(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    # -- geometry ------------------------------------------------------
    @property
    def cell(self):
        return library().kmask_cell(self._handle)

    @property
    def grid_size(self):
        return (library().kmask_grid_width(self._handle),
                library().kmask_grid_height(self._handle))

    @property
    def source_size(self):
        return (library().kmask_source_width(self._handle),
                library().kmask_source_height(self._handle))

    # -- contents ------------------------------------------------------
    def get(self, cx, cy):
        return library().kmask_get(self._handle, cx, cy)

    def set(self, cx, cy, region):
        library().kmask_set(self._handle, cx, cy, region)

    def fill_rect(self, x0, y0, x1, y1, region):
        library().kmask_fill_rect(self._handle, x0, y0, x1, y1, region)

    def expand(self):
        """One byte per source pixel, holding the region id."""
        width, height = self.source_size
        buffer = (ctypes.c_uint8 * (width * height))()
        if not library().kmask_expand(self._handle, buffer, len(buffer)):
            raise RuntimeError("expand failed")
        return bytes(buffer)

    def import_bytes(self, values):
        """Replace the map from one byte per source pixel."""
        width, height = self.source_size
        if len(values) != width * height:
            raise ValueError(f"expected {width * height} bytes, "
                             f"got {len(values)}")
        buffer = (ctypes.c_uint8 * len(values)).from_buffer_copy(bytes(values))
        if not library().kmask_import(self._handle, buffer, len(values)):
            raise RuntimeError("import failed")

    # -- regions -------------------------------------------------------
    def set_name(self, region, name):
        library().kmask_region_set_name(self._handle, region, name.encode())

    def set_color(self, region, rgb):
        library().kmask_region_set_color(self._handle, region, rgb)

    def set_attr(self, region, key, value):
        ok = library().kmask_region_set_attr(
            self._handle, region, key.encode(),
            None if value is None else str(value).encode())
        if not ok and value is not None:
            raise RuntimeError(f"could not set {key} on region {region}")

    def attr(self, region, key):
        value = library().kmask_region_attr(self._handle, region,
                                            key.encode())
        return value.decode() if value else None

    # -- rectangles ----------------------------------------------------
    def decompose(self, region, capacity=4096):
        """-> (bounds, [holes]) or (None, None) when nothing is painted.

        The count is asked for first so a shape over the buffer is a
        clear error rather than a silently short list of holes, which
        would decompose into a different room.
        """
        bounds = Rect()
        needed = ctypes.c_size_t(0)
        library().kmask_decompose(self._handle, region, ctypes.byref(bounds),
                                  None, 0, ctypes.byref(needed))
        holes = (Rect * max(1, needed.value))()
        ok = library().kmask_decompose(self._handle, region,
                                       ctypes.byref(bounds), holes,
                                       len(holes), ctypes.byref(needed))
        if not ok:
            return None, None
        return bounds, [holes[i] for i in range(needed.value)]

    def apply(self, region, bounds, holes):
        array = (Rect * max(1, len(holes)))()
        for index, hole in enumerate(holes):
            array[index] = hole
        if not library().kmask_apply(self._handle, region, ctypes.byref(bounds),
                                     array, len(holes)):
            raise RuntimeError("apply failed")
