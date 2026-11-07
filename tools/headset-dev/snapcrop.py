import zlib,struct,sys
# snapcrop.py OUT.png: the dashboard panel only (left eye, opaque panel area), no room image around it
d=open("/tmp/quest1-snap.ppm","rb").read()
i=d.index(b"255\n")+4; w=2880; px=d[i:]; s=2
x0,x1,y0,y1=420,1220,480,980
rows=[]
for y in range(y0,y1,s):
    row=bytearray(b"\0")
    for x in range(x0,x1,s):
        o=(y*w+x)*3; row+=px[o:o+3]
    rows.append(bytes(row))
def ch(t,b):
    c=struct.pack(">I",len(b))+t+b; return c+struct.pack(">I",zlib.crc32(t+b)&0xffffffff)
open(sys.argv[1],"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",(x1-x0)//s,(y1-y0)//s,8,2,0,0,0))+ch(b"IDAT",zlib.compress(b"".join(rows)))+ch(b"IEND",b""))
