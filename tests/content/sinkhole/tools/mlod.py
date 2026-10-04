import struct
def read(path):
    d=open(path,'rb').read(); p=0
    def u(fmt):
        nonlocal p; v=struct.unpack_from(fmt,d,p); p+=struct.calcsize(fmt); return v
    assert d[:4]==b'MLOD'; p=4; ver,n=u('<II'); lods=[]
    for li in range(n):
        sig=d[p:p+4]; p+=4; assert sig==b'SP3X',sig
        hs,v,np_,nn,nf,fl=u('<6i')
        pts=[u('<3fi') for _ in range(np_)]
        nrm=[u('<3f') for _ in range(nn)]
        faces=[]
        for _ in range(nf):
            tex=d[p:p+32].split(b'\0')[0].decode('latin1'); p+=32
            nv=u('<i')[0]; vs=[u('<2i2f') for _ in range(4)]; ff=u('<i')[0]
            faces.append((tex,nv,vs,ff))
        assert d[p:p+4]==b'TAGG'; p+=4; tags=[]
        while True:
            name=d[p:p+64].split(b'\0')[0].decode('latin1'); p+=64; sz=u('<i')[0]
            data=d[p:p+sz]; p+=sz; tags.append((name,data))
            if name=='#EndOfFile#': break
        res=u('<f')[0]
        lods.append(dict(hs=hs,ver=v,flags=fl,pts=pts,nrm=nrm,faces=faces,tags=tags,res=res))
    return dict(ver=ver,lods=lods,tail=d[p:])
def write(path,m):
    out=bytearray(b'MLOD')+struct.pack('<II',m.get('ver',0x101),len(m['lods']))
    for L in m['lods']:
        out+=b'SP3X'+struct.pack('<6i',L.get('hs',28),L.get('ver',0x100),len(L['pts']),len(L['nrm']),len(L['faces']),L.get('flags',0))
        for x,y,z,f in L['pts']: out+=struct.pack('<3fi',x,y,z,f)
        for x,y,z in L['nrm']: out+=struct.pack('<3f',x,y,z)
        for tex,nv,vs,ff in L['faces']:
            out+=tex.encode('latin1')[:31].ljust(32,b'\0')+struct.pack('<i',nv)
            for a,b,c,e in vs: out+=struct.pack('<2i2f',a,b,c,e)
            out+=struct.pack('<i',ff)
        out+=b'TAGG'
        for name,data in L['tags']: out+=name.encode('latin1').ljust(64,b'\0')+struct.pack('<i',len(data))+data
        out+=struct.pack('<f',L['res'])
    out+=m.get('tail',b'')
    open(path,'wb').write(out)
