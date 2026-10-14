"""Extract stored Umbrella Chronicles GX textures with Dolphin-compatible names."""
import argparse
import ctypes
import hashlib
import json
import os
import struct
import sys
import tempfile
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

U8 = b"\x55\xAA\x38\x2D"
TPL = b"\x00\x20\xAF\x30"
TILES = {0:(8,8,32), 1:(8,4,32), 2:(8,4,32), 3:(4,4,32),
         4:(4,4,32), 5:(4,4,32), 6:(4,4,64), 8:(8,8,32),
         9:(8,4,32), 10:(4,4,32), 14:(8,8,32)}


class Reader:
    def __init__(self, data):
        self.data = data

    def take(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data)-offset:
            raise ValueError(f"range {offset:#x}+{size:#x} outside {len(self.data):#x}-byte resource")
        return self.data[offset:offset+size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.take(offset, struct.calcsize(fmt)))

    def u32(self, offset):
        return self.unpack(">I", offset)[0]

    def string(self, offset, end=None):
        end = len(self.data) if end is None else end
        self.take(offset, end-offset)
        stop = self.data.find(b"\0", offset, end)
        if stop < 0:
            raise ValueError(f"unterminated resource name at {offset:#x}")
        return self.data[offset:stop].decode("utf-8", errors="replace")


def level_size(width, height, fmt):
    if not 1 <= width <= 1024 or not 1 <= height <= 1024 or fmt not in TILES:
        raise ValueError(f"unsupported GX descriptor {width}x{height}, format {fmt}")
    x, y, size = TILES[fmt]
    return ((width+x-1)//x)*((height+y-1)//y)*size


@dataclass
class Texture:
    source: str
    name: str
    offset: int
    width: int
    height: int
    format: int
    levels: int
    data: bytes
    palette_format: int = 0
    palette: bytes = b""

    def validate(self):
        level_size(self.width, self.height, self.format)
        if not 1 <= self.levels <= max(self.width, self.height).bit_length():
            raise ValueError("texture mip chain extends past 1x1")
        required = sum(level_size(max(1,self.width>>i),max(1,self.height>>i),self.format)
                       for i in range(self.levels))
        if len(self.data) != required:
            raise ValueError("truncated or oversized texture mip chain")
        if self.palette_format not in (0,1,2) or len(self.palette)%2 or len(self.palette)>32768:
            raise ValueError("invalid GX palette")
        if self.format in (8,9,10) and not self.palette:
            raise ValueError("indexed texture has no palette")


def image(reader, source, name, offset, width, height, fmt, levels, palette_format=0, palette=b"", end=None):
    level_size(width,height,fmt)
    if not 1 <= levels <= max(width,height).bit_length():
        raise ValueError("invalid mip count")
    size = sum(level_size(max(1,width>>i),max(1,height>>i),fmt) for i in range(levels))
    if end is not None and offset+size > end:
        raise ValueError("texture data exceeds its block")
    result = Texture(source,name,offset,width,height,fmt,levels,reader.take(offset,size),palette_format,palette)
    result.validate()
    return result


def u8_members(data,source="",errors=None):
    r = Reader(data)
    root, header_size, data_start = r.unpack(">3I",4)
    if root < 16:
        raise ValueError("invalid U8 root offset")
    kind, parent, count = r.unpack(">3I",root)
    if kind>>24 != 1 or parent or count < 1 or count > (len(data)-root)//12:
        raise ValueError("invalid U8 root/count")
    names = root+count*12
    names_end = root+header_size
    if names_end < names or data_start < names_end:
        raise ValueError("invalid U8 header/data boundary")
    r.take(root,header_size)
    if data_start>((len(data)+31)&~31):
        raise ValueError("U8 data start exceeds aligned file end")
    stack = [(count,0,"")]
    for index in range(1,count):
        while index >= stack[-1][0]:
            stack.pop()
        value, offset, size = r.unpack(">3I",root+index*12)
        name = r.string(names+(value&0xFFFFFF),names_end)
        path = stack[-1][2]+name
        if value>>24 == 1:
            if offset != stack[-1][1] or not index < size <= stack[-1][0]:
                raise ValueError("invalid U8 directory parent/range")
            stack.append((size,index,path+"/"))
        elif value>>24 == 0:
            try:
                if offset < data_start:
                    raise ValueError("U8 file overlaps metadata")
                member=b"" if size==0 and offset<=((len(data)+31)&~31) else r.take(offset,size)
            except ValueError as error:
                if errors is None:
                    raise
                errors.append({"source":source+"::"+path,"error":str(error)})
                continue
            yield path,member
        else:
            raise ValueError("unknown U8 node type")


def tpl_textures(data, source):
    r = Reader(data)
    count, table = r.unpack(">2I",4)
    if not count or table < 12:
        raise ValueError("invalid TPL descriptor table")
    r.take(table,count*8)
    for index in range(count):
        header, pal = r.unpack(">2I",table+index*8)
        if header < 12:
            raise ValueError("invalid TPL image header")
        h,w,fmt,offset = r.unpack(">HHII",header)
        r.take(header,36)
        levels = r.take(header+34,1)[0]+1
        palette, palette_format = b"",0
        if pal:
            entries, _, _, palette_format, palette_offset = r.unpack(">HBBII",pal)
            if not entries or entries > 16384:
                raise ValueError("invalid TPL palette count")
            palette = r.take(palette_offset,entries*2)
        yield image(r,source,f"image_{index}",offset,w,h,fmt,levels,palette_format,palette)


def brres_group(reader, offset):
    size,count = reader.unpack(">2I",offset)
    if size < 8+16*(count+1):
        raise ValueError("truncated BRRES dictionary")
    reader.take(offset,size)
    for index in range(count):
        name, target = reader.unpack(">2I",offset+8+16*(index+1)+8)
        yield reader.string(offset+name),offset+target


def brres_textures(data, source):
    r = Reader(data)
    bom,version,size,root,count = r.unpack(">HHIHH",4)
    if bom != 0xFEFF or version != 0 or size != len(data) or count < 1 or r.take(root,4)!=b"root":
        raise ValueError("unsupported BRRES header")
    r.take(root,r.u32(root+4))
    groups = dict(brres_group(r,root+8))
    palettes = {}
    for name,pos in brres_group(r,groups["Palettes(NW4R)"]) if "Palettes(NW4R)" in groups else ():
        if r.take(pos,4)!=b"PLT0" or r.u32(pos+8) not in (1,3):
            raise ValueError("unsupported PLT0 block")
        block_end = pos+r.u32(pos+4)
        r.take(pos,block_end-pos)
        fmt = r.u32(pos+24)
        entries = r.unpack(">H",pos+28)[0]
        offset = pos+r.u32(pos+16)
        if entries>16384 or not entries or offset+entries*2>block_end:
            raise ValueError("invalid PLT0 data range")
        palettes[name]=(fmt,r.take(offset,entries*2))
    if "Textures(NW4R)" not in groups:
        return
    for name,pos in brres_group(r,groups["Textures(NW4R)"]):
        if r.take(pos,4)!=b"TEX0" or r.u32(pos+8) not in (1,3):
            raise ValueError("unsupported TEX0 block")
        if r.unpack(">i",pos+12)[0] != -pos:
            raise ValueError("invalid TEX0 parent offset")
        block_end=pos+r.u32(pos+4)
        r.take(pos,block_end-pos)
        w,h,fmt,levels=r.unpack(">HHII",pos+28)
        palette_format,palette=palettes.get(name,(0,b""))
        yield image(r,source,name,pos+r.u32(pos+16),w,h,fmt,levels,palette_format,palette,block_end)


def font_textures(data,source):
    r=Reader(data)
    bom,version,size,pos,count=r.unpack(">HHIHH",4)
    if bom!=0xFEFF or version!=0x104 or size!=len(data):
        raise ValueError("unsupported RFNT header")
    for _ in range(count):
        magic,block_size=r.unpack(">4sI",pos)
        if block_size<8:
            raise ValueError("invalid font block size")
        r.take(pos,block_size)
        if magic==b"TGLP":
            sheet_size,sheets,fmt,_,_,w,h,offset=r.unpack(">I6HI",pos+12)
            if not sheets or sheet_size != level_size(w,h,fmt):
                raise ValueError("invalid font sheet size/count")
            r.take(offset,sheets*sheet_size)
            for index in range(sheets):
                yield image(r,source,f"sheet_{index}",offset+index*sheet_size,w,h,fmt,1,end=pos+block_size)
        pos+=block_size


def decompress_lz10(data):
    r=Reader(data)
    if r.take(0,1)!=b"\x10":
        raise ValueError("unsupported Nintendo LZ compression")
    size=int.from_bytes(r.take(1,3),"little")
    if not 1<=size<=256*1024*1024:
        raise ValueError("invalid LZ10 output size")
    output=bytearray()
    pos=4
    while len(output)<size:
        flags=r.take(pos,1)[0]
        pos+=1
        for bit in range(7,-1,-1):
            if len(output)==size:
                break
            if flags&(1<<bit):
                first,second=r.take(pos,2)
                pos+=2
                count=(first>>4)+3
                distance=((first&15)<<8|second)+1
                if distance>len(output) or count>size-len(output):
                    raise ValueError("invalid LZ10 back-reference")
                for _ in range(count):
                    output.append(output[-distance])
            else:
                output.extend(r.take(pos,1))
                pos+=1
    return bytes(output)


def effect_textures(data,source):
    r=Reader(data)
    bom,version,size,section,sections=r.unpack(">HHIHH",4)
    if bom!=0xFEFF or version!=7 or size!=len(data) or section!=16 or sections!=1 or r.take(section,4)!=b"REFT":
        raise ValueError("unsupported REFT header")
    table=24+r.u32(24)
    table_size,count,_=r.unpack(">IHH",table)
    r.take(table,table_size)
    pos=table+8
    for _ in range(count):
        name_size=r.unpack(">H",pos)[0]
        if not name_size or pos+10+name_size>table+table_size:
            raise ValueError("invalid REFT name/table range")
        name=r.string(pos+2,pos+2+name_size)
        offset,length=r.unpack(">II",pos+2+name_size)
        header=table+offset
        r.take(header,32+length)
        w,h,data_size,fmt,pal_fmt,colors,pal_size,mips=r.unpack(">HHIBBHIB",header+4)
        if data_size+pal_size!=length or pal_size!=colors*2:
            raise ValueError("REFT image/palette size mismatch")
        palette=r.take(header+32+data_size,pal_size)
        tex=image(r,source,name,header+32,w,h,fmt,mips+1,pal_fmt,palette,header+32+data_size)
        if len(tex.data)!=data_size:
            raise ValueError("REFT encoded image size mismatch")
        yield tex
        pos+=10+name_size


def textures(data,source,depth=0,inventory=None,errors=None):
    if depth>32:
        raise ValueError("container nesting exceeds 32")
    magic=data[:4]
    if inventory is not None:
        inventory[magic.hex()]+=1
    if magic==U8:
        for name,member in u8_members(data,source,errors):
            member_source=source+"::"+name
            try:
                yield from textures(member,member_source,depth+1,inventory,errors)
            except (ValueError,struct.error) as error:
                if errors is None:
                    raise
                errors.append({"source":member_source,"error":str(error)})
    elif magic==TPL:
        yield from tpl_textures(data,source)
    elif magic==b"bres":
        yield from brres_textures(data,source)
    elif magic==b"RFNT":
        yield from font_textures(data,source)
    elif magic==b"REFT":
        yield from effect_textures(data,source)
    elif magic==b"IMD5":
        r=Reader(data)
        payload=r.take(32,r.u32(4))
        if hashlib.md5(payload).digest()!=r.take(16,16):
            raise ValueError("IMD5 payload checksum mismatch")
        yield from textures(payload,source+"::IMD5",depth+1,inventory,errors)
    elif magic==b"LZ77":
        yield from textures(decompress_lz10(data[4:]),source+"::LZ10",depth+1,inventory,errors)
    elif magic in (b"Yaz0",b"Yay0") or Path(source).suffix.lower() in (".tpl",".brres",".brfnt",".tex0",".plt0",".breft"):
        raise ValueError(f"unsupported texture/container signature {magic.hex()}")
    else:
        # Embedded DOL/banner resources have no outer texture container.
        if Path(source).suffix.lower() not in (".dol",".bnr"):
            return
        for signature in (U8,TPL,b"bres",b"RFNT"):
            pos=0
            while (pos:=data.find(signature,pos))>=0:
                r=Reader(data)
                if signature in (b"bres",b"RFNT") and pos+16<=len(data) and data[pos+4:pos+6]==b"\xFE\xFF":
                    size=r.u32(pos+8)
                    yield from textures(r.take(pos,size),f"{source}@{pos:#x}",depth+1,inventory,errors)
                elif signature in (U8,TPL) and pos+12<=len(data):
                    # A magic constant in machine code is not a serialized header.
                    table=r.u32(pos+4 if signature==U8 else pos+8)
                    if 12<=table<len(data)-pos:
                        yield from textures(data[pos:],f"{source}@{pos:#x}",depth+1,inventory,errors)
                        if signature==U8:
                            return  # Its table owns nested resources, including compressed bytes.
                pos+=4


def xxhash64(data):
    mask=(1<<64)-1
    p1,p2,p3,p4,p5=11400714785074694791,14029467366897019727,1609587929392839161,9650029242287828579,2870177450012600261
    def rotate(v,n):
        return ((v<<n)|(v>>(64-n)))&mask
    def step(v,word):
        return (rotate((v+word*p2)&mask,31)*p1)&mask
    size=len(data)
    pos=0
    if size>=32:
        values=[(p1+p2)&mask,p2,0,(-p1)&mask]
        while pos<=size-32:
            words=struct.unpack_from("<4Q",data,pos)
            values=[step(v,w) for v,w in zip(values,words)]
            pos+=32
        value=sum(rotate(v,n) for v,n in zip(values,(1,7,12,18)))&mask
        for v in values:
            value=((value^step(0,v))*p1+p4)&mask
    else:
        value=p5
    value=(value+size)&mask
    while pos<=size-8:
        value=(rotate(value^step(0,struct.unpack_from("<Q",data,pos)[0]),27)*p1+p4)&mask
        pos+=8
    if pos<=size-4:
        value=(rotate(value^(struct.unpack_from("<I",data,pos)[0]*p1&mask),23)*p2+p3)&mask
        pos+=4
    for byte in data[pos:]:
        value=rotate(value^(byte*p5&mask),11)*p1&mask
    value^=value>>33
    value=value*p2&mask
    value^=value>>29
    value=value*p3&mask
    return value^(value>>32)


def texture_name(texture,mipmapped):
    base=texture.data[:level_size(texture.width,texture.height,texture.format)]
    palette_hash=""
    if texture.format in (8,9,10):
        if texture.format==8:
            indexes=[v for byte in base for v in (byte>>4,byte&15)]
        elif texture.format==9:
            indexes=base
        else:
            indexes=[v[0]&0x3FFF for v in struct.iter_unpack(">H",base)]
        low,high=min(indexes),max(indexes)
        if (high+1)*2>len(texture.palette):
            # Padding may address unloaded TLUT memory; its runtime hash is unknowable offline.
            palette_hash="_$"
        else:
            palette_hash=f"_{xxhash64(texture.palette[low*2:(high+1)*2]):016x}"
    return f"tex1_{texture.width}x{texture.height}{'_m' if mipmapped else ''}_{xxhash64(base):016x}{palette_hash}_{texture.format}"


class Decoder:
    def __init__(self,path):
        self.dll=ctypes.CDLL(str(Path(path).resolve()))
        self.fn=self.dll.uc_decode_texture
        self.fn.argtypes=[ctypes.c_uint32]*3+[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,
                         ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,
                         ctypes.c_void_p,ctypes.c_size_t]
        self.fn.restype=ctypes.c_int

    def decode(self,texture,*,base_only=False):
        texture.validate()
        offset=0
        result=[]
        for index in range(1 if base_only else texture.levels):
            w,h=max(1,texture.width>>index),max(1,texture.height>>index)
            size=level_size(w,h,texture.format)
            raw=ctypes.create_string_buffer(texture.data[offset:offset+size])
            palette=ctypes.create_string_buffer(texture.palette)
            output=ctypes.create_string_buffer(w*h*4)
            error=ctypes.create_string_buffer(256)
            if self.fn(w,h,texture.format,raw,size,texture.palette_format,palette,len(texture.palette),output,len(output),error,len(error)):
                raise ValueError(error.value.decode("utf-8",errors="replace"))
            result.append(output.raw)
            offset+=size
        return result


def png_bytes(width,height,rgba):
    if width<1 or height<1 or len(rgba)!=width*height*4:
        raise ValueError("invalid PNG dimensions/data")
    def chunk(kind,data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data))
    scanlines=b"".join(b"\0"+rgba[y*width*4:(y+1)*width*4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">2I5B",width,height,8,6,0,0,0))+
            chunk(b"IDAT",zlib.compress(scanlines))+chunk(b"IEND",b""))


def write_verified(path,data,*,replace=False):
    path=Path(path)
    if path.exists() and not replace:
        if path.read_bytes()!=data:
            raise ValueError(f"existing output differs; preserved: {path}")
        return
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix=".extract-",dir=path.parent)
    try:
        with os.fdopen(fd,"wb") as out:
            out.write(data)
        if replace:
            os.replace(name,path)
        else:
            try:
                os.link(name,path)
            except FileExistsError:
                if path.read_bytes()!=data:
                    raise ValueError(f"concurrent output differs; preserved: {path}")
    finally:
        Path(name).unlink(missing_ok=True)


def extract(input_path,output,decoder,*,full=False):
    input_path=Path(input_path).resolve()
    output=Path(output).resolve()
    if input_path.is_file():
        paths=[input_path]
        root=input_path.parent
    elif input_path.is_dir():
        if output==input_path or input_path in output.parents:
            raise ValueError("output must be outside the input tree")
        paths=sorted(p for p in input_path.rglob("*") if p.is_file())
        root=input_path
    else:
        raise ValueError(f"input does not exist: {input_path}")
    inventory=Counter()
    manifest={"schema":1,"input":str(input_path),"textures":[],"errors":[],"files":len(paths)}
    unique={}
    base_groups={}
    existing_bases={}
    if not full:
        # Reuse previously chosen filenames when rerunning a cleaned dump.
        for path in sorted(output.glob("tex1_*.png")):
            if "_m_" not in path.name and "_mip" not in path.name:
                existing_bases.setdefault(hashlib.sha256(path.read_bytes()).digest(),path.name)
    png_files=set()
    for index,path in enumerate(paths,1):
        source=path.relative_to(root).as_posix()
        error_start=len(manifest["errors"])
        try:
            data=path.read_bytes()
            for tex in textures(data,source,inventory=inventory,errors=manifest["errors"]):
                entry={"source":tex.source,"name":tex.name,"offset":tex.offset,
                    "width":tex.width,"height":tex.height,"format":tex.format,"levels":tex.levels,
                    "palette_format":tex.palette_format if tex.palette else None,"files":[]}
                manifest["textures"].append(entry)
                try:
                    identity=(tex.width,tex.height,tex.format,tex.levels,tex.palette_format,
                              hashlib.sha256(tex.data+tex.palette).digest())
                    if identity in unique:
                        entry.update(unique[identity])
                        continue
                    names=[texture_name(tex,False),texture_name(tex,True)]
                    decoded=decoder.decode(tex,base_only=not full)
                    if not full:
                        entry["original_files"]=[name+(f"_mip{level}" if level else "")+".png"
                            for level in range(tex.levels) for name in names]
                    for level,rgba in enumerate(decoded):
                        content=png_bytes(max(1,tex.width>>level),max(1,tex.height>>level),rgba)
                        export_names=names
                        if not full:
                            # Deterministic PNG encoding includes dimensions and every RGBA byte.
                            pixels=hashlib.sha256(content).digest()
                            filename=existing_bases.get(pixels,names[0]+".png")
                            group=base_groups.setdefault(pixels,{"file":filename,"width":tex.width,
                                "height":tex.height,"aliases":{filename}})
                            group["aliases"].update(name+".png" for name in names)
                            export_names=[group["file"][:-4]]
                        for name in export_names:
                            filename=name+(f"_mip{level}" if level else "")+".png"
                            entry["files"].append(filename)
                            try:
                                write_verified(output/filename,content)
                                png_files.add(filename)
                            except (OSError,ValueError) as error:
                                entry.setdefault("failed_files",[]).append(filename)
                                manifest["errors"].append({"source":tex.source,"error":str(error)})
                    if "failed_files" not in entry:
                        unique[identity]={"files":entry["files"]}
                        if not full:
                            unique[identity]["original_files"]=entry["original_files"]
                except (OSError,ValueError,struct.error) as error:
                    entry["error"]=str(error)
                    manifest["errors"].append({"source":tex.source,"error":str(error)})
        except (OSError,ValueError,struct.error) as error:
            manifest["errors"].append({"source":source,"error":str(error)})
        for failure in manifest["errors"][error_start:]:
            print(f"ERROR {failure['source']}: {failure['error']}",file=sys.stderr,flush=True)
        if index%100==0:
            count=len(unique) if full else len(base_groups)
            print(f"[{index}/{len(paths)}] {len(manifest['textures'])} textures, {count} unique",flush=True)
    manifest["signatures"]=dict(sorted(inventory.items()))
    manifest["export"]="full" if full else "unique_base_levels"
    manifest["unique_textures"]=len(unique) if full else len(base_groups)
    manifest["encoded_resources"]=len(unique)
    manifest["png_files"]=len(png_files)
    manifest["palette_wildcards"]=sum(any("_$_" in f for f in t.get("original_files",t["files"])) for t in manifest["textures"])
    manifest["limits"]=["Stored textures only; runtime-generated EFB copies and video frames are not disc textures.",
                        "Both sampler mip flags emitted: the resource alone does not identify the runtime filter."]
    if not full:
        manifest["base_levels"]=[{**group,"aliases":sorted(group["aliases"])}
            for group in sorted(base_groups.values(),key=lambda g:g["file"])]
        manifest["original_png_files"]=len({name for t in manifest["textures"] for name in t.get("original_files",[])})
        manifest["limits"][1]="Only unique base-level PNGs emitted; original sampler names and mip references are metadata."
    if not manifest["textures"]:
        manifest["errors"].append({"source":str(input_path),"error":"no supported textures found"})
    write_verified(output/"extractor_manifest.json",(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n").encode("utf-8"),replace=True)
    return manifest


def main():
    here=Path(__file__).resolve().parent
    installed=here.parent
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input",type=Path,default=installed/"game/DATA",
                        help="extracted game directory, archive or texture container")
    parser.add_argument("--output",type=Path,help="default: TexturesDump/RBUE08; --full: TexturesDumpFull/RBUE08")
    parser.add_argument("--decoder",type=Path,default=here/"uc_texture_decode.dll")
    parser.add_argument("--full",action="store_true",help="export all mip levels and both Dolphin sampler names")
    args=parser.parse_args()
    output=args.output or installed/("TexturesDumpFull/RBUE08" if args.full else "TexturesDump/RBUE08")
    try:
        report=extract(args.input,output,Decoder(args.decoder),full=args.full)
    except (OSError,ValueError) as error:
        parser.exit(2,f"Texture extraction failed: {error}\n")
    print(f"{len(report['textures'])} texture resources, {report['unique_textures']} unique, "
          f"{report['png_files']} PNGs, {len(report['errors'])} errors. Output: {output}")
    return 1 if report["errors"] else 0


if __name__=="__main__":
    raise SystemExit(main())
