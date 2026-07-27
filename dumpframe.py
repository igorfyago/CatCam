
import os
import ctypes, struct
from ctypes import wintypes
k = ctypes.WinDLL('kernel32', use_last_error=True)
OpenFileMappingW = k.OpenFileMappingW; OpenFileMappingW.restype = wintypes.HANDLE
OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
MapViewOfFile = k.MapViewOfFile; MapViewOfFile.restype = ctypes.c_void_p
MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
FILE_MAP_READ = 4
h = OpenFileMappingW(FILE_MAP_READ, False, r'Global\CatCam_SharedMem')
assert h, ctypes.get_last_error()
total = 24 + 1280*720*3//2
p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, total)
assert p, ctypes.get_last_error()
hdr = ctypes.string_at(p, 24)
w, hh, stride, fs, idx = struct.unpack('<IIIIQ', hdr)
print('dims', w, hh, 'stride', stride, 'frameSize', fs, 'frameIndex', idx)
fsz = w*hh*3//2
frame = ctypes.string_at(p+24, fsz)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'frame.nv12'),'wb').write(frame)
print('dumped', fsz)
