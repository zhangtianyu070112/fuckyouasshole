import ctypes
libc = ctypes.cdll.msvcrt
x=ctypes.c_float()
y=ctypes.c_float()
z=ctypes.c_float()
w=ctypes.c_float()
ret = libc.sscanf(b'"bbox": [0.010, 0.115, 0.083, 0.150]}', b'"bbox": [%f, %f, %f, %f]', ctypes.byref(x), ctypes.byref(y), ctypes.byref(z), ctypes.byref(w))
print("ret:", ret, "x:", x.value, "y:", y.value)
