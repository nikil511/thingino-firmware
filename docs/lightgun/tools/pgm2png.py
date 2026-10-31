import sys, zlib, struct
def rd(p):
    d=open(p,'rb').read(); parts=d.split(b'\n',3); w,h=map(int,parts[1].split()); return w,h,parts[3]
def wr(p,w,h,px):
    raw=b''.join(b'\x00'+px[y*w:(y+1)*w] for y in range(h))
    def ch(t,b): return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,0,0,0,0))+ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b''))
for p in sys.argv[1:]:
    w,h,px=rd(p); wr(p[:-4]+'.png',w,h,px); print(p,w,h)
