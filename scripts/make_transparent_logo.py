#!/usr/bin/env python3
"""Convert the shipped opaque logo to transparent RGBA, without dependencies."""
from pathlib import Path
import struct, zlib
root=Path(__file__).resolve().parents[1]/"app/src/main/res/drawable"
source=root/"strato_mali_logo_separada_azul.png"
target=root/"strato_mali_logo_transparent.png"
raw=source.read_bytes()
assert raw.startswith(b"\x89PNG\r\n\x1a\n")
pos=8;header=None;parts=[]
while pos < len(raw):
    n=struct.unpack_from(">I",raw,pos)[0];typ=raw[pos+4:pos+8];blob=raw[pos+8:pos+8+n];pos+=n+12
    if typ==b"IHDR":header=struct.unpack(">IIBBBBB",blob)
    elif typ==b"IDAT":parts.append(blob)
    elif typ==b"IEND":break
w,h,depth,mode,compression,filtering,interlace=header
assert (depth,mode,compression,filtering,interlace)==(8,2,0,0,0),header
data=zlib.decompress(b"".join(parts)); stride=w*3;prev=bytearray(stride);offset=0;dst=bytearray()
for y in range(h):
    f=data[offset];offset+=1
    row=bytearray(data[offset:offset+stride]);offset+=stride
    for i in range(stride):
        a=row[i-3] if i>=3 else 0;b=prev[i];c=prev[i-3] if i>=3 else 0
        if f==1:v=a
        elif f==2:v=b
        elif f==3:v=(a+b)//2
        elif f==4:
            p=a+b-c;pa=abs(p-a);pb=abs(p-b);pc=abs(p-c)
            v=a if pa<=pb and pa<=pc else (b if pb<=pc else c)
        elif f==0:v=0
        else:raise ValueError("Unsupported PNG filter")
        row[i]=(row[i]+v)&255
    dst.append(0)
    for i in range(0,stride,3):
        r,g,b=row[i:i+3];bright=max(r,g,b)
        alpha=min(255,(bright*255+44)//90)
        if alpha:
            scale=255/alpha
            dst.extend((min(255,round(r*scale)),min(255,round(g*scale)),min(255,round(b*scale)),alpha))
        else:dst.extend((0,0,0,0))
    prev=row
def ck(t,p):return struct.pack(">I",len(p))+t+p+struct.pack(">I",zlib.crc32(t+p)&0xffffffff)
result=b"\x89PNG\r\n\x1a\n"+ck(b"IHDR",struct.pack(">IIBBBBB",w,h,8,6,0,0,0))+ck(b"IDAT",zlib.compress(dst,8))+ck(b"IEND",b"")
target.write_bytes(result)
print(f"{target.name}: {w}x{h}, RGBA {len(result)} bytes")
