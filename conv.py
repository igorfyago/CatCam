
import os
from PIL import Image
w,h = 720,1280
d = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'frame.nv12'),'rb').read()
y = d[:w*h]
uv = d[w*h:]
img_y = Image.frombytes('L',(w,h),y)
# NV12: deinterleave U,V, upsample 2x
u = bytearray(); v = bytearray()
for i in range(0,len(uv),2): u.append(uv[i]); v.append(uv[i+1])
img_u = Image.frombytes('L',(w//2,h//2),bytes(u)).resize((w,h))
img_v = Image.frombytes('L',(w//2,h//2),bytes(v)).resize((w,h))
img = Image.merge('YCbCr',(img_y,img_u,img_v)).convert('RGB')
img.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'frame.png'))
print('saved')
