"""Behavior fixtures for offline texture extraction; no game data required."""
import importlib.util
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
DLL = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / "build/tools/texture_extract/uc_texture_decode.dll"


def tpl(fmt=6, width=1, height=1, raw=None, palette=b"", palette_format=0, levels=1):
    raw = raw if raw is not None else bytes([17, 34]) + bytes(30) + bytes([51, 68]) + bytes(30)
    data = bytearray(128)
    struct.pack_into(">5I", data, 0, 0x20AF30, 1, 12, 20, 56 if palette else 0)
    struct.pack_into(">HH7I4B", data, 20, height, width, fmt, 128, 0, 0, 1, 1, 0, 0, 0, levels-1, 0)
    if palette:
        struct.pack_into(">HBBII", data, 56, len(palette)//2, 0, 0, palette_format, 128+len(raw))
    return bytes(data) + raw + palette


def u8(members):
    names = b"\0" + b"".join(name.encode() + b"\0" for name, _ in members)
    start = 32 + 12*(len(members)+1) + len(names)
    result = bytearray(start)
    struct.pack_into(">4I", result, 0, 0x55AA382D, 32, start-32, start)
    struct.pack_into(">3I", result, 32, 0x01000000, 0, len(members)+1)
    name_offset = 1
    for index, (name, contents) in enumerate(members, 1):
        struct.pack_into(">3I", result, 32+12*index, name_offset, len(result), len(contents))
        result.extend(contents)
        name_offset += len(name.encode())+1
    result[32+12*(len(members)+1):start] = names
    return bytes(result)


def brres():
    data = bytearray(320)
    struct.pack_into(">4sHHIHH", data, 0, b"bres", 0xFEFF, 0, len(data), 16, 2)
    data[16:20] = b"root"
    struct.pack_into(">I", data, 20, 128)
    struct.pack_into(">2I", data, 24, 40, 1)
    struct.pack_into(">2I", data, 56, 264, 72)
    struct.pack_into(">2I", data, 96, 40, 1)
    struct.pack_into(">2I", data, 128, 208, 64)
    struct.pack_into(">4sIII3IHHIIffI", data, 160, b"TEX0", 96, 1, 0xFFFFFF60, 64, 144, 0, 1, 1, 1, 1, 0, 0, 0)
    data[288:303] = b"Textures(NW4R)\0"
    data[304:309] = b"test\0"
    data[224:256] = bytes([73])*32
    return bytes(data)


class ExtractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = ROOT / "tools/texture_extract/extract_textures.py"
        assert path.exists(), "offline texture extractor missing"
        spec = importlib.util.spec_from_file_location("extract_textures", path)
        cls.mod = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.mod
        spec.loader.exec_module(cls.mod)

    def test_xxhash_reference_vectors(self):
        for data, expected in [(b"", 0xEF46DB3751D8E999), (b"a", 0xD24EC4F1A98C6E5B), (b"abc", 0x44BC2CF5AD770999)]:
            self.assertEqual(self.mod.xxhash64(data), expected)

    def test_nested_archive_provenance_and_rgba(self):
        images = list(self.mod.textures(u8([("inner.arc", u8([("../texture.tpl", tpl())]))]), "outer.arc"))
        self.assertEqual(len(images), 1)
        self.assertIn("inner.arc", images[0].source)
        pixels = self.mod.Decoder(DLL).decode(images[0])
        self.assertEqual(pixels, [bytes([34, 51, 68, 17])])

    def test_tpl_palette_and_mip_levels(self):
        image, = self.mod.textures(tpl(8, 8, 8, bytes([0x11])*64, b"\0\0\x12\xC4", levels=2), "indexed.tpl")
        self.assertEqual(self.mod.Decoder(DLL).decode(image), [bytes([196,196,196,18])*64, bytes([196,196,196,18])*16])

    def test_brres_tex0(self):
        image, = self.mod.textures(brres(), "model.brres")
        self.assertEqual((image.name, image.width, image.height, image.format), ("test",1,1,1))
        self.assertEqual(self.mod.Decoder(DLL).decode(image), [bytes([73])*4])

    def test_font_sheets(self):
        data = bytearray(112)
        struct.pack_into(">4sHHIHH",data,0,b"RFNT",0xFEFF,0x104,112,16,1)
        struct.pack_into(">4sI4B I6H I",data,16,b"TGLP",96,0,0,0,0,32,2,1,1,1,1,1,48)
        data[48:80]=bytes([11])*32
        data[80:112]=bytes([22])*32
        images=list(self.mod.textures(bytes(data),"font.brfnt"))
        self.assertEqual([self.mod.Decoder(DLL).decode(t)[0] for t in images], [bytes([11])*4,bytes([22])*4])

    def test_banner_imd5_lz10_and_corruption(self):
        raw=u8([("banner.tpl",tpl())])
        payload=b"LZ77\x10"+len(raw).to_bytes(3,"little")
        payload+=b"".join(b"\0"+raw[i:i+8] for i in range(0,len(raw),8))
        wrapped=b"IMD5"+len(payload).to_bytes(4,"big")+bytes(8)+hashlib.md5(payload).digest()+payload
        image,=self.mod.textures(wrapped,"banner.bin")
        self.assertEqual(self.mod.Decoder(DLL).decode(image),[bytes([34,51,68,17])])
        embedded=bytes(64)+u8([("banner.bin",wrapped)])
        self.assertEqual(len(list(self.mod.textures(embedded,"opening.bnr"))),1)
        with self.assertRaises(ValueError):
            list(self.mod.textures(wrapped[:-1]+b"x","banner.bin"))
        with self.assertRaises(ValueError):
            list(self.mod.textures(b"LZ77\x10\x03\0\0\x80\0\0","broken.bin"))

    def test_effect_texture_table_palette(self):
        data=bytearray(128)
        struct.pack_into(">4sHHIHH",data,0,b"REFT",0xFEFF,7,len(data),16,1)
        struct.pack_into(">4sII",data,16,b"REFT",len(data)-24,40)
        struct.pack_into(">IHH",data,64,32,1,0)
        struct.pack_into(">H4sII",data,72,4,b"tex\0",32,36)
        struct.pack_into(">IHHIBBHIBBBBfI",data,96,0,1,1,32,8,0,2,4,0,0,0,0,0,0)
        data.extend(bytes([0x11])*32+b"\0\0\x12\xC4")
        struct.pack_into(">I",data,8,len(data))
        struct.pack_into(">I",data,20,len(data)-24)
        image,=self.mod.textures(bytes(data),"effects.breft")
        self.assertEqual(self.mod.Decoder(DLL).decode(image),[bytes([196,196,196,18])])

    def test_corrupt_offsets_and_counts_rejected(self):
        for data in [tpl()[:140], u8([("x",tpl())])[:50], tpl(7), tpl(levels=12)]:
            with self.assertRaises(ValueError):
                list(self.mod.textures(data, "broken"))

    def test_missing_palette_rejected(self):
        with self.assertRaises(ValueError):
            list(self.mod.textures(tpl(8,raw=bytes(32)), "missing.tpl"))

    def test_empty_archives_and_files_allow_unstored_alignment(self):
        data=bytearray(u8([]))
        struct.pack_into(">I",data,12,64)
        self.assertEqual(list(self.mod.textures(bytes(data),"empty.arc")),[])
        data=bytearray(u8([("empty",b"")]))
        struct.pack_into(">I",data,48,(len(data)+31)&~31)
        self.assertEqual(list(self.mod.textures(bytes(data),"empty-file.arc")),[])

    def test_missing_padding_palette_uses_dolphin_wildcard(self):
        raw=b"\0\0"+b"\xff\xff"*15
        image,=self.mod.textures(tpl(10,raw=raw,palette=b"\x12\xC4"),"edge.tpl")
        self.assertIn("_$_10",self.mod.texture_name(image,False))
        self.assertEqual(self.mod.Decoder(DLL).decode(image),[bytes([196,196,196,18])])

    def test_dolphin_palette_hash_ignores_unused_entries(self):
        image, = self.mod.textures(tpl(8,raw=bytes([0x11])*32,palette=b"\0\0\x12\xC4"), "palette.tpl")
        first = self.mod.texture_name(image, False)
        image.palette = b"\xFF\xFF"+image.palette[2:]+b"\xAB\xCD"
        self.assertEqual(self.mod.texture_name(image,False), first)
        self.assertTrue(self.mod.texture_name(image,True).startswith("tex1_1x1_m_"))

    def test_png_preserves_alpha_and_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"image.png"
            encoded=self.mod.png_bytes(1,1,bytes([34,51,68,17]))
            self.mod.write_verified(path, encoded)
            data=path.read_bytes()
            self.assertEqual(data[:8],b"\x89PNG\r\n\x1a\n")
            pos=8
            while data[pos+4:pos+8]!=b"IDAT":
                pos += int.from_bytes(data[pos:pos+4],"big")+12
            size=int.from_bytes(data[pos:pos+4],"big")
            self.assertEqual(zlib.decompress(data[pos+8:pos+8+size]),bytes([0,34,51,68,17]))
            self.mod.write_verified(path,encoded)
            with self.assertRaises(ValueError):
                self.mod.write_verified(path,encoded+b"changed")
            self.assertEqual(path.read_bytes(),encoded)

    def test_edited_first_texture_preserves_provenance_and_exports_second(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            archive=root/"two.arc"
            output=root/"output"
            raw=bytes([73])*64
            archive.write_bytes(u8([("first.tpl",tpl()),("second.tpl",tpl(raw=raw))]))
            decoder=self.mod.Decoder(DLL)
            before=self.mod.extract(archive,output,decoder)
            first=output/before["textures"][0]["files"][0]
            first.write_bytes(b"edited texture pack")
            after=self.mod.extract(archive,output,decoder)
            self.assertEqual(len(after["textures"]),2)
            self.assertEqual(first.read_bytes(),b"edited texture pack")
            self.assertTrue(after["errors"])
            self.assertIn("first.tpl",after["errors"][0]["source"])
            self.assertTrue(all((output/f).exists() for f in after["textures"][1]["files"]))

    def test_corrupt_archive_member_does_not_skip_valid_sibling(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            archive=root/"members.arc"
            for bad_offset in (False,True):
                data=bytearray(u8([("broken.tpl",tpl()[:24]),("valid.tpl",tpl())]))
                if bad_offset:
                    struct.pack_into(">I",data,48,0xFFFFFFF0)
                archive.write_bytes(data)
                report=self.mod.extract(archive,root/str(bad_offset),self.mod.Decoder(DLL))
                self.assertEqual(len(report["textures"]),1)
                self.assertEqual(len(report["errors"]),1)
                self.assertIn("broken.tpl",report["errors"][0]["source"])
                self.assertIn("valid.tpl",report["textures"][0]["source"])

    def test_default_exports_one_base_with_all_original_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            source=root/"mips.tpl"
            source.write_bytes(tpl(1,4,4,bytes([11])*32+bytes([22])*32+bytes([33])*32,levels=3))
            report=self.mod.extract(source,root/"output",self.mod.Decoder(DLL))
            self.assertEqual(report["errors"],[])
            self.assertEqual(report["png_files"],1)
            self.assertEqual(len(list((root/"output").glob("*.png"))),1)
            entry=report["textures"][0]
            self.assertEqual(entry["levels"],3)
            self.assertEqual(len(entry["original_files"]),6)
            self.assertFalse("_m_" in entry["files"][0])
            self.assertEqual(len(report["base_levels"][0]["aliases"]),2)

    def test_equal_pixels_deduplicated_despite_different_encoded_padding(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            source=root/"padding.arc"
            source.write_bytes(u8([("one.tpl",tpl(1,raw=bytes([73])+bytes(31))),
                                  ("two.tpl",tpl(1,raw=bytes([73])+bytes([255])*31))]))
            report=self.mod.extract(source,root/"output",self.mod.Decoder(DLL))
            self.assertEqual(report["errors"],[])
            self.assertEqual(report["png_files"],1)
            self.assertEqual(report["textures"][0]["files"],report["textures"][1]["files"])
            self.assertEqual(len(report["base_levels"][0]["aliases"]),4)

    def test_dimensions_and_alpha_are_part_of_base_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            source=root/"different.arc"
            raw=bytes([73])*64
            alpha=bytes([18,34])+bytes(30)+bytes([51,68])+bytes(30)
            source.write_bytes(u8([("wide.tpl",tpl(1,2,1,raw[:32])),
                                  ("tall.tpl",tpl(1,1,2,raw[:32])),
                                  ("alpha17.tpl",tpl()),("alpha18.tpl",tpl(raw=alpha))]))
            report=self.mod.extract(source,root/"output",self.mod.Decoder(DLL))
            self.assertEqual(report["errors"],[])
            self.assertEqual(report["png_files"],4)

    def test_existing_canonical_base_name_is_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            first=tpl(1,raw=bytes([73])+bytes(31))
            second=tpl(1,raw=bytes([73])+bytes([255])*31)
            second_tex,=self.mod.textures(second,"two.tpl")
            canonical=self.mod.texture_name(second_tex,False)+".png"
            output=root/"output"
            output.mkdir()
            (output/canonical).write_bytes(self.mod.png_bytes(1,1,bytes([73])*4))
            source=root/"reuse.arc"
            source.write_bytes(u8([("one.tpl",first),("two.tpl",second)]))
            report=self.mod.extract(source,output,self.mod.Decoder(DLL))
            self.assertEqual(report["errors"],[])
            self.assertEqual({p.name for p in output.glob("*.png")},{canonical})
            self.assertTrue(all(t["files"]==[canonical] for t in report["textures"]))

    def test_full_cli_retains_mips_and_sampler_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            source=root/"mips.tpl"
            source.write_bytes(tpl(1,4,4,bytes([11])*96,levels=3))
            output=root/"output"
            run=subprocess.run([sys.executable,str(ROOT/"tools/texture_extract/extract_textures.py"),
                "--input",str(source),"--output",str(output),"--decoder",str(DLL),"--full"],
                capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertEqual(len(list(output.glob("*.png"))),6)


if __name__ == "__main__":
    unittest.main()
