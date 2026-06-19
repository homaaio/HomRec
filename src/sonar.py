#!/usr/bin/env python3
"""
Sonar v1.2 — File and device diagnostics tool

Architecture:
  Python  — UI, orchestration, lightweight tasks
  C       — entropy, CRC32, histograms, LSB analysis (sonar_core.dll/.so)
  HTML/JS — exportable HTML report

Features:
  - File analysis: EXIF/ID3/DOCX/PDF metadata, recursive archive structure
  - File comparison: line-by-line diff with highlighting (right-click -> Compare)
  - Header repair (right-click -> Repair)
  - Steganography: LSB image analysis
  - Deep scan: signature-based virus database from JSON
  - Devices: display, battery, Wi-Fi, Bluetooth, USB, speakers, mouse, keyboard, microphone
  - Real-time file monitoring
  - Scheduled scanning
  - Multi-threaded analysis
  - Drag & drop
  - Export: TXT / JSON / HTML
  - Light theme by default
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
import threading, queue, os, sys, zipfile, tarfile, gzip, bz2, lzma
import zlib, struct, time, wave, subprocess, platform, json, ctypes
import tempfile, hashlib, re, webbrowser, difflib, socket, math
import shutil, copy, traceback
from pathlib import Path
from datetime import datetime, timedelta

# ── OPTIONAL DEPENDENCIES ───────────────────────────────────────
try:
    from PIL import Image, ImageTk, ExifTags
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import mutagen
    from mutagen.id3 import ID3, ID3NoHeaderError
    from mutagen.mp3 import MP3
    from mutagen.flac import FLAC
    from mutagen.ogg import OggFileType
    HAS_MUTAGEN = True
except ImportError:
    HAS_MUTAGEN = False

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

# ── PATHS ────────────────────────────────────────────────────────
BASE_DIR   = Path(__file__).parent
VIRUS_DB   = BASE_DIR / "virus_db" / "signatures.json"
# Project layout: scripts live in src/, shared images live in ../Assets
ASSETS_DIR = BASE_DIR.parent / "Assets"

# ── C-CORE ──────────────────────────────────────────────────────
_C_SRC = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

double scan_entropy(const char *path){
    FILE *f=fopen(path,"rb");if(!f)return -1.0;
    uint64_t freq[256]={0},total=0;uint8_t buf[65536];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0){for(size_t i=0;i<n;i++)freq[buf[i]]++;total+=n;}
    fclose(f);if(total==0)return 0.0;
    double e=0.0;
    for(int i=0;i<256;i++){if(!freq[i])continue;double p=(double)freq[i]/(double)total;e-=p*log2(p);}
    return e;
}
double scan_nullratio(const char *p){
    FILE *f=fopen(p,"rb");if(!f)return -1.0;
    uint64_t z=0,t=0;uint8_t buf[65536];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0){for(size_t i=0;i<n;i++)if(!buf[i])z++;t+=n;}
    fclose(f);return t==0?0.0:(double)z/(double)t;
}
double scan_ascii_ratio(const char *p){
    FILE *f=fopen(p,"rb");if(!f)return -1.0;
    uint64_t a=0,t=0;uint8_t buf[65536];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0){for(size_t i=0;i<n;i++)if(buf[i]>=0x20&&buf[i]<=0x7E)a++;t+=n;}
    fclose(f);return t==0?0.0:(double)a/(double)t;
}
uint32_t calc_crc32(const char *p){
    static uint32_t tbl[256];static int ready=0;
    if(!ready){for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1);tbl[i]=c;}ready=1;}
    FILE *f=fopen(p,"rb");if(!f)return 0;
    uint32_t crc=0xFFFFFFFFu;uint8_t buf[65536];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0)for(size_t i=0;i<n;i++)crc=tbl[(crc^buf[i])&0xFF]^(crc>>8);
    fclose(f);return crc^0xFFFFFFFFu;
}
void byte_histogram(const char *p, uint64_t *out){
    memset(out,0,256*sizeof(uint64_t));
    FILE *f=fopen(p,"rb");if(!f)return;
    uint8_t buf[65536];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0)for(size_t i=0;i<n;i++)out[buf[i]]++;
    fclose(f);
}
double lsb_randomness(const uint8_t *pixels, int64_t count){
    if(count<2)return 0.0;
    int64_t diff=0;
    for(int64_t i=0;i+1<count;i++)diff+=((pixels[i]^pixels[i+1])&1);
    return (double)diff/(double)(count-1);
}
"""

def _find_lib():
    for name in ("sonar_core.dll","sonar_core.so"):
        for d in (BASE_DIR, BASE_DIR.parent, Path(tempfile.gettempdir())):
            p = d / name
            if p.exists(): return str(p)
    return None

def _compile_lib():
    is_win = platform.system()=="Windows"
    ext = ".dll" if is_win else ".so"
    dst = str(Path(tempfile.gettempdir()) / f"sonar_core{ext}")
    src = dst.replace(ext,".c")
    try:
        with open(src,"w") as f: f.write(_C_SRC)
        cmd = ["gcc","-O2","-shared","-o",dst,src,"-lm"]
        if not is_win: cmd.insert(3,"-fPIC")
        r = subprocess.run(cmd,capture_output=True,timeout=30)
        return dst if r.returncode==0 else None
    except: return None

class SonarCore:
    def __init__(self):
        self._lib=None
        lp=_find_lib() or _compile_lib()
        if lp:
            try:
                lib=ctypes.CDLL(lp)
                def _set(fn,ret,*args):
                    f=getattr(lib,fn); f.restype=ret; f.argtypes=list(args)
                _set("scan_entropy",   ctypes.c_double, ctypes.c_char_p)
                _set("scan_nullratio", ctypes.c_double, ctypes.c_char_p)
                _set("scan_ascii_ratio",ctypes.c_double,ctypes.c_char_p)
                _set("calc_crc32",     ctypes.c_uint32, ctypes.c_char_p)
                _set("byte_histogram", None, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64))
                _set("lsb_randomness", ctypes.c_double, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int64)
                self._lib=lib
            except: pass

    @property
    def available(self): return self._lib is not None

    def entropy(self,path):
        if self._lib: return float(self._lib.scan_entropy(path.encode()))
        return self._py_entropy(path)
    def null_ratio(self,path):
        if self._lib: return float(self._lib.scan_nullratio(path.encode()))
        return 0.0
    def ascii_ratio(self,path):
        if self._lib: return float(self._lib.scan_ascii_ratio(path.encode()))
        return 0.0
    def crc32(self,path):
        if self._lib: return int(self._lib.calc_crc32(path.encode()))
        return 0
    def histogram(self,path):
        if self._lib:
            arr=(ctypes.c_uint64*256)()
            self._lib.byte_histogram(path.encode(),arr)
            return list(arr)
        return [0]*256
    def lsb_randomness(self,pixels_bytes):
        if self._lib and pixels_bytes:
            arr=(ctypes.c_uint8*len(pixels_bytes))(*pixels_bytes)
            return float(self._lib.lsb_randomness(arr,len(pixels_bytes)))
        return self._py_lsb(pixels_bytes)
    def _py_entropy(self,path):
        try:
            freq=[0]*256
            with open(path,'rb') as f:
                for chunk in iter(lambda:f.read(65536),b''):
                    for b in chunk: freq[b]+=1
            total=sum(freq)
            if not total: return 0.0
            return -sum(p/total*math.log2(p/total) for p in freq if p)
        except: return 0.0
    def _py_lsb(self,pixels):
        if len(pixels)<2: return 0.0
        return sum((pixels[i]^pixels[i+1])&1 for i in range(len(pixels)-1))/(len(pixels)-1)

CORE = SonarCore()

# ── VIRUS SIGNATURE DATABASE ────────────────────────────────────
class VirusDB:
    def __init__(self):
        self.signatures=[]
        self.dangerous_ext=set()
        self.suspicious_ext=set()
        self.known_malware=set()
        self._load()

    def _load(self):
        try:
            if VIRUS_DB.exists():
                with open(VIRUS_DB,'r',encoding='utf-8') as f:
                    db=json.load(f)
                for sig in db.get('byte_signatures',[]):
                    try:
                        b=bytes.fromhex(sig['hex'].replace(' ',''))
                        self.signatures.append((b,sig['name'],sig['severity'],sig['type']))
                    except: pass
                self.dangerous_ext = set(db.get('dangerous_extensions',[]))
                self.suspicious_ext = set(db.get('suspicious_extensions',[]))
                self.known_malware  = set(db.get('known_malware_sha256',[]))
        except Exception as e:
            print(f"VirusDB load error: {e}")

    def scan(self, path:str, first64k:bytes) -> list:
        """Returns list of found threats."""
        found=[]
        # Byte signatures
        for sig,name,sev,typ in self.signatures:
            if sig in first64k:
                found.append({"name":name,"severity":sev,"type":typ})
        # SHA256 full file
        try:
            sha=hashlib.sha256()
            with open(path,'rb') as f:
                for chunk in iter(lambda:f.read(65536),b''): sha.update(chunk)
            digest=sha.hexdigest()
            if digest in self.known_malware:
                found.append({"name":f"Known malware (SHA256: {digest[:16]}…)","severity":"danger","type":"known_malware"})
        except: pass
        return found

VDB = VirusDB()

# ── UTILITIES ───────────────────────────────────────────────────
def _fmt(n):
    for u in ('B','KB','MB','GB'):
        if n<1024: return f"{n:.1f} {u}"
        n/=1024
    return f"{n:.1f} TB"

def _ts(): return datetime.now().strftime("%H:%M:%S")
def _dt(): return datetime.now().strftime("%d.%m.%Y %H:%M:%S")

# ── METADATA ────────────────────────────────────────────────────
class MetaReader:
    """Reads EXIF, ID3, PDF, DOCX metadata."""

    def read(self, path:str) -> dict:
        ext=Path(path).suffix.lower()
        if ext in ('.jpg','.jpeg','.png','.tiff','.webp') and HAS_PIL:
            return self._exif(path)
        elif ext in ('.mp3','.flac','.ogg','.m4a') and HAS_MUTAGEN:
            return self._id3(path)
        elif ext=='.pdf':
            return self._pdf(path)
        elif ext in('.docx','.xlsx','.pptx'):
            return self._ooxml(path)
        return {}

    def _exif(self,path):
        meta={"format":"EXIF/Image"}
        try:
            img=Image.open(path)
            meta["mode"]      = img.mode
            meta["size"]      = f"{img.width}×{img.height}"
            meta["format_str"]= img.format or "?"
            raw=img._getexif() if hasattr(img,'_getexif') else None
            if raw:
                for tag,val in raw.items():
                    name=ExifTags.TAGS.get(tag,str(tag))
                    if isinstance(val,bytes): continue
                    meta[name]=str(val)[:120]
        except Exception as e: meta["error"]=str(e)
        return meta

    def _id3(self,path):
        meta={"format":"ID3/Audio"}
        try:
            audio=mutagen.File(path)
            if audio:
                if hasattr(audio,'info'):
                    info=audio.info
                    if hasattr(info,'length'):   meta["duration"]=f"{info.length:.1f}s"
                    if hasattr(info,'bitrate'):  meta["bitrate"] =f"{info.bitrate} bps"
                    if hasattr(info,'sample_rate'): meta["sample_rate"]=f"{info.sample_rate} Hz"
                for k,v in audio.items():
                    meta[str(k)]=str(v)[:120]
        except Exception as e: meta["error"]=str(e)
        return meta

    def _pdf(self,path):
        meta={"format":"PDF"}
        try:
            with open(path,'rb') as f:
                data=f.read(4096)
            # Search /Info
            for field in (b'Title',b'Author',b'Creator',b'Producer',b'Subject',b'Keywords',b'CreationDate'):
                pat=b'/'+field+b' ('
                idx=data.find(pat)
                if idx>=0:
                    start=idx+len(pat); end=data.find(b')',start)
                    if end>start:
                        val=data[start:end].decode('latin-1','replace')[:100]
                        meta[field.decode()]=val
            # Version
            if data[:4]==b'%PDF': meta["version"]=data[5:8].decode('ascii','replace')
        except Exception as e: meta["error"]=str(e)
        return meta

    def _ooxml(self,path):
        meta={"format":"OOXML"}
        try:
            with zipfile.ZipFile(path,'r') as z:
                if 'docProps/core.xml' in z.namelist():
                    xml=z.read('docProps/core.xml').decode('utf-8','replace')
                    for tag in ('dc:title','dc:creator','cp:lastModifiedBy','dcterms:created',
                                'dcterms:modified','cp:revision','cp:lastPrinted'):
                        m=re.search(rf'<{re.escape(tag)}[^>]*>(.*?)</{re.escape(tag)}>', xml)
                        if m: meta[tag.split(':')[1]]=m.group(1)[:100]
                if 'docProps/app.xml' in z.namelist():
                    xml=z.read('docProps/app.xml').decode('utf-8','replace')
                    for tag in ('Application','AppVersion','Company','Pages','Words','Characters'):
                        m=re.search(rf'<{tag}>(.*?)</{tag}>', xml)
                        if m: meta[tag]=m.group(1)[:100]
        except Exception as e: meta["error"]=str(e)
        return meta

META = MetaReader()

# ── RECURSIVE ARCHIVE ANALYSIS ──────────────────────────────────
class ArchiveAnalyzer:
    MAX_DEPTH = 5
    MAX_ENTRIES = 2000

    def analyze(self, path:str, depth:int=0) -> dict:
        if depth>=self.MAX_DEPTH:
            return {"error":"Max depth reached"}
        ext=Path(path).suffix.lower()
        try:
            if ext in('.zip','.docx','.xlsx','.pptx','.jar','.apk','.odt'):
                return self._zip(path,depth)
            elif ext in('.tar','.tgz','.tar.gz','.tar.bz2','.tar.xz'):
                return self._tar(path,depth)
            elif ext in('.gz',) and not path.endswith('.tar.gz'):
                return self._gz(path,depth)
            elif ext in('.7z',):
                return self._sevenz(path,depth)
        except Exception as e:
            return {"error":str(e)}
        return {}

    def _zip(self,path,depth):
        result={"type":"ZIP","depth":depth,"entries":[],"stats":{}}
        with zipfile.ZipFile(path,'r') as z:
            infos=z.infolist()[:self.MAX_ENTRIES]
            total_comp=sum(i.compress_size for i in infos)
            total_unc =sum(i.file_size for i in infos)
            exts={}
            dangerous=[]
            nested=[]
            for info in infos:
                e=Path(info.filename).suffix.lower()
                exts[e]=exts.get(e,0)+1
                if e in VDB.dangerous_ext: dangerous.append(info.filename)
                if e in('.zip','.gz','.bz2','.7z','.tar','.rar'): nested.append(info.filename)
                result["entries"].append({
                    "name":info.filename,
                    "size":_fmt(info.file_size),
                    "compressed":_fmt(info.compress_size),
                    "is_dir":info.is_dir(),
                    "ext":e
                })
            result["stats"]={
                "total_files":len(infos),
                "compressed":_fmt(total_comp),
                "uncompressed":_fmt(total_unc),
                "ratio":f"{total_comp/total_unc*100:.1f}%" if total_unc else "—",
                "ext_summary":dict(sorted(exts.items(),key=lambda x:-x[1])[:10]),
                "dangerous_files":dangerous[:20],
                "nested_archives":nested[:10],
                "zip_bomb_risk": total_unc>1_000_000_000 or (total_comp>0 and total_unc/total_comp>200)
            }
            # Recursive nested archive analysis
            if nested and depth<self.MAX_DEPTH:
                result["nested"]={}
                for nname in nested[:3]:
                    try:
                        with z.open(nname) as src:
                            tmp=tempfile.NamedTemporaryFile(suffix=Path(nname).suffix,delete=False)
                            tmp.write(src.read(10*1024*1024))
                            tmp.close()
                            result["nested"][nname]=self.analyze(tmp.name,depth+1)
                            os.unlink(tmp.name)
                    except: pass
        return result

    def _tar(self,path,depth):
        result={"type":"TAR","depth":depth,"stats":{}}
        with tarfile.open(path,'r:*') as t:
            members=t.getmembers()[:self.MAX_ENTRIES]
            exts={}
            for m in members:
                e=Path(m.name).suffix.lower()
                exts[e]=exts.get(e,0)+1
            total_size=sum(m.size for m in members)
            result["stats"]={
                "total_files":len(members),
                "total_size":_fmt(total_size),
                "ext_summary":dict(sorted(exts.items(),key=lambda x:-x[1])[:10])
            }
        return result

    def _gz(self,path,depth):
        result={"type":"GZIP","depth":depth}
        with gzip.open(path,'rb') as f:
            size=sum(len(c) for c in iter(lambda:f.read(65536),b''))
        result["uncompressed_size"]=_fmt(size)
        orig_size=os.path.getsize(path)
        result["compression_ratio"]=f"{orig_size/size*100:.1f}%" if size else "—"
        return result

    def _sevenz(self,path,depth):
        result={"type":"7ZIP","depth":depth}
        try:
            r=subprocess.run(['7z','l',path],capture_output=True,timeout=15,text=True)
            lines=r.stdout.splitlines()
            count=sum(1 for l in lines if l.strip() and not l.startswith('-') and len(l)>40)
            result["approx_files"]=count
        except:
            result["error"]="7z not found"
        return result

ARCH = ArchiveAnalyzer()

# ── STEGANOGRAPHY (LSB) ─────────────────────────────────────────
class StegoAnalyzer:
    def analyze(self, path:str) -> dict:
        if not HAS_PIL:
            return {"error":"Required Pillow: pip install Pillow"}
        result={}
        try:
            img=Image.open(path).convert("RGB")
            pixels=list(img.getdata())
            w,h=img.size
            result["size"]=f"{w}×{h}"
            result["total_pixels"]=w*h

            # Flat channels
            r_ch=bytes(p[0] for p in pixels)
            g_ch=bytes(p[1] for p in pixels)
            b_ch=bytes(p[2] for p in pixels)

            lsb_r=CORE.lsb_randomness(r_ch)
            lsb_g=CORE.lsb_randomness(g_ch)
            lsb_b=CORE.lsb_randomness(b_ch)
            result["lsb_r"]=round(lsb_r,4)
            result["lsb_g"]=round(lsb_g,4)
            result["lsb_b"]=round(lsb_b,4)
            avg=(lsb_r+lsb_g+lsb_b)/3
            result["lsb_avg"]=round(avg,4)

            # Random LSB ≈ 0.5 — suspicious (natural images: 0.45–0.55 normal)
            # If too close to 0.5 and variance small → possible stego
            dev=max(abs(lsb_r-0.5),abs(lsb_g-0.5),abs(lsb_b-0.5))
            result["suspicion_score"]=round(1.0-dev*4,2)  # 0..1

            if avg>0.48 and dev<0.03:
                result["verdict"]="⚠ High probability of LSB steganography"
                result["level"]="warn"
            elif avg>0.45 and dev<0.07:
                result["verdict"]="? Possible LSB steganography (verify manually)"
                result["level"]="info"
            else:
                result["verdict"]="✓ LSB pattern normal"
                result["level"]="ok"

            # Chi-square on R-channel LSB
            lsb_bits=[b&1 for b in r_ch]
            n0=lsb_bits.count(0); n1=lsb_bits.count(1)
            total_lsb=n0+n1
            if total_lsb>0:
                expected=total_lsb/2
                chi2=((n0-expected)**2+(n1-expected)**2)/expected if expected else 0
                result["chi2_r"]=round(chi2,4)
                result["chi2_verdict"]="suspicious (χ²<1 → near-perfect randomness)" if chi2<1 else "normal"

        except Exception as e:
            result["error"]=str(e)
        return result

STEGO = StegoAnalyzer()

# ── FILE REPAIR ─────────────────────────────────────────────────
class FileRepairer:
    # Only bytes that are *always* constant for a given format go here.
    # NOTE: for ZIP we previously hardcoded a full 10-byte local-file-header
    # (signature + version + flags + method). Those last 6 bytes legitimately
    # vary between valid ZIPs (different compression method, different
    # general-purpose flags, etc.), so comparing against one fixed value
    # produced false positives on perfectly healthy archives and then
    # overwrote their real header fields — corrupting files that were never
    # broken, while still reporting "repaired". Only the 4-byte magic
    # signature is ever safe to rewrite.
    HEADERS = {
        '.jpg':  b'\xff\xd8\xff\xe0\x00\x10JFIF',
        '.jpeg': b'\xff\xd8\xff\xe0\x00\x10JFIF',
        '.png':  b'\x89PNG\r\n\x1a\n',
        '.gif':  b'GIF89a',
        '.pdf':  b'%PDF-1.4\n',
        '.zip':  b'PK\x03\x04',
        '.gz':   b'\x1f\x8b\x08\x00\x00\x00\x00\x00',
        '.bmp':  b'BM',
        '.mp3':  b'\xff\xfb',
        '.wav':  b'RIFF',
    }
    FOOTERS = {
        '.jpg':  b'\xff\xd9',
        '.jpeg': b'\xff\xd9',
        '.png':  b'\x00\x00\x00\x00IEND\xaeB`\x82',
        '.pdf':  b'\n%%EOF\n',
        '.gif':  b'\x00;',
    }
    ZIP_FAMILY = ('.zip','.docx','.xlsx','.pptx','.jar','.apk')

    def attempt_repair(self, path:str, progress_cb=None) -> dict:
        result={"path":path,"actions":[],"success":False}
        ext=Path(path).suffix.lower()

        def step(msg):
            result["actions"].append(msg)
            if progress_cb: progress_cb(msg)
            time.sleep(0.1)

        step(f"Reading file: {os.path.basename(path)}")
        try:
            with open(path,'rb') as f: data=f.read()
        except Exception as e:
            result["error"]=str(e); return result

        original=data
        step(f"Size: {_fmt(len(data))}, extension: {ext}")

        # 1. Detect real type from content
        detected=self._detect(data)
        if detected and detected!=ext:
            step(f"⚠ Detected format: {detected} (extension: {ext})")
            result["detected_type"]=detected
        else:
            step(f"Format matches extension: {ext}")

        use_ext=detected or ext

        # 2. Fix header (signature bytes only — never touch fields that
        #    legitimately vary between valid files, e.g. ZIP version/flags/method)
        if use_ext in self.HEADERS:
            expected=self.HEADERS[use_ext]
            if not data.startswith(expected):
                step(f"Header signature damaged — restoring ({len(expected)} bytes)")
                data=expected+data[len(expected):]
                result["header_fixed"]=True
            else:
                step("Header OK")

        # 3. Fix footer
        if use_ext in self.FOOTERS:
            expected=self.FOOTERS[use_ext]
            if not data.endswith(expected):
                step(f"Footer missing — appending {len(expected)} bytes")
                data=data+expected
                result["footer_fixed"]=True
            else:
                step("Footer OK")

        # 4. ZIP family: if the local header is present but not at offset 0
        #    (junk/garbage prefix), trim everything before it
        if use_ext in self.ZIP_FAMILY:
            pk_pos=data.find(b'PK\x03\x04')
            if pk_pos>0:
                step(f"ZIP: local header found at offset {pk_pos} — trimming prefix")
                data=data[pk_pos:]
                result["zip_trimmed"]=True

        # 5. GZIP: attempt find magic
        if use_ext=='.gz':
            gz_pos=data.find(b'\x1f\x8b')
            if gz_pos>0:
                step(f"GZIP: magic found at offset {gz_pos}")
                data=data[gz_pos:]

        # 6. ZIP family: actually verify the archive opens and its entries
        #    pass a CRC check before claiming success — don't just trust
        #    that "we made some byte changes" means "it's fixed"
        zip_valid=None
        if use_ext in self.ZIP_FAMILY:
            zip_valid=self._verify_zip(data)
            step("ZIP structure verified — archive opens cleanly" if zip_valid
                 else "⚠ ZIP structure still invalid after repair attempt")

        # 7. Save if anything changed
        if data!=original:
            if use_ext in self.ZIP_FAMILY and not zip_valid:
                step("✗ Repair would not produce a valid archive — file left untouched")
                result["error"]="Could not reconstruct a valid ZIP from this file. " \
                                 "The data needed to rebuild it (central directory / " \
                                 "compressed entries) appears to be missing or corrupted " \
                                 "beyond what a header/footer fix can recover."
                result["success"]=False
            else:
                backup=path+".sonar_bak"
                try:
                    shutil.copy2(path,backup)
                    step(f"Backup: {os.path.basename(backup)}")
                    with open(path,'wb') as f: f.write(data)
                    result["saved"]=True
                    result["success"]=True
                    step(f"✓ File repaired ({_fmt(len(data))})")
                except Exception as e:
                    step(f"✗ Could not save: {e}")
                    result["error"]=str(e)
        else:
            if use_ext in self.ZIP_FAMILY and zip_valid is False:
                step("✗ File unchanged — ZIP is still invalid and no safe fix was found")
                result["success"]=False
                result["error"]="Archive could not be validated; no changes were safe to make."
            else:
                step("No changes needed — file OK or cannot be repaired")
                result["success"]=True
                result["no_changes"]=True

        return result

    def _verify_zip(self,data:bytes)->bool:
        """Write to a temp file and confirm zipfile can actually open and CRC-check it."""
        tmp=os.path.join(tempfile.gettempdir(),f"sonar_zipcheck_{os.getpid()}_{int(time.time()*1000)}.zip")
        try:
            with open(tmp,'wb') as f: f.write(data)
            with zipfile.ZipFile(tmp,'r') as z:
                return z.testzip() is None and len(z.namelist())>0
        except Exception:
            return False
        finally:
            try: os.remove(tmp)
            except Exception: pass

    def _detect(self,data):
        SIGS={b'\x89PNG\r\n\x1a\n':'.png',b'\xff\xd8\xff':'.jpg',
              b'%PDF':'.pdf',b'PK\x03\x04':'.zip',b'\x1f\x8b':'.gz',
              b'GIF8':'.gif',b'BM':'.bmp',b'RIFF':'.wav',b'ID3':'.mp3',
              b'\x7fELF':'.elf',b'MZ':'.exe'}
        for sig,ext in SIGS.items():
            if data[:len(sig)]==sig: return ext
        return None

REPAIRER = FileRepairer()

# ── THREAT ANALYSIS (extended) ──────────────────────────────────
_MALWARE_NAME_RE=re.compile(
    r'(invoice|free.?crack|keygen|patch|serial|activat|hack|trojan'
    r'|ransomware|virus|malware|payload|exploit|dropper|loader|stager'
    r'|bypass|inject|shellcode|rootkit|backdoor|crypter|obfuscat)',re.IGNORECASE)
_DOUBLE_EXT_RE=re.compile(
    r'\.(pdf|doc|docx|jpg|png|mp3|mp4|txt)\.(exe|bat|cmd|vbs|js|ps1|scr|pif|com)$',re.IGNORECASE)

def threat_scan(path:str,entropy:float,null_ratio:float,first64k:bytes) -> dict:
    reasons=[]; level="clean"
    _lv={"clean":0,"suspicious":1,"danger":2}
    def _up(l): nonlocal level; level=l if _lv[l]>_lv[level] else level

    fname=os.path.basename(path); ext=Path(path).suffix.lower()

    # 1. Bdatabase signatures
    hits=VDB.scan(path,first64k)
    for h in hits:
        if h["severity"]=="danger":
            reasons.append(f"🚨 [{h['type'].upper()}] {h['name']}"); _up("danger")
        elif h["severity"]=="warn":
            reasons.append(f"⚠ [{h['type'].upper()}] {h['name']}"); _up("suspicious")
        else:
            reasons.append(f"ℹ {h['name']}")

    # 2. Double extension
    if _DOUBLE_EXT_RE.search(fname):
        reasons.append(f"🚨 Double extension: «{fname}»"); _up("danger")

    # 3. Name
    if _MALWARE_NAME_RE.search(fname):
        reasons.append("⚠ Suspicious filename"); _up("suspicious")

    # 4. Entropy EXE
    if ext in('.exe','.dll','.scr','.sys','.com') and entropy>7.2:
        reasons.append(f"⚠ EXE high entropy ({entropy:.2f}) — possible packer"); _up("suspicious")

    # 5. Script with obfuscation
    if ext in('.js','.vbs','.ps1','.bat','.cmd') and entropy>5.5:
        reasons.append(f"⚠ Script with high entropy ({entropy:.2f})"); _up("suspicious")

    # 6. ZIP-bomb
    if ext in('.zip','.docx','.xlsx','.pptx','.jar','.apk'):
        try:
            with zipfile.ZipFile(path,'r') as z:
                comp=sum(i.compress_size for i in z.infolist())
                unc =sum(i.file_size for i in z.infolist())
                if unc>1_000_000_000:
                    reasons.append(f"🚨 ZIP-bomb: {unc//1_000_000} MB unpacked"); _up("danger")
                elif comp>0 and unc/comp>200:
                    reasons.append(f"⚠ Suspicious compression ratio ×{unc/comp:.0f}"); _up("suspicious")
                exes=[n for n in z.namelist() if Path(n).suffix.lower() in VDB.dangerous_ext]
                if exes:
                    reasons.append(f"⚠ Executables in archive: {', '.join(exes[:3])}"
                                   +(f" +{len(exes)-3}" if len(exes)>3 else "")); _up("suspicious")
        except: pass

    # 7. PDF exploits
    if ext=='.pdf':
        if b'/JavaScript' in first64k or b'/JS' in first64k:
            reasons.append("⚠ PDF /JavaScript"); _up("suspicious")
        if b'/Launch' in first64k:
            reasons.append("🚨 PDF /Launch (known exploit)"); _up("danger")

    # 8. Many null bytes in EXE
    if ext in('.exe','.dll') and null_ratio>0.6:
        reasons.append(f"⚠ {null_ratio*100:.0f}% null bytes in EXE"); _up("suspicious")

    return {"level":level,"reasons":reasons,"hits":hits}

# ── PROCESS & AUTORUN ANALYSIS ──────────────────────────────────
class ProcessScanner:
    SUSPICIOUS_NAMES=re.compile(
        r'(miner|cryptominer|xmrig|monero|coinhive|svchost32|svch0st'
        r'|wscript|cscript|powershell|cmd|regsvr32|rundll|mshta|certutil'
        r'|bitsadmin|wmic|cmstp)',re.IGNORECASE)

    def scan_processes(self) -> list:
        if not HAS_PSUTIL: return [{"error":"psutil not available"}]
        results=[]
        try:
            for proc in psutil.process_iter(['pid','name','exe','cpu_percent','memory_info','status']):
                try:
                    info=proc.info
                    suspicious=bool(self.SUSPICIOUS_NAMES.search(info.get('name','') or ''))
                    mem=info['memory_info'].rss if info.get('memory_info') else 0
                    results.append({
                        "pid": info['pid'],
                        "name": info.get('name','?'),
                        "exe": info.get('exe','?') or '?',
                        "cpu": info.get('cpu_percent',0),
                        "mem": _fmt(mem),
                        "status": info.get('status','?'),
                        "suspicious": suspicious
                    })
                except: pass
        except Exception as e:
            results.append({"error":str(e)})
        return sorted(results,key=lambda x:x.get('suspicious',False),reverse=True)

    def scan_autorun(self) -> list:
        results=[]
        sys_name=platform.system()
        if sys_name=="Windows":
            results+=self._win_autorun()
        elif sys_name=="Linux":
            results+=self._linux_autorun()
        elif sys_name=="Darwin":
            results+=self._mac_autorun()
        return results

    def _win_autorun(self):
        items=[]
        keys=["HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
              "HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
              "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce"]
        for key in keys:
            try:
                r=subprocess.run(["reg","query",key],capture_output=True,text=True,timeout=5)
                for line in r.stdout.splitlines():
                    if 'REG_' in line:
                        parts=line.strip().split(None,2)
                        if len(parts)>=3:
                            name,_,val=parts[0],parts[1],parts[2]
                            suspicious=bool(self.SUSPICIOUS_NAMES.search(val))
                            items.append({"location":key,"name":name,"value":val[:100],"suspicious":suspicious})
            except: pass
        return items

    def _linux_autorun(self):
        items=[]
        paths=[Path('/etc/init.d'),Path('/etc/rc.local'),Path.home()/'.config/autostart',
               Path('/etc/xdg/autostart')]
        for p in paths:
            try:
                if p.is_file():
                    items.append({"location":str(p),"name":p.name,"value":"script","suspicious":False})
                elif p.is_dir():
                    for f in p.iterdir():
                        if f.is_file():
                            suspicious=bool(self.SUSPICIOUS_NAMES.search(f.name))
                            items.append({"location":str(p),"name":f.name,
                                         "value":str(f),"suspicious":suspicious})
            except: pass
        # systemd
        for svc_dir in (Path('/etc/systemd/system'),Path('/lib/systemd/system')):
            try:
                if svc_dir.exists():
                    for f in list(svc_dir.glob('*.service'))[:20]:
                        items.append({"location":str(svc_dir),"name":f.name,"value":str(f),"suspicious":False})
            except: pass
        return items

    def _mac_autorun(self):
        items=[]
        for d in (Path.home()/'Library/LaunchAgents',Path('/Library/LaunchAgents'),
                  Path('/Library/LaunchDaemons')):
            try:
                if d.exists():
                    for f in d.glob('*.plist'):
                        items.append({"location":str(d),"name":f.name,"value":str(f),"suspicious":False})
            except: pass
        return items

PROC_SCANNER = ProcessScanner()

# ── DEVICE TESTS ────────────────────────────────────────────────
class DeviceTester:

    # ── Battery ───────────────────────────────────────────────────────────
    def battery(self) -> dict:
        r={"available":False}
        if HAS_PSUTIL:
            try:
                b=psutil.sensors_battery()
                if b:
                    r={"available":True,"percent":round(b.percent,1),
                       "plugged":b.power_plugged,
                       "time_left":str(timedelta(seconds=int(b.secsleft))) if b.secsleft>0 else "∞"}
            except: pass
        if not r["available"] and platform.system()=="Linux":
            try:
                bp=Path("/sys/class/power_supply/BAT0")
                if bp.exists():
                    cap=int((bp/"capacity").read_text().strip())
                    status=(bp/"status").read_text().strip()
                    r={"available":True,"percent":cap,"plugged":status=="Charging","time_left":"?"}
                    try:
                        cyc=int((bp/"cycle_count").read_text().strip())
                        r["cycles"]=cyc
                    except: pass
                    try:
                        full=int((bp/"energy_full").read_text().strip())
                        design=int((bp/"energy_full_design").read_text().strip())
                        r["health"]=round(full/design*100,1) if design else None
                    except: pass
            except: pass
        if platform.system()=="Windows" and not r["available"]:
            try:
                out=subprocess.check_output(
                    ["powershell","-Command",
                     "Get-WmiObject Win32_Battery | Select-Object EstimatedChargeRemaining,BatteryStatus,DesignCapacity,FullChargeCapacity"],
                    timeout=8,text=True)
                for line in out.splitlines():
                    if "EstimatedChargeRemaining" in line:
                        pct=re.search(r'\d+',line.split(':')[-1])
                        if pct: r={"available":True,"percent":int(pct.group()),"plugged":False,"time_left":"?"}
            except: pass
        return r

    # ── Network / Wi-Fi ──────────────────────────────────────────────────────
    def _ping_host(self,host,count=4,timeout_s=10):
        """Ping one host, return dict(ok,ping_ms,packet_loss,raw) — cross-platform."""
        res={"ok":False,"ping_ms":None,"packet_loss":None,"raw":""}
        try:
            is_win=platform.system()=="Windows"
            cmd=(["ping","-n",str(count),"-w","1500",host] if is_win
                 else ["ping","-c",str(count),"-W","2",host])
            pr=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout_s)
            out=pr.stdout
            res["raw"]=out
            ml=re.search(r'(\d+)%\s*(?:packet\s*)?loss',out,re.I)
            if ml: res["packet_loss"]=int(ml.group(1))
            m=re.search(r'Average\s*=\s*(\d+)\s*ms',out,re.I)
            if not m:
                m=re.search(r'=\s*[\d.]+/([\d.]+)/[\d.]+(?:/[\d.]+)?\s*ms',out)
            if m:
                res["ping_ms"]=float(m.group(1))
                res["ok"]=True
            elif pr.returncode==0 and res["packet_loss"] is not None and res["packet_loss"]<100:
                res["ok"]=True
        except Exception as e:
            res["raw"]=str(e)
        return res

    def network_test(self, progress_cb=None) -> dict:
        r={"ping_ms":None,"packet_loss":None,"download_mbps":None,"upload_mbps":None,
           "dns_ok":None,"gateway_ok":None,"internet_ok":False,"ping_host":None,
           "interfaces":[],"status":"unknown","details":[]}
        def step(msg):
            r["details"].append(msg)
            if progress_cb: progress_cb(msg)

        step("Проверка сетевых интерфейсов…")
        active_iface=None
        if HAS_PSUTIL:
            try:
                stats=psutil.net_if_stats()
                addrs=psutil.net_if_addrs()
                for iface,stat in stats.items():
                    addr_list=addrs.get(iface,[])
                    ips=[a.address for a in addr_list if a.family==socket.AF_INET]
                    if stat.isup and ips and not ips[0].startswith("127."):
                        r["interfaces"].append({"name":iface,"ip":ips[0],"speed":stat.speed})
                        step(f"Интерфейс: {iface} — {ips[0]} ({stat.speed} Mbps)")
                        if not active_iface: active_iface=iface
            except: pass
        if not r["interfaces"]:
            step("⚠ Активные сетевые интерфейсы не найдены")

        step("Проверка шлюза по умолчанию…")
        gw=self._default_gateway()
        if gw:
            gres=self._ping_host(gw,count=2,timeout_s=5)
            r["gateway_ok"]=gres["ok"]
            step(f"Шлюз {gw}: {'OK ('+str(gres['ping_ms'])+' ms)' if gres['ok'] else 'нет ответа'}")
        else:
            step("Шлюз не определён")

        step("Проверка интернет-соединения (ping)…")
        for host in ("8.8.8.8","1.1.1.1","77.88.8.8"):
            pres=self._ping_host(host,count=4,timeout_s=8)
            if pres["packet_loss"] is not None and r["packet_loss"] is None:
                r["packet_loss"]=pres["packet_loss"]
            if pres["ok"]:
                r["ping_ms"]=pres["ping_ms"]; r["ping_host"]=host
                r["packet_loss"]=pres["packet_loss"] if pres["packet_loss"] is not None else 0
                r["internet_ok"]=True
                step(f"Ping {host}: {r['ping_ms']} ms, потери {r['packet_loss']}%")
                break
            else:
                step(f"Ping {host}: нет ответа")

        step("Проверка DNS…")
        try:
            socket.setdefaulttimeout(4)
            socket.gethostbyname("ya.ru")
            r["dns_ok"]=True; step("DNS: OK")
        except Exception:
            r["dns_ok"]=False; step("DNS: не отвечает")
        finally:
            socket.setdefaulttimeout(None)

        if r["internet_ok"]:
            step("Тест скорости загрузки (HTTP)…")
            try:
                import urllib.request, time as _t
                url="http://speedtest.tele2.net/1MB.zip"
                start=_t.time()
                with urllib.request.urlopen(url,timeout=10) as resp:
                    data_len=len(resp.read(1024*1024))
                elapsed=_t.time()-start
                if elapsed>0:
                    r["download_mbps"]=round(data_len*8/elapsed/1_000_000,2)
                    step(f"Скорость загрузки: {r['download_mbps']} Mbps")
            except Exception as e:
                step(f"Тест скорости недоступен: {e}")

        if not r["interfaces"]:
            r["status"]="no_adapter"
        elif r["gateway_ok"] is False:
            r["status"]="no_gateway"
        elif not r["internet_ok"]:
            r["status"]="no_internet"
        elif r["dns_ok"] is False:
            r["status"]="no_dns"
        elif r["packet_loss"] and r["packet_loss"]>=20:
            r["status"]="unstable"
        else:
            r["status"]="ok"
        step(f"Статус соединения: {r['status']}")
        return r

    def _default_gateway(self):
        """Best-effort default gateway lookup, cross-platform."""
        try:
            system=platform.system()
            if system=="Windows":
                out=subprocess.check_output(["ipconfig"],timeout=5,text=True,errors="ignore")
                m=re.search(r'Default Gateway[^\n:]*:\s*([\d.]+)',out)
                if m and m.group(1).strip(): return m.group(1).strip()
            elif system=="Darwin":
                out=subprocess.check_output(["route","-n","get","default"],timeout=5,text=True)
                m=re.search(r'gateway:\s*([\d.]+)',out)
                if m: return m.group(1)
            else:
                out=subprocess.check_output(["ip","route"],timeout=5,text=True)
                m=re.search(r'default via ([\d.]+)',out)
                if m: return m.group(1)
        except Exception:
            pass
        return None

    def network_repair(self, progress_cb=None) -> dict:
        """Attempt to repair a broken network connection.

        Mirrors 'Devices -> Internet -> RMB -> Try to fix': release/renew IP,
        flush DNS, reset Winsock/network stack, restart the adapter.
        Returns dict(actions=[(label, ok, detail), ...], retest=<network_test result>).
        """
        result={"actions":[],"retest":None}
        def step(msg):
            result["actions"].append((msg,None,""))
            if progress_cb: progress_cb(msg)
        def run(cmd,label,timeout=20):
            try:
                pr=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout)
                ok=pr.returncode==0
                lines=(pr.stdout or pr.stderr or "").strip().splitlines()
                detail=lines[-1] if lines else ("OK" if ok else "ошибка")
                result["actions"][-1]=(label,ok,detail)
                if progress_cb: progress_cb(f"{'✓' if ok else '✗'} {label}: {detail}")
                return ok
            except Exception as e:
                result["actions"][-1]=(label,False,str(e))
                if progress_cb: progress_cb(f"✗ {label}: {e}")
                return False

        system=platform.system()
        if system=="Windows":
            step("Сброс Winsock…");          run(["netsh","winsock","reset"],"netsh winsock reset")
            step("Сброс TCP/IP стека…");      run(["netsh","int","ip","reset"],"netsh int ip reset")
            step("Освобождение IP-адреса…");  run(["ipconfig","/release"],"ipconfig /release")
            step("Обновление IP-адреса…");    run(["ipconfig","/renew"],"ipconfig /renew")
            step("Очистка кэша DNS…");        run(["ipconfig","/flushdns"],"ipconfig /flushdns")
        elif system=="Darwin":
            step("Очистка кэша DNS…")
            run(["sudo","killall","-HUP","mDNSResponder"],"flush DNS (mDNSResponder)")
            try:
                svc=subprocess.check_output(["bash","-c",
                    "networksetup -listallnetworkservices | tail -n +2 | head -1"],
                    timeout=5,text=True).strip()
                if svc:
                    step(f"Перезапуск адаптера ({svc})…")
                    run(["networksetup","-setnetworkserviceenabled",svc,"off"],f"{svc} off")
                    time.sleep(2)
                    run(["networksetup","-setnetworkserviceenabled",svc,"on"],f"{svc} on")
            except Exception: pass
        else:  # Linux
            step("Очистка кэша DNS…")
            if not run(["systemd-resolve","--flush-caches"],"systemd-resolve --flush-caches",timeout=10):
                run(["resolvectl","flush-caches"],"resolvectl flush-caches",timeout=10)
            step("Перезапуск NetworkManager / интерфейса…")
            run(["nmcli","networking","off"],"nmcli networking off",timeout=10)
            run(["nmcli","networking","on"],"nmcli networking on",timeout=10)
            run(["systemctl","restart","NetworkManager"],"restart NetworkManager",timeout=15)
            try:
                ifaces=psutil.net_if_stats().keys() if HAS_PSUTIL else []
                main_if=next((i for i in ifaces if i!="lo"),None)
                if main_if:
                    run(["dhclient","-r",main_if],f"dhclient -r {main_if}",timeout=10)
                    run(["dhclient",main_if],f"dhclient {main_if}",timeout=15)
            except Exception: pass

        if progress_cb: progress_cb("Повторная проверка соединения…")
        time.sleep(1.5)
        result["retest"]=self.network_test(progress_cb=progress_cb)
        return result

    # ── USB ───────────────────────────────────────────────────────────────
    def usb_info(self) -> list:
        devices=[]
        if platform.system()=="Linux":
            try:
                out=subprocess.check_output(["lsusb"],timeout=5,text=True)
                for line in out.splitlines():
                    m=re.match(r'Bus (\d+) Device (\d+): ID (\S+) (.*)',line)
                    if m:
                        devices.append({"bus":m.group(1),"dev":m.group(2),
                                        "id":m.group(3),"name":m.group(4)[:60]})
            except: pass
        elif platform.system()=="Windows":
            try:
                out=subprocess.check_output(
                    ["powershell","-Command",
                     "Get-PnpDevice -Class USB | Select-Object -ExpandProperty FriendlyName"],
                    timeout=8,text=True)
                for line in out.splitlines():
                    if line.strip():
                        devices.append({"name":line.strip()[:80]})
            except: pass
        elif platform.system()=="Darwin":
            try:
                out=subprocess.check_output(
                    ["system_profiler","SPUSBDataType","-detailLevel","mini"],
                    timeout=10,text=True)
                for line in out.splitlines():
                    if 'Product ID' in line or line.strip().startswith('USB'):
                        devices.append({"name":line.strip()[:80]})
            except: pass
        return devices

    # ── Bluetooth ─────────────────────────────────────────────────────────
    def bluetooth_scan(self, progress_cb=None) -> dict:
        r={"devices":[],"details":[]}
        def step(msg):
            r["details"].append(msg); progress_cb(msg) if progress_cb else None

        step("Scanning Bluetooth…")
        if platform.system()=="Linux":
            try:
                out=subprocess.check_output(["bluetoothctl","devices"],timeout=5,text=True)
                for line in out.splitlines():
                    m=re.match(r'Device (\S+) (.*)',line)
                    if m: r["devices"].append({"mac":m.group(1),"name":m.group(2)})
                step(f"Saved devices found: {len(r['devices'])}")
                # Scanning 5 sec
                proc=subprocess.Popen(["bluetoothctl","scan","on"],
                                      stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                time.sleep(5); proc.terminate()
                step("Scan complete")
            except Exception as e: step(f"bluetoothctl: {e}")
        elif platform.system()=="Windows":
            try:
                out=subprocess.check_output(
                    ["powershell","-Command",
                     "Get-PnpDevice -Class Bluetooth | Select-Object FriendlyName,Status"],
                    timeout=8,text=True)
                for line in out.splitlines():
                    if line.strip() and 'FriendlyName' not in line and '---' not in line:
                        r["devices"].append({"name":line.strip()[:60]})
                step(f"BT devices found: {len(r['devices'])}")
            except Exception as e: step(f"PowerShell BT: {e}")
        else:
            step("Bluetooth scanning supported on Linux/Windows")
        return r

    # ── Speakers ──────────────────────────────────────────────────────────
    def speaker_test(self, freq_hz:int=1000, duration:float=1.0) -> dict:
        r={"freq":freq_hz,"duration":duration,"status":"?"}
        try:
            rate=44100
            samples=int(rate*duration)
            data=bytearray(samples*2)
            for i in range(samples):
                v=int(32767*math.sin(2*math.pi*freq_hz*i/rate))
                struct.pack_into('<h',data,i*2,v)
            # Write WAV to temp file and play
            tmp=tempfile.NamedTemporaryFile(suffix='.wav',delete=False)
            with wave.open(tmp.name,'wb') as wf:
                wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(rate)
                wf.writeframes(bytes(data))
            tmp.close()
            if platform.system()=="Linux":
                ret=subprocess.run(["aplay","-q",tmp.name],timeout=duration+2).returncode
                r["status"]="ok" if ret==0 else "aplay not found"
            elif platform.system()=="Windows":
                import winsound
                winsound.PlaySound(tmp.name,winsound.SND_FILENAME|winsound.SND_ASYNC)
                time.sleep(duration+0.2); r["status"]="ok"
            elif platform.system()=="Darwin":
                subprocess.run(["afplay",tmp.name],timeout=duration+2)
                r["status"]="ok"
            os.unlink(tmp.name)
        except Exception as e:
            r["status"]=str(e)
        return r

    # ── Display ───────────────────────────────────────────────────────────
    def display_test(self, root:tk.Tk):
        """Opens display test windows."""
        _DisplayTestWindow(root)

DEV = DeviceTester()

# ── REAL-TIME MONITORING ────────────────────────────────────────
class FileMonitor:
    def __init__(self, callback):
        self._cb=callback
        self._watching={}
        self._active=False
        self._thread=None

    def add(self,path:str):
        try:
            st=os.stat(path)
            self._watching[path]={"mtime":st.st_mtime,"size":st.st_size}
        except: pass

    def remove(self,path:str):
        self._watching.pop(path,None)

    def start(self):
        if self._active: return
        self._active=True
        self._thread=threading.Thread(target=self._run,daemon=True)
        self._thread.start()

    def stop(self):
        self._active=False

    def _run(self):
        while self._active:
            for path,old in list(self._watching.items()):
                try:
                    st=os.stat(path)
                    if st.st_mtime!=old["mtime"] or st.st_size!=old["size"]:
                        self._cb(path,"modified",old["size"],st.st_size)
                        self._watching[path]={"mtime":st.st_mtime,"size":st.st_size}
                except FileNotFoundError:
                    self._cb(path,"deleted",old["size"],0)
                    self._watching.pop(path,None)
                except: pass
            time.sleep(1.0)

# ── SCHEDULER ───────────────────────────────────────────────────
class Scheduler:
    def __init__(self,scan_callback):
        self._cb=scan_callback
        self._jobs=[]  # {"label":str,"interval_min":int,"next":datetime,"paths":[]}
        self._active=False
        self._thread=None

    def add_job(self,label:str,interval_min:int,paths:list):
        self._jobs.append({"label":label,"interval_min":interval_min,
                           "next":datetime.now()+timedelta(minutes=interval_min),"paths":paths})

    def remove_job(self,label:str):
        self._jobs=[j for j in self._jobs if j["label"]!=label]

    def start(self):
        if self._active: return
        self._active=True
        self._thread=threading.Thread(target=self._run,daemon=True)
        self._thread.start()

    def stop(self): self._active=False

    def _run(self):
        while self._active:
            now=datetime.now()
            for job in self._jobs:
                if now>=job["next"]:
                    threading.Thread(target=self._cb,args=(job["paths"],job["label"]),daemon=True).start()
                    job["next"]=now+timedelta(minutes=job["interval_min"])
            time.sleep(30)

# ── HTML EXPORT ─────────────────────────────────────────────────
def export_html(results:list, log_entries:list, path:str):
    rows=""
    for r in results:
        status=r.get("status","?")
        icon={"ok":"OK","warn":"WARN","error":"ERROR"}.get(status,"?")
        deep=r.get("deep",{})
        threat=deep.get("threat",{})
        t_txt=threat.get("level","-")
        rows+=f"""
        <tr>
          <td>{icon}</td>
          <td title="{r['path']}">{r['name']}</td>
          <td>{r.get('type','?')}</td>
          <td>{_fmt(r.get('size',0))}</td>
          <td>{deep.get('crc32','-')}</td>
          <td>{deep.get('entropy','-')}</td>
          <td>{t_txt}</td>
          <td>{'; '.join(r.get('issues',[]))[:80] or '-'}</td>
        </tr>"""

    ok_n   = sum(1 for r in results if r.get('status')=='ok')
    warn_n = sum(1 for r in results if r.get('status')=='warn')
    err_n  = sum(1 for r in results if r.get('status')=='error')

    log_rows = "".join(
        f"<tr><td>{ts}</td><td>{lvl.upper()}</td><td>{msg}</td></tr>"
        for ts,lvl,msg in log_entries[-50:]
    )

    html=f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sonar Report — {_dt()}</title>
</head>
<body>
<h1>Sonar Report</h1>
<p>Generated {_dt()} | C-core: {"active" if CORE.available else "Python fallback"} | Signatures: {len(VDB.signatures)}</p>

<h2>Summary</h2>
<ul>
  <li>Files scanned: {len(results)}</li>
  <li>OK: {ok_n}</li>
  <li>Warnings: {warn_n}</li>
  <li>Damaged: {err_n}</li>
</ul>

<h2>Check results</h2>
<table border="1" cellpadding="4" cellspacing="0">
<thead><tr><th>Status</th><th>File</th><th>Type</th><th>Size</th><th>CRC-32</th><th>Entropy</th><th>Threat</th><th>Details</th></tr></thead>
<tbody>{rows}</tbody>
</table>

<h2>Scan log</h2>
<table border="1" cellpadding="4" cellspacing="0">
<thead><tr><th>Time</th><th>Level</th><th>Message</th></tr></thead>
<tbody>{log_rows}</tbody>
</table>

<p>Sonar v1.2 — Generated {_dt()}</p>
</body>
</html>"""
    with open(path,'w',encoding='utf-8') as f: f.write(html)

# ── WINDOWS EXTRA FEATURES ──────────────────────────────────────

class _DiffWindow(tk.Toplevel):
    """Line-by-line diff of two text files."""
    def __init__(self,parent,path1):
        super().__init__(parent)
        self.title(f"Compare — {os.path.basename(path1)}")
        self.geometry("900x620"); self.configure(bg="#FFFFFF")

        # Toolbar
        tb=tk.Frame(self,bg="#F3F3F3",height=32); tb.pack(fill="x"); tb.pack_propagate(False)
        tk.Button(tb,text="📂 Open second file…",command=self._open_second,
                  bg="#F3F3F3",fg="#1E1E1E",relief="flat",font=("Segoe UI",8),cursor="hand2"
                  ).pack(side="left",padx=6,pady=4)
        self._path1=path1; self._path2=None

        # Legend
        leg=tk.Frame(self,bg="#FFFFFF"); leg.pack(fill="x",padx=8,pady=4)
        for col,lbl in (("#D7F4D7","+ Added"),("#FBDADA","− Removed"),("#D7E8FB","  Changed")):
            tk.Label(leg,text=f"  {lbl}  ",bg=col,fg="white",font=("Segoe UI",8)).pack(side="left",padx=2)
        tk.Label(leg,text=f"  File 1: {os.path.basename(path1)}  ",
                 bg="#FFFFFF",fg="#888",font=("Segoe UI",8)).pack(side="right")

        # Text widget
        frame=tk.Frame(self,bg="#FFFFFF"); frame.pack(fill="both",expand=True,padx=6,pady=6)
        xsb=ttk.Scrollbar(frame,orient="horizontal"); ysb=ttk.Scrollbar(frame,orient="vertical")
        self._txt=tk.Text(frame,font=("Consolas",9),bg="#FFFFFF",fg="#1E1E1E",
                          wrap="none",state="disabled",
                          xscrollcommand=xsb.set,yscrollcommand=ysb.set)
        xsb.configure(command=self._txt.xview); ysb.configure(command=self._txt.yview)
        xsb.pack(side="bottom",fill="x"); ysb.pack(side="right",fill="y")
        self._txt.pack(fill="both",expand=True)
        self._txt.tag_configure("add",  background="#D7F4D7",foreground="#1B5E20")
        self._txt.tag_configure("del",  background="#FBDADA",foreground="#B71C1C")
        self._txt.tag_configure("chg",  background="#D7E8FB",foreground="#0D47A1")
        self._txt.tag_configure("eq",   foreground="#888888")
        self._txt.tag_configure("hdr",  foreground="#1565C0",font=("Consolas",9,"bold"))
        self._txt.tag_configure("lnum", foreground="#555",font=("Consolas",9))

        # Statusbar
        self._status=tk.Label(self,text="Open second file to compare",
                              bg="#007ACC",fg="white",font=("Segoe UI",8),anchor="w")
        self._status.pack(fill="x",side="bottom")

    def _open_second(self):
        p=filedialog.askopenfilename(title="Select second file to compare")
        if not p: return
        self._path2=p
        self._run_diff()

    def _run_diff(self):
        try:
            enc_list=["utf-8","cp1251","latin-1"]
            def _read(p):
                for enc in enc_list:
                    try:
                        with open(p,'r',encoding=enc) as f: return f.readlines()
                    except: pass
                with open(p,'rb') as f: return [l.decode('latin-1') for l in f.readlines()]

            lines1=_read(self._path1); lines2=_read(self._path2)
            differ=difflib.unified_diff(lines1,lines2,
                fromfile=os.path.basename(self._path1),
                tofile=os.path.basename(self._path2),lineterm='')
            diff_lines=list(differ)

            self._txt.configure(state="normal"); self._txt.delete("1.0","end")
            add_c=del_c=chg_c=0
            lnum=0
            for line in diff_lines:
                lnum+=1
                ln=f"{lnum:>5}  "
                if line.startswith("+++") or line.startswith("---"):
                    self._txt.insert("end",ln,"lnum"); self._txt.insert("end",line+"\n","hdr")
                elif line.startswith("@@"):
                    self._txt.insert("end","\n"); self._txt.insert("end",ln,"lnum")
                    self._txt.insert("end",line+"\n","hdr")
                elif line.startswith("+"):
                    add_c+=1; self._txt.insert("end",ln,"lnum"); self._txt.insert("end",line+"\n","add")
                elif line.startswith("-"):
                    del_c+=1; self._txt.insert("end",ln,"lnum"); self._txt.insert("end",line+"\n","del")
                else:
                    self._txt.insert("end",ln,"lnum"); self._txt.insert("end",line+"\n","eq")

            self._txt.configure(state="disabled")
            self._status.configure(
                text=f"  Added: +{add_c}  Removed: -{del_c}  "
                     f"Lines file 1: {len(lines1)}  file 2: {len(lines2)}")
        except Exception as e:
            messagebox.showerror("Diff error",str(e))


class _RepairWindow(tk.Toplevel):
    """File repair window."""
    def __init__(self,parent,path):
        super().__init__(parent)
        self.title(f"Repair — {os.path.basename(path)}")
        self.geometry("560x420"); self.configure(bg="#FFFFFF"); self.resizable(False,False)
        self.grab_set()

        hdr=tk.Frame(self,bg="#264F78",height=40); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text=f"  🔧 File repair: {os.path.basename(path)}",
                 bg="#264F78",fg="white",font=("Segoe UI",10,"bold")).pack(side="left",padx=8,pady=8)

        self._prog=ttk.Progressbar(self,mode="indeterminate",length=540)
        self._prog.pack(padx=10,pady=(10,4))

        self._txt=tk.Text(self,font=("Consolas",8),bg="#F7F7F7",fg="#1E1E1E",
                          state="disabled",relief="flat",padx=6,pady=4)
        self._txt.pack(fill="both",expand=True,padx=6,pady=4)
        self._txt.tag_configure("ok",  foreground="#1E8E3E")
        self._txt.tag_configure("err", foreground="#D32F2F")
        self._txt.tag_configure("info",foreground="#1565C0")

        self._btn=tk.Button(self,text="Close",command=self.destroy,
                            bg="#264F78",fg="white",font=("Segoe UI",9),relief="flat",state="disabled")
        self._btn.pack(pady=6)

        self._prog.start(10)
        threading.Thread(target=self._run,args=(path,),daemon=True).start()

    def _log(self,msg,tag="info"):
        self._txt.configure(state="normal")
        self._txt.insert("end",f"  {msg}\n",tag)
        self._txt.see("end"); self._txt.configure(state="disabled")

    def _run(self,path):
        def cb(msg): self.after(0,lambda:self._log(msg))
        result=REPAIRER.attempt_repair(path,progress_cb=cb)
        self.after(0,self._done,result)

    def _done(self,result):
        self._prog.stop()
        if result.get("success"):
            if result.get("no_changes"):
                self._log("✓ File OK or cannot be repaired","ok")
            else:
                self._log("✓ Repair successful!","ok")
        else:
            self._log(f"✗ Could not repair: {result.get('error','')}","err")
        self._btn.configure(state="normal")


class _ArchiveViewWindow(tk.Toplevel):
    """Recursive archive viewer."""
    def __init__(self,parent,path):
        super().__init__(parent)
        self.title(f"Archive structure — {os.path.basename(path)}")
        self.geometry("700x500"); self.configure(bg="#FFFFFF")

        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text=f"  📦 {os.path.basename(path)}",
                 bg="#264F78",fg="white",font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)

        self._tree=ttk.Treeview(self,show="tree headings",
                                 columns=("size","type","flag"))
        self._tree.heading("#0",  text="Name")
        self._tree.heading("size",text="Size",anchor="e")
        self._tree.heading("type",text="Type",   anchor="center")
        self._tree.heading("flag",text="",       anchor="center")
        self._tree.column("#0",  width=340)
        self._tree.column("size",width=90, anchor="e")
        self._tree.column("type",width=80, anchor="center")
        self._tree.column("flag",width=60, anchor="center")
        self._tree.tag_configure("danger",foreground="#D32F2F")
        self._tree.tag_configure("warn",  foreground="#B8860B")
        self._tree.tag_configure("dir",   foreground="#1565C0")
        vsb=ttk.Scrollbar(self,command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right",fill="y"); self._tree.pack(fill="both",expand=True)

        self._status=tk.Label(self,text="Analyzing…",bg="#007ACC",fg="white",
                              font=("Segoe UI",8),anchor="w")
        self._status.pack(fill="x",side="bottom")

        threading.Thread(target=self._analyze,args=(path,),daemon=True).start()

    def _analyze(self,path):
        result=ARCH.analyze(path)
        self.after(0,self._populate,result,path)

    def _populate(self,result,path):
        stats=result.get("stats",{})
        root_id=self._tree.insert("","end",text=f"📦 {os.path.basename(path)}",
                                   values=(stats.get("uncompressed","?"),"ZIP",""),open=True)
        entries=result.get("entries",[])
        for e in entries[:500]:
            flag="🚨" if Path(e["name"]).suffix.lower() in VDB.dangerous_ext else \
                 "⚠" if Path(e["name"]).suffix.lower() in VDB.suspicious_ext else ""
            tag="danger" if flag=="🚨" else "warn" if flag=="⚠" else "dir" if e.get("is_dir") else ""
            icon="📁" if e.get("is_dir") else "📄"
            self._tree.insert(root_id,"end",text=f"{icon} {e['name']}",
                               values=(e["size"],e["ext"],flag),tags=(tag,))
        if len(entries)>500:
            self._tree.insert(root_id,"end",text=f"… {len(entries)-500} more files")
        # Nested
        nested=result.get("nested",{})
        if nested:
            nid=self._tree.insert(root_id,"end",text="🔍 Nested archives",open=True)
            for name,sub in nested.items():
                sub_stats=sub.get("stats",{})
                self._tree.insert(nid,"end",text=f"📦 {name}",
                    values=(_fmt(0),"nested",""))
        danger=stats.get("dangerous_files",[])
        if danger:
            did=self._tree.insert("","end",text=f"🚨 Dangerous files ({len(danger)})",
                                   open=True,tags=("danger",))
            for f in danger:
                self._tree.insert(did,"end",text=f"  ⚠ {f}",tags=("danger",))
        self._status.configure(text=f"  Files: {stats.get('total_files','?')}  "
                                f"Compressed: {stats.get('compressed','?')}  "
                                f"Unpacked: {stats.get('uncompressed','?')}  "
                                f"Ratio: {stats.get('ratio','?')}"
                                +("  ⚠ ZIP BOMB!" if stats.get("zip_bomb_risk") else ""))


class _DisplayTestWindow(tk.Toplevel):
    """Display Test: dead pixels, color accuracy."""
    def __init__(self,parent):
        super().__init__(parent)
        self.title("Display Test")
        self.attributes("-fullscreen",True)
        self.configure(bg="black")
        self._colors=["#FF0000","#00FF00","#0000FF","#FFFFFF","#000000",
                      "#FFFF00","#FF00FF","#00FFFF","#808080"]
        self._idx=0
        self._canvas=tk.Canvas(self,bg="black",highlightthickness=0)
        self._canvas.pack(fill="both",expand=True)
        self._lbl=tk.Label(self,text="",bg="black",fg="white",
                           font=("Segoe UI",14),anchor="s")
        self._lbl.place(relx=0.5,rely=0.95,anchor="center")
        self.bind("<space>",self._next_color)
        self.bind("<Escape>",lambda e:self.destroy())
        self.bind("<Button-1>",self._next_color)
        self._show()

    def _show(self):
        c=self._colors[self._idx%len(self._colors)]
        self.configure(bg=c); self._canvas.configure(bg=c)
        names=["Red","Green","Blue","White","Black","Yellow","Purple","Cyan","Gray"]
        txt=f"{names[self._idx%len(names)]}  —  {self._idx+1}/{len(self._colors)}  · Space/Click = next  · Esc = exit"
        fg="#000" if c in("#FFFFFF","#FFFF00","#00FFFF","#00FF00") else "#FFF"
        self._lbl.configure(text=txt,bg=c,fg=fg)
        # Grid for dead pixel detection
        if c=="#000000":
            self._canvas.delete("all")
            W,H=self.winfo_screenwidth(),self.winfo_screenheight()
            for x in range(0,W,50): self._canvas.create_line(x,0,x,H,fill="#111",width=1)
            for y in range(0,H,50): self._canvas.create_line(0,y,W,y,fill="#111",width=1)
            self._lbl.configure(text=txt+" · Look for bright pixels on black background")

    def _next_color(self,event=None):
        self._idx+=1; self._canvas.delete("all"); self._show()


class _MetaWindow(tk.Toplevel):
    """Metadata viewer."""
    def __init__(self,parent,path):
        super().__init__(parent)
        self.title(f"Metadata — {os.path.basename(path)}")
        self.geometry("560x440"); self.configure(bg="#FFFFFF")
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text=f"  🏷 {os.path.basename(path)}",
                 bg="#264F78",fg="white",font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)
        self._tree=ttk.Treeview(self,columns=("val",),show="tree headings")
        self._tree.heading("#0",  text="Field")
        self._tree.heading("val", text="Value")
        self._tree.column("#0",width=180,minwidth=100)
        self._tree.column("val",width=340,minwidth=100)
        vsb=ttk.Scrollbar(self,command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right",fill="y"); self._tree.pack(fill="both",expand=True,padx=4,pady=4)
        threading.Thread(target=self._load,args=(path,),daemon=True).start()

    def _load(self,path):
        meta=META.read(path)
        self.after(0,self._populate,meta)

    def _populate(self,meta):
        fmt=meta.pop("format","Metadata")
        root=self._tree.insert("","end",text=fmt,open=True)
        if not meta or (len(meta)==1 and "error" in meta):
            self._tree.insert(root,"end",text="(no metadata)",values=(meta.get("error",""),))
            return
        for k,v in meta.items():
            self._tree.insert(root,"end",text=k,values=(str(v)[:200],))


class _StegoWindow(tk.Toplevel):
    """LSB steganography analysis window."""
    def __init__(self,parent,path):
        super().__init__(parent)
        self.title(f"LSB Analysis — {os.path.basename(path)}")
        self.geometry("500x360"); self.configure(bg="#FFFFFF"); self.grab_set()
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔍 Steganography Analysis (LSB)",
                 bg="#264F78",fg="white",font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)
        self._txt=tk.Text(self,font=("Consolas",9),bg="#F7F7F7",fg="#1E1E1E",
                          state="disabled",relief="flat",padx=8,pady=6)
        self._txt.pack(fill="both",expand=True,padx=6,pady=6)
        self._txt.tag_configure("ok",   foreground="#1E8E3E",font=("Consolas",9,"bold"))
        self._txt.tag_configure("warn", foreground="#B8860B",font=("Consolas",9,"bold"))
        self._txt.tag_configure("err",  foreground="#D32F2F",font=("Consolas",9,"bold"))
        self._txt.tag_configure("key",  foreground="#1565C0",font=("Consolas",9,"bold"))
        self._txt.tag_configure("val",  foreground="#1E1E1E")
        tk.Label(self,text="Analyzing…",bg="#007ACC",fg="white",
                 font=("Segoe UI",8),anchor="w").pack(fill="x",side="bottom")
        threading.Thread(target=self._run,args=(path,),daemon=True).start()

    def _run(self,path):
        r=STEGO.analyze(path)
        self.after(0,self._show,r)

    def _show(self,r):
        t=self._txt; t.configure(state="normal"); t.delete("1.0","end")
        if "error" in r:
            t.insert("end",f"Error: {r['error']}\n","err"); t.configure(state="disabled"); return
        def kv(k,v,tag="val"): t.insert("end",f"  {k:<22}","key"); t.insert("end",f"{v}\n",tag)
        kv("Size:",        r.get("size","?"))
        kv("Pixels:",      r.get("total_pixels","?"))
        t.insert("end","\n")
        t.insert("end","  LSB randomness per channel:\n","key")
        kv("  Red LSB:",   r.get("lsb_r","?"))
        kv("  Green LSB:", r.get("lsb_g","?"))
        kv("  Blue LSB:",  r.get("lsb_b","?"))
        kv("  Average:",   r.get("lsb_avg","?"))
        t.insert("end","\n")
        kv("Chi² (R-channel):",r.get("chi2_r","—"))
        kv("Chi² verdict:",  r.get("chi2_verdict","—"))
        t.insert("end","\n")
        lvl=r.get("level","ok")
        tag={"ok":"ok","warn":"warn","info":"warn"}.get(lvl,"ok")
        t.insert("end",f"  VERDICT: {r.get('verdict','?')}\n",(tag,"key"))
        score=r.get("suspicion_score",0)
        bar="█"*int(score*20)+"░"*(20-int(score*20))
        kv("Suspicion index:", f"{score:.2f}  [{bar}]")
        t.configure(state="disabled")


class _ProcessWindow(tk.Toplevel):
    """Processes and autorun."""
    def __init__(self,parent):
        super().__init__(parent)
        self.title("Process & Autorun Scan")
        self.geometry("820x560"); self.configure(bg="#FFFFFF")
        nb=ttk.Notebook(self); nb.pack(fill="both",expand=True,padx=4,pady=4)

        # Processes tab
        f_proc=tk.Frame(nb,bg="#FFFFFF"); nb.add(f_proc,text="  Processes  ")
        cols=("pid","name","cpu","mem","status","flag")
        self._ptree=ttk.Treeview(f_proc,columns=cols,show="headings")
        for c,w,t in (("pid",55,"PID"),("name",160,"Name"),("cpu",60,"CPU%"),
                      ("mem",80,"Memory"),("status",80,"Status"),("flag",80,"")):
            self._ptree.heading(c,text=t); self._ptree.column(c,width=w,anchor="center" if c!="name" else "w")
        self._ptree.tag_configure("sus",foreground="#D32F2F",font=("Consolas",8,"bold"))
        vsb=ttk.Scrollbar(f_proc,command=self._ptree.yview)
        self._ptree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right",fill="y"); self._ptree.pack(fill="both",expand=True)

        # Autorun tab
        f_auto=tk.Frame(nb,bg="#FFFFFF"); nb.add(f_auto,text="  Autorun  ")
        cols2=("location","name","value","flag")
        self._atree=ttk.Treeview(f_auto,columns=cols2,show="headings")
        for c,w,t in (("location",180,"Location"),("name",120,"Name"),
                      ("value",280,"Value"),("flag",60,"")):
            self._atree.heading(c,text=t); self._atree.column(c,width=w)
        self._atree.tag_configure("sus",foreground="#D32F2F",font=("Consolas",8,"bold"))
        vsb2=ttk.Scrollbar(f_auto,command=self._atree.yview)
        self._atree.configure(yscrollcommand=vsb2.set)
        vsb2.pack(side="right",fill="y"); self._atree.pack(fill="both",expand=True)

        sb=tk.Label(self,text="  Loading…",bg="#007ACC",fg="white",font=("Segoe UI",8),anchor="w")
        sb.pack(fill="x",side="bottom"); self._sb=sb
        threading.Thread(target=self._load,daemon=True).start()

    def _load(self):
        procs=PROC_SCANNER.scan_processes()
        runs=PROC_SCANNER.scan_autorun()
        self.after(0,self._populate,procs,runs)

    def _populate(self,procs,runs):
        for p in procs:
            if "error" in p: continue
            flag="🚨 SUSPICIOUS." if p.get("suspicious") else ""
            tag=("sus",) if p.get("suspicious") else ()
            self._ptree.insert("","end",
                values=(p["pid"],p["name"],p.get("cpu","?"),p["mem"],p["status"],flag),tags=tag)
        sus=sum(1 for p in procs if p.get("suspicious"))
        for a in runs:
            flag="🚨" if a.get("suspicious") else ""
            tag=("sus",) if a.get("suspicious") else ()
            self._atree.insert("","end",
                values=(a.get("location","?")[:40],a.get("name","?")[:30],
                        a.get("value","?")[:60],flag),tags=tag)
        self._sb.configure(text=f"  Processes: {len(procs)}  Suspicious: {sus}  Autorun entries: {len(runs)}")


class _NetworkWindow(tk.Toplevel):
    """Network test + repair."""
    _STATUS_TEXT={
        "ok":         ("✓ Соединение в норме","ok"),
        "unstable":   ("⚠ Соединение нестабильно (большие потери пакетов)","warn"),
        "no_dns":     ("⚠ Интернет есть, но DNS не отвечает","warn"),
        "no_internet":("✗ Нет интернета (нет ответа от внешних серверов)","err"),
        "no_gateway": ("✗ Нет связи с роутером / шлюзом","err"),
        "no_adapter": ("✗ Активный сетевой адаптер не найден","err"),
        "unknown":    ("? Статус неизвестен","warn"),
    }

    def __init__(self,parent):
        super().__init__(parent)
        self.title("Network Test — Wi-Fi / Ethernet")
        self.geometry("560x480"); self.configure(bg="#FFFFFF")
        self._last_result=None
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  📡 Network Diagnostics",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)

        self._status_var=tk.StringVar(value="Нажмите «Проверить», чтобы начать")
        self._status_lbl=tk.Label(self,textvariable=self._status_var,bg="#FFFFFF",
                                   font=("Segoe UI",10,"bold"),anchor="w",padx=10,pady=4)
        self._status_lbl.pack(fill="x")

        self._prog=ttk.Progressbar(self,mode="indeterminate"); self._prog.pack(fill="x",padx=8,pady=4)
        self._txt=tk.Text(self,font=("Consolas",9),bg="#F7F7F7",fg="#1E1E1E",
                          state="disabled",relief="flat",padx=8,pady=4)
        self._txt.pack(fill="both",expand=True,padx=6,pady=4)
        self._txt.tag_configure("ok",   foreground="#1E8E3E")
        self._txt.tag_configure("warn", foreground="#B8860B")
        self._txt.tag_configure("err",  foreground="#C0392B")
        self._txt.tag_configure("key",  foreground="#1565C0",font=("Consolas",9,"bold"))

        btnrow=tk.Frame(self,bg="#FFFFFF"); btnrow.pack(pady=6)
        self._btn=tk.Button(btnrow,text="▶ Проверить",command=self._start,
                      bg="#264F78",fg="white",font=("Segoe UI",9),relief="flat",cursor="hand2",width=16)
        self._btn.pack(side="left",padx=4)
        self._fix_btn=tk.Button(btnrow,text="🔧 Попытка починить",command=self._start_repair,
                      bg="#B8860B",fg="white",font=("Segoe UI",9),relief="flat",cursor="hand2",
                      width=20,state="disabled")
        self._fix_btn.pack(side="left",padx=4)

    def _start(self):
        self._btn.configure(state="disabled"); self._fix_btn.configure(state="disabled")
        self._prog.start(8)
        self._status_var.set("Проверка…")
        self._txt.configure(state="normal"); self._txt.delete("1.0","end"); self._txt.configure(state="disabled")
        threading.Thread(target=self._run,daemon=True).start()

    def _run(self):
        r=DEV.network_test(progress_cb=lambda m:self.after(0,self._log,m))
        self.after(0,self._done,r)

    def _log(self,msg,tag=None):
        if tag is None:
            tag="err" if ("✗" in msg or "не отвечает" in msg or "нет ответа" in msg) else \
                ("warn" if "⚠" in msg else "ok")
        self._txt.configure(state="normal")
        self._txt.insert("end",f"  {msg}\n",tag)
        self._txt.see("end"); self._txt.configure(state="disabled")

    def _done(self,r):
        self._last_result=r
        self._prog.stop(); self._btn.configure(state="normal")
        text,tag=self._STATUS_TEXT.get(r["status"],self._STATUS_TEXT["unknown"])
        self._status_var.set(text)
        self._status_lbl.configure(fg={"ok":"#1E8E3E","warn":"#B8860B","err":"#C0392B"}[tag])
        # Enable "fix" button whenever the connection isn't fully healthy
        self._fix_btn.configure(state=("normal" if r["status"]!="ok" else "disabled"))

        self._txt.configure(state="normal")
        self._txt.insert("end","\n  ─── Итог ───\n","key")
        kv=lambda k,v,t="ok": (self._txt.insert("end",f"  {k:<22}","key"),self._txt.insert("end",f"{v}\n",t))
        kv("Ping:",        f"{r['ping_ms']} ms ({r['ping_host']})" if r['ping_ms'] else "нет ответа",
           "ok" if r['ping_ms'] else "err")
        kv("Потери пакетов:", f"{r['packet_loss']}%" if r['packet_loss'] is not None else "—",
           "ok" if (r['packet_loss'] or 0)<10 else "warn")
        kv("Шлюз:",        "OK" if r['gateway_ok'] else ("нет ответа" if r['gateway_ok'] is False else "—"),
           "ok" if r['gateway_ok'] else "err")
        kv("DNS:",         "OK" if r['dns_ok'] else ("не отвечает" if r['dns_ok'] is False else "—"),
           "ok" if r['dns_ok'] else "err")
        kv("Download:",    f"{r['download_mbps']} Mbps" if r['download_mbps'] else "—")
        self._txt.configure(state="disabled")

    def _start_repair(self):
        if not messagebox.askyesno("Попытка починить",
                "Будет выполнен сброс сетевых настроек (DNS, IP, Winsock/адаптер).\n"
                "Это может на несколько секунд прервать соединение. Продолжить?",
                parent=self):
            return
        self._btn.configure(state="disabled"); self._fix_btn.configure(state="disabled")
        self._prog.start(8)
        self._status_var.set("Выполняется попытка починить соединение…")
        self._txt.configure(state="normal")
        self._txt.insert("end","\n  ─── 🔧 Попытка починить ───\n","key")
        self._txt.configure(state="disabled")
        threading.Thread(target=self._run_repair,daemon=True).start()

    def _run_repair(self):
        res=DEV.network_repair(progress_cb=lambda m:self.after(0,self._log,m))
        self.after(0,self._repair_done,res)

    def _repair_done(self,res):
        ok_count=sum(1 for _,ok,_ in res["actions"] if ok)
        total=len([a for a in res["actions"] if a[1] is not None])
        self._log(f"Готово: {ok_count}/{total} действий выполнено успешно","ok")
        self._done(res["retest"])


class _BatteryWindow(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent)
        self.title("Battery Status")
        self.geometry("420x300"); self.configure(bg="#FFFFFF"); self.grab_set()
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔋 Battery",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)
        self._txt=tk.Text(self,font=("Consolas",9),bg="#F7F7F7",fg="#1E1E1E",
                          state="disabled",relief="flat",padx=8,pady=6)
        self._txt.pack(fill="both",expand=True,padx=6,pady=6)
        self._txt.tag_configure("key",foreground="#1565C0",font=("Consolas",9,"bold"))
        self._txt.tag_configure("val",foreground="#1E1E1E")
        self._txt.tag_configure("ok", foreground="#1E8E3E")
        self._txt.tag_configure("warn",foreground="#B8860B")
        threading.Thread(target=self._load,daemon=True).start()

    def _load(self):
        r=DEV.battery()
        self.after(0,self._show,r)

    def _show(self,r):
        t=self._txt; t.configure(state="normal"); t.delete("1.0","end")
        def kv(k,v,tag="val"): t.insert("end",f"  {k:<22}","key"); t.insert("end",f"{v}\n",tag)
        if not r.get("available"):
            t.insert("end","  Battery not found or access denied\n","warn")
        else:
            pct=r.get("percent",0)
            tag="ok" if pct>50 else "warn" if pct>20 else "err"
            bar="█"*int(pct/5)+"░"*(20-int(pct/5))
            kv("Charge:", f"{pct}%  [{bar}]",tag)
            kv("Power:", "AC power" if r.get("plugged") else "battery")
            if r.get("time_left"): kv("Remaining:", r["time_left"])
            if r.get("cycles"):    kv("Charge cycles:", r["cycles"])
            if r.get("health"):    kv("Battery health:", f"{r['health']}%",
                                      "ok" if r["health"]>80 else "warn")
        t.configure(state="disabled")


class _BTWindow(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent)
        self.title("Bluetooth")
        self.geometry("480x360"); self.configure(bg="#FFFFFF")
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔵 Bluetooth",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)
        self._prog=ttk.Progressbar(self,mode="indeterminate"); self._prog.pack(fill="x",padx=8,pady=4)
        self._txt=tk.Text(self,font=("Consolas",9),bg="#F7F7F7",fg="#1E1E1E",
                          state="disabled",relief="flat",padx=8,pady=4)
        self._txt.pack(fill="both",expand=True,padx=6,pady=4)
        self._txt.tag_configure("ok",  foreground="#1E8E3E")
        self._txt.tag_configure("info",foreground="#1565C0")
        btn=tk.Button(self,text="🔍 Scan",command=self._start,
                      bg="#264F78",fg="white",font=("Segoe UI",9),relief="flat",cursor="hand2")
        btn.pack(pady=6); self._btn=btn

    def _start(self):
        self._btn.configure(state="disabled"); self._prog.start(8)
        self._txt.configure(state="normal"); self._txt.delete("1.0","end"); self._txt.configure(state="disabled")
        threading.Thread(target=self._run,daemon=True).start()

    def _run(self):
        r=DEV.bluetooth_scan(progress_cb=lambda m:self.after(0,self._log,m,"info"))
        self.after(0,self._done,r)

    def _log(self,msg,tag="info"):
        self._txt.configure(state="normal")
        self._txt.insert("end",f"  {msg}\n",tag); self._txt.see("end"); self._txt.configure(state="disabled")

    def _done(self,r):
        self._prog.stop(); self._btn.configure(state="normal")
        self._log(f"Devices found: {len(r['devices'])}","ok")
        for d in r["devices"]:
            self._log(f"  • {d.get('name','?')}  {d.get('mac','')}","ok")


class _USBWindow(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent)
        self.title("USB Devices")
        self.geometry("560x380"); self.configure(bg="#FFFFFF")
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔌 USB Ports & Devices",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)
        cols=("bus","dev","id","name")
        self._tree=ttk.Treeview(self,columns=cols,show="headings")
        for c,w,t in (("bus",40,"Bus"),("dev",40,"Dev"),("id",100,"ID"),("name",340,"Device")):
            self._tree.heading(c,text=t); self._tree.column(c,width=w)
        vsb=ttk.Scrollbar(self,command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right",fill="y"); self._tree.pack(fill="both",expand=True)
        self._sb=tk.Label(self,text="  Loading…",bg="#007ACC",fg="white",font=("Segoe UI",8),anchor="w")
        self._sb.pack(fill="x",side="bottom")
        threading.Thread(target=self._load,daemon=True).start()

    def _load(self):
        devs=DEV.usb_info()
        self.after(0,self._populate,devs)

    def _populate(self,devs):
        for d in devs:
            self._tree.insert("","end",
                values=(d.get("bus",""),d.get("dev",""),d.get("id",""),d.get("name","?")))
        self._sb.configure(text=f"  Devices: {len(devs)}")


class _SpeakerWindow(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent)
        self.title("Speaker Test")
        self.geometry("420x300"); self.configure(bg="#FFFFFF"); self.grab_set()
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔊 Speaker Test",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)

        body=tk.Frame(self,bg="#FFFFFF"); body.pack(fill="both",expand=True,padx=16,pady=8)
        tk.Label(body,text="Frequency (Hz):",bg="#FFFFFF",fg="#1E1E1E",
                 font=("Segoe UI",9)).grid(row=0,column=0,sticky="w",pady=4)
        self._freq=tk.Scale(body,from_=100,to=8000,orient="horizontal",length=280,
                            bg="#FFFFFF",fg="#1E1E1E",troughcolor="#264F78",highlightthickness=0)
        self._freq.set(1000); self._freq.grid(row=0,column=1,pady=4)

        tk.Label(body,text="Duration (sec):",bg="#FFFFFF",fg="#1E1E1E",
                 font=("Segoe UI",9)).grid(row=1,column=0,sticky="w",pady=4)
        self._dur=tk.Scale(body,from_=0.5,to=5.0,resolution=0.5,orient="horizontal",length=280,
                           bg="#FFFFFF",fg="#1E1E1E",troughcolor="#264F78",highlightthickness=0)
        self._dur.set(1.0); self._dur.grid(row=1,column=1,pady=4)

        freqs=[(100,"Sub-bass"),(300,"Bass"),(1000,"Mid"),(3000,"High-mid"),(8000,"Highs")]
        pf=tk.Frame(body,bg="#FFFFFF"); pf.grid(row=2,column=0,columnspan=2,pady=8)
        for hz,lbl in freqs:
            tk.Button(pf,text=f"{lbl}\n{hz} Hz",
                      command=lambda h=hz:self._play(h,1.0),
                      bg="#264F78",fg="white",font=("Segoe UI",8),relief="flat",
                      cursor="hand2",width=9).pack(side="left",padx=3)

        self._status=tk.Label(self,text="  Ready",bg="#007ACC",fg="white",
                              font=("Segoe UI",8),anchor="w")
        self._status.pack(fill="x",side="bottom")

        tk.Button(self,text="▶ Play",command=lambda:self._play(int(self._freq.get()),self._dur.get()),
                  bg="#264F78",fg="white",font=("Segoe UI",10),relief="flat",cursor="hand2"
                  ).pack(pady=6)

    def _play(self,freq,dur):
        self._status.configure(text=f"  Playing {freq} Hz…")
        threading.Thread(target=self._do_play,args=(freq,dur),daemon=True).start()

    def _do_play(self,freq,dur):
        r=DEV.speaker_test(freq,dur)
        self.after(0,lambda:self._status.configure(
            text=f"  {freq} Hz — {'OK' if r['status']=='ok' else r['status']}"))


class _SchedulerWindow(tk.Toplevel):
    """Scan Scheduler."""
    def __init__(self,parent,scheduler,file_paths):
        super().__init__(parent)
        self.title("Scan Scheduler")
        self.geometry("520x340"); self.configure(bg="#FFFFFF")
        self._sched=scheduler; self._paths=file_paths
        hdr=tk.Frame(self,bg="#264F78",height=32); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  ⏰ Scheduler",bg="#264F78",fg="white",
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=8,pady=4)

        body=tk.Frame(self,bg="#FFFFFF"); body.pack(fill="both",expand=True,padx=16,pady=8)

        tk.Label(body,text="Job name:",bg="#FFFFFF",fg="#1E1E1E",
                 font=("Segoe UI",9)).grid(row=0,column=0,sticky="w",pady=4)
        self._name=tk.Entry(body,font=("Segoe UI",9),bg="#F0F0F0",fg="#1E1E1E",width=24)
        self._name.insert(0,"Auto-scan"); self._name.grid(row=0,column=1,pady=4,padx=8)

        tk.Label(body,text="Interval (minutes):",bg="#FFFFFF",fg="#1E1E1E",
                 font=("Segoe UI",9)).grid(row=1,column=0,sticky="w",pady=4)
        self._interval=tk.Scale(body,from_=5,to=1440,resolution=5,orient="horizontal",
                                 length=200,bg="#FFFFFF",fg="#1E1E1E",troughcolor="#264F78",
                                 highlightthickness=0)
        self._interval.set(60); self._interval.grid(row=1,column=1,pady=4,padx=8)

        tk.Label(body,text=f"Files queued: {len(file_paths)}",
                 bg="#FFFFFF",fg="#888",font=("Segoe UI",8)).grid(row=2,column=0,columnspan=2,pady=4)

        btn_frame=tk.Frame(body,bg="#FFFFFF"); btn_frame.grid(row=3,column=0,columnspan=2,pady=12)
        tk.Button(btn_frame,text="➕ Add job",command=self._add,
                  bg="#264F78",fg="white",font=("Segoe UI",9),relief="flat",cursor="hand2"
                  ).pack(side="left",padx=4)
        tk.Button(btn_frame,text="🗑 Remove selected",command=self._remove,
                  bg="#C62828",fg="white",font=("Segoe UI",9),relief="flat",cursor="hand2"
                  ).pack(side="left",padx=4)

        self._listbox=tk.Listbox(body,bg="#F0F0F0",fg="#1E1E1E",font=("Consolas",8),height=5)
        self._listbox.grid(row=4,column=0,columnspan=2,sticky="ew",pady=4)
        self._refresh()

        self._sb=tk.Label(self,text="  Scheduler active" if scheduler._active else "  Scheduler stopped",
                          bg="#007ACC",fg="white",font=("Segoe UI",8),anchor="w")
        self._sb.pack(fill="x",side="bottom")

    def _add(self):
        name=self._name.get().strip()
        if not name: return
        interval=int(self._interval.get())
        self._sched.add_job(name,interval,list(self._paths))
        if not self._sched._active: self._sched.start()
        self._refresh()
        self._sb.configure(text=f"  Job «{name}» added (every {interval} min)")

    def _remove(self):
        sel=self._listbox.curselection()
        if not sel: return
        item=self._listbox.get(sel[0])
        label=item.split("]")[0].strip("[ ")
        self._sched.remove_job(label)
        self._refresh()

    def _refresh(self):
        self._listbox.delete(0,"end")
        for job in self._sched._jobs:
            nxt=job["next"].strftime("%H:%M")
            self._listbox.insert("end",f"[ {job['label']} ]  every {job['interval_min']}min  next: {nxt}")

# ── THEMES ──────────────────────────────────────────────────────
THEME = {"bg":"#FFFFFF","bg2":"#F0F0F0","fg":"#1E1E1E","fg2":"#5A5A5A",
         "accent":"#264F78","accent_fg":"#FFFFFF","toolbar":"#F3F3F3","statusbar":"#007ACC",
         "sep":"#CCCCCC","tree_ok":"#1E8E3E","tree_warn":"#B8860B","tree_err":"#D32F2F",
         "tree_pend":"#888888","log_ok":"#1E8E3E","log_warn":"#B8860B","log_err":"#D32F2F",
         "log_info":"#1565C0","log_threat":"#B71C1C","detail_bg":"#F0F0F0","btn_hover":"#E5E5E5","pane_sash":"#D9D9D9"}

# ── MAIN APPLICATION ────────────────────────────────────────────
class SonarApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self._lang  = "RU"
        self._theme = "light"
        self.geometry("1100x700"); self.minsize(800,520)

        self._q            = queue.Queue()
        self._checker_inst = None   # FileChecker — created in _build_ui
        self._running      = False
        self._results      = []
        self._file_paths   = []
        self._kbd_active   = False
        self._mouse_active = False
        self._log_entries  = []
        self._monitor      = FileMonitor(self._on_file_changed)
        self._scheduler    = Scheduler(self._scheduled_scan)

        self._build_ui()
        self._poll_queue()
        self._log("Sonar v1.2 started","info")
        c_st="active" if CORE.available else "Python fallback"
        self._log(f"C-core: {c_st}","ok" if CORE.available else "warn")
        self._log(f"Virus DB: {len(VDB.signatures)} signatures","info")
        self._log(f"Pillow: {'available' if HAS_PIL else 'missing (pip install Pillow)'}","info")
        self._log(f"Mutagen: {'available' if HAS_MUTAGEN else 'missing (pip install mutagen)'}","info")
        self._log(f"psutil: {'available' if HAS_PSUTIL else 'missing (pip install psutil)'}","info")

    @property
    def T(self): return THEME

    def _build_ui(self):
        T=self.T
        self.title("Sonar v1.2 — File & Device Diagnostics")
        self.configure(bg=T["bg"])

        style=ttk.Style(self)
        for th in ("vista","winnative","clam","default"):
            try: style.theme_use(th); break
            except: pass

        # ── Menu ──────────────────────────────────────────────────────────
        mb=tk.Menu(self,tearoff=0,bg=T["toolbar"],fg=T["fg"])

        mf=tk.Menu(mb,tearoff=0,bg=T["toolbar"],fg=T["fg"])
        mf.add_command(label="Add files…",  accelerator="Ctrl+O",command=self._add_files)
        mf.add_command(label="Add folder…",  accelerator="Ctrl+D",command=self._add_folder)
        mf.add_separator()
        mf.add_command(label="Save report…", accelerator="Ctrl+S",command=self._export_report)
        mf.add_separator()
        mf.add_command(label="Exit",command=self.quit)
        mb.add_cascade(label="File",menu=mf)

        ms=tk.Menu(mb,tearoff=0,bg=T["toolbar"],fg=T["fg"])
        ms.add_command(label="Scan",       accelerator="F5",command=self._start_scan)
        ms.add_command(label="Deep analysis",  accelerator="F6",command=self._start_deep)
        ms.add_separator()
        ms.add_command(label="Clear list",command=self._clear)
        ms.add_command(label="Scheduler…",command=lambda:_SchedulerWindow(self,self._scheduler,self._file_paths))
        mb.add_cascade(label="Scan",menu=ms)

        mv=tk.Menu(mb,tearoff=0,bg=T["toolbar"],fg=T["fg"])
        mv.add_command(label="Selected details",accelerator="Enter",command=self._show_details)
        mb.add_cascade(label="View",menu=mv)

        mt=tk.Menu(mb,tearoff=0,bg=T["toolbar"],fg=T["fg"])
        mt.add_command(label="🔎 Processes and autorun",command=lambda:_ProcessWindow(self))
        mt.add_command(label="⏰ Scheduler",command=lambda:_SchedulerWindow(self,self._scheduler,self._file_paths))
        mb.add_cascade(label="Tools",menu=mt)

        mh=tk.Menu(mb,tearoff=0,bg=T["toolbar"],fg=T["fg"])
        mh.add_command(label="About",command=self._about)
        mb.add_cascade(label="Help",menu=mh)
        self.config(menu=mb)

        self.bind("<Control-o>",lambda e:self._add_files())
        self.bind("<Control-d>",lambda e:self._add_folder())
        self.bind("<Control-s>",lambda e:self._export_report())
        self.bind("<F5>",lambda e:self._start_scan())
        self.bind("<F6>",lambda e:self._start_deep())
        self.bind("<Return>",lambda e:self._show_details())

        # ── Toolbar ────────────────────────────────────────────────────────
        tb=tk.Frame(self,bg=T["toolbar"],relief="raised",bd=1,height=32)
        tb.pack(fill="x"); tb.pack_propagate(False)
        for txt,cmd in [("📄 Files",self._add_files),("📁 Folder",self._add_folder),
                        ("▶ Scan [F5]",self._start_scan),("🔬 Deep [F6]",self._start_deep),
                        ("✕ Clear",self._clear),("💾 Report",self._export_report),
                        ("🔎 Processes",lambda:_ProcessWindow(self)),
                        ("⏰ Scheduler",lambda:_SchedulerWindow(self,self._scheduler,self._file_paths))]:
            self._mk_tb_btn(txt,cmd,tb)
            if txt in("📁 Folder","🔬 Deep [F6]","💾 Report"):
                tk.Frame(tb,bg=T["sep"],width=1).pack(side="left",fill="y",padx=3,pady=3)

        # C-core badge
        c_txt="✓ C-core" if CORE.available else "✗ C-core"
        c_col=T["log_ok"] if CORE.available else T["log_err"]
        tk.Label(tb,text=f"  {c_txt}  |  DB: {len(VDB.signatures)} sigs.  ",
                 bg=T["toolbar"],fg=c_col,font=("Consolas",8)).pack(side="right",padx=4)

        # ── Notebook ──────────────────────────────────────────────────────
        nb=ttk.Notebook(self); nb.pack(fill="both",expand=True)
        f_files=tk.Frame(nb,bg=T["bg"])
        f_dev  =tk.Frame(nb,bg=T["bg"])
        f_logs =tk.Frame(nb,bg=T["bg"])
        nb.add(f_files,text="   Files   ")
        nb.add(f_dev,  text="   Devices   ")
        nb.add(f_logs, text="   Log   ")

        self._build_files_tab(f_files)
        self._build_devices_tab(f_dev)
        self._build_logs_tab(f_logs)

        # ── Statusbar ─────────────────────────────────────────────────────
        sbar=tk.Frame(self,bg=T["statusbar"],relief="sunken",bd=1,height=20)
        sbar.pack(fill="x",side="bottom"); sbar.pack_propagate(False)
        fg_s=T["accent_fg"] if self._theme=="dark" else T["fg"]
        self._status_left =tk.Label(sbar,text="  Ready",bg=T["statusbar"],fg=fg_s,font=("Segoe UI",8),anchor="w")
        self._status_left.pack(side="left",fill="x",expand=True,padx=4)
        self._status_right=tk.Label(sbar,text="",bg=T["statusbar"],fg=fg_s,font=("Segoe UI",8),anchor="e")
        self._status_right.pack(side="right",padx=4)

    def _mk_tb_btn(self,text,cmd,parent):
        T=self.T
        btn=tk.Button(parent,text=f" {text} ",command=cmd,
                      relief="flat",bg=T["toolbar"],fg=T["fg"],
                      activebackground=T["btn_hover"],activeforeground=T["fg"],
                      font=("Segoe UI",8),cursor="hand2",bd=0,highlightthickness=0)
        btn.pack(side="left",padx=1,pady=4)
        btn.bind("<Enter>",lambda e,b=btn:b.config(relief="raised",bg=T["btn_hover"]))
        btn.bind("<Leave>",lambda e,b=btn:b.config(relief="flat",bg=T["toolbar"]))

    def _full_rebuild(self):
        data={"paths":list(self._file_paths),"results":list(self._results),"logs":list(self._log_entries)}
        for w in self.winfo_children(): w.destroy()
        self.config(menu=tk.Menu(self))
        self._build_ui()
        self._file_paths=data["paths"]; self._results=data["results"]; self._log_entries=data["logs"]
        for path in data["paths"]:
            r=next((x for x in data["results"] if x["path"]==path),None)
            if r: self._update_row(r)
            else: self._insert_pending(path)
        for ts,lvl,msg in data["logs"]:
            self._log_text.configure(state="normal")
            self._log_text.insert("end",f"[{ts}] {msg}\n",lvl)
            self._log_text.see("end"); self._log_text.configure(state="disabled")
        self._poll_queue()

    # ─── Tab: Files ────────────────────────────────────────────────────
    def _build_files_tab(self,parent):
        T=self.T
        pane=tk.PanedWindow(parent,orient="horizontal",sashwidth=5,bg=T["pane_sash"],handlesize=0)
        pane.pack(fill="both",expand=True)

        left=tk.Frame(pane,bg=T["bg"]); pane.add(left,width=720)
        cols=("st","name","type","size","detail")
        self._tree=ttk.Treeview(left,columns=cols,show="headings",selectmode="browse")
        for c,w,t,a in (("st",26,"","center"),("name",220,"File","w"),("type",90,"Type","center"),
                        ("size",80,"Size","e"),("detail",380,"Result","w")):
            self._tree.heading(c,text=t,anchor=a); self._tree.column(c,width=w,anchor=a)
        self._tree.tag_configure("ok",   foreground=T["tree_ok"])
        self._tree.tag_configure("warn", foreground=T["tree_warn"])
        self._tree.tag_configure("err",  foreground=T["tree_err"])
        self._tree.tag_configure("pending",foreground=T["tree_pend"])
        vsb=ttk.Scrollbar(left,orient="vertical",  command=self._tree.yview)
        hsb=ttk.Scrollbar(left,orient="horizontal",command=self._tree.xview)
        self._tree.configure(yscrollcommand=vsb.set,xscrollcommand=hsb.set)
        vsb.pack(side="right",fill="y"); hsb.pack(side="bottom",fill="x")
        self._tree.pack(fill="both",expand=True)
        self._tree.bind("<Double-1>",    lambda e:self._show_details())
        self._tree.bind("<<TreeviewSelect>>",self._on_sel)
        self._tree.bind("<Button-3>",    self._context_menu)

        # Drag & Drop (TkDND if available, else fallback)
        try:
            self._tree.drop_target_register("DND_Files")
            self._tree.dnd_bind("<<Drop>>", self._on_drop)
        except: pass

        right=tk.Frame(pane,bg=T["bg"]); pane.add(right,width=360)
        tk.Label(right,text="File details",bg=T["bg"],fg=T["fg"],
                 font=("Segoe UI",9,"bold")).pack(anchor="w",padx=8,pady=(6,2))
        ttk.Separator(right,orient="horizontal").pack(fill="x",padx=4)
        self._detail_text=tk.Text(right,font=("Consolas",8),state="disabled",relief="flat",
                                   bg=T["detail_bg"],fg=T["fg"],wrap="word",cursor="arrow",padx=6,pady=4)
        self._detail_text.pack(fill="both",expand=True,padx=2,pady=2)

        prog=tk.Frame(parent,bg=T["bg"]); prog.pack(fill="x",padx=4,pady=(2,4))
        self._prog=ttk.Progressbar(prog,mode="determinate",length=260)
        self._prog.pack(side="left",padx=(0,6))
        self._prog_lbl=tk.Label(prog,text="Ready",bg=T["bg"],fg=T["fg"],font=("Segoe UI",8))
        self._prog_lbl.pack(side="left")
        self._prog_cnt=tk.Label(prog,text="",bg=T["bg"],fg=T["fg"],font=("Segoe UI",8))
        self._prog_cnt.pack(side="right")

    def _context_menu(self,event):
        sel=self._tree.identify_row(event.y)
        if not sel: return
        self._tree.selection_set(sel)
        path=sel
        ext=Path(path).suffix.lower()
        is_img=ext in('.png','.jpg','.jpeg','.gif','.bmp','.webp')
        is_txt=ext in('.txt','.log','.py','.js','.json','.xml','.csv','.ini','.cfg','.md','.html','.c','.h','.cpp')
        is_arch=ext in('.zip','.docx','.xlsx','.pptx','.jar','.apk','.gz','.bz2','.tar','.7z','.rar')

        menu=tk.Menu(self,tearoff=0,bg=self.T["toolbar"],fg=self.T["fg"])
        menu.add_command(label="🔬 Deep analysis",command=lambda:self._start_deep_single(path))
        menu.add_command(label="🏷 Metadata",command=lambda:_MetaWindow(self,path))
        if is_arch:
            menu.add_command(label="📦 Archive structure",command=lambda:_ArchiveViewWindow(self,path))
        if is_img:
            menu.add_command(label="🔍 LSB Steganography",command=lambda:_StegoWindow(self,path))
        if is_txt:
            menu.add_command(label="📊 Compare line-by-line…",command=lambda:_DiffWindow(self,path))
        menu.add_separator()
        menu.add_command(label="🔧 Attempt repair",command=lambda:_RepairWindow(self,path))
        menu.add_separator()
        monitor_lbl="🔴 Stop monitoring" if path in self._monitor._watching else "👁 Monitor file"
        def toggle_mon():
            if path in self._monitor._watching:
                self._monitor.remove(path); self._log(f"Monitoring stopped: {os.path.basename(path)}","info")
            else:
                self._monitor.add(path); self._monitor.start(); self._log(f"Monitoring: {os.path.basename(path)}","info")
        menu.add_command(label=monitor_lbl,command=toggle_mon)
        menu.add_separator()
        menu.add_command(label="📋 Copy path",command=lambda:(self.clipboard_clear(),self.clipboard_append(path)))
        menu.post(event.x_root,event.y_root)

    def _on_drop(self,event):
        files=self.tk.splitlist(event.data)
        for f in files:
            if os.path.exists(f) and f not in self._file_paths:
                self._file_paths.append(f); self._insert_pending(f)

    def _on_sel(self,event):
        sel=self._tree.selection()
        if not sel: return
        res=next((r for r in self._results if r["path"]==sel[0]),None)
        if res: self._render_details(res)

    def _render_details(self,res):
        T=self.T; t=self._detail_text
        t.configure(state="normal"); t.delete("1.0","end")
        t.configure(bg=T["detail_bg"],fg=T["fg"])
        t.tag_configure("head",font=("Segoe UI",8,"bold"),foreground=T["fg"])
        t.tag_configure("ok",  foreground=T["tree_ok"])
        t.tag_configure("warn",foreground=T["tree_warn"])
        t.tag_configure("err", foreground=T["tree_err"])
        t.tag_configure("key", font=("Consolas",7,"bold"),foreground=T["fg2"])
        t.tag_configure("val", font=("Consolas",7),foreground=T["fg"])
        t.tag_configure("thr_d",foreground=T["log_threat"],font=("Segoe UI",8,"bold"))
        t.tag_configure("thr_w",foreground=T["log_warn"])
        t.tag_configure("thr_c",foreground=T["log_ok"])

        sm={"ok":("✓ OK","ok"),"warn":("⚠ WARNING","warn"),"error":("✗ DAMAGED","err")}
        st,sg=sm.get(res["status"],("?",""))
        t.insert("end",f"{st}\n",(sg,"head"))
        t.insert("end","─"*30+"\n","key")
        for k,v in (("Name",res["name"]),("Type",res["type"]),("Size",_fmt(res["size"]))):
            t.insert("end",f"{k:<9}","key"); t.insert("end",f"{v}\n","val")

        if res.get("details"):
            t.insert("end","\nDetails:\n","head")
            for d in res["details"]: t.insert("end",f"  {d}\n","val")
        if res.get("issues"):
            t.insert("end","\nIssues:\n","head")
            for i in res["issues"]: t.insert("end",f"  ⚠ {i}\n",("warn","val"))

        deep=res.get("deep")
        if deep:
            t.insert("end","\n🔬 Deep analysis:\n","head")
            for k,v in (("CRC-32",deep.get("crc32","—")),
                        ("Entropy",f"{deep.get('entropy','—')} bits/bytes"),
                        ("",deep.get("entropy_hint","")),
                        ("Nulls",f"{deep.get('null_ratio','—')}%  {deep.get('null_hint','')}"),
                        ("ASCII",f"{deep.get('ascii_ratio','—')}% ({deep.get('content_class','—')})"),
                        ("C-core","✓ yes" if deep.get("c_backend") else "✗ Python fallback")):
                t.insert("end",f"  {k:<10}","key"); t.insert("end",f"{v}\n","val")

            if deep.get("top_bytes"):
                t.insert("end","\n  Top-5 bytes:\n","head")
                for b in deep["top_bytes"]:
                    t.insert("end",f"    {b['byte']} '{b['char']}' → {b['count']} ({b['pct']}%)\n","val")

            if deep.get("extra"):
                t.insert("end","\n  Structure:\n","head")
                for line in deep["extra"]: t.insert("end",f"  {line}\n","val")

            # Metadata
            meta=deep.get("meta")
            if meta and len(meta)>1:
                t.insert("end","\n🏷 Metadata:\n","head")
                for k,v in list(meta.items())[:12]:
                    if k!="format": t.insert("end",f"  {k[:18]:<18}","key"); t.insert("end",f"{str(v)[:60]}\n","val")

            # Steganography
            stego=deep.get("stego")
            if stego and not stego.get("error"):
                t.insert("end","\n🔍 LSB analysis:\n","head")
                t.insert("end",f"  {stego.get('verdict','?')}\n",
                         {"ok":"thr_c","warn":"thr_w"}.get(stego.get("level","ok"),"val"))
                t.insert("end",f"  LSB avg: {stego.get('lsb_avg','?')}  χ²: {stego.get('chi2_r','?')}\n","val")

            # Archive
            arch=deep.get("archive")
            if arch and "stats" in arch:
                s=arch["stats"]
                t.insert("end","\n📦 Archive:\n","head")
                t.insert("end",f"  Files: {s.get('total_files','?')}  {s.get('compressed','?')}→{s.get('uncompressed','?')}\n","val")
                if s.get("zip_bomb_risk"):
                    t.insert("end","  🚨 ZIP BOMB!\n","thr_d")

            # Threats
            threat=deep.get("threat")
            if threat:
                t.insert("end","\n🛡 Threat analysis:\n","head")
                lvl=threat.get("level","clean")
                lbl={"clean":"✓ No threats detected","suspicious":"⚠ Suspicious","danger":"🚨 LIKELY MALICIOUS"}[lvl]
                tag={"clean":"thr_c","suspicious":"thr_w","danger":"thr_d"}[lvl]
                t.insert("end",f"  {lbl}\n",(tag,"head"))
                for r2 in threat.get("reasons",[]):
                    tg="thr_d" if "🚨" in r2 else "thr_w"
                    t.insert("end",f"  {r2}\n",tg)

            probs=deep.get("verdict_problems",[])
            if probs:
                t.insert("end","\n  ⚠ Summary:\n",("warn","head"))
                for p in probs: t.insert("end",f"    • {p}\n",("warn","val"))
            else:
                t.insert("end","\n  ✓ No problems found\n",("ok","val"))

        t.configure(state="disabled")

    # ─── Tab: Devices ──────────────────────────────────────────────
    def _build_devices_tab(self,parent):
        T=self.T
        pane=tk.PanedWindow(parent,orient="horizontal",sashwidth=5,bg=T["pane_sash"],handlesize=0)
        pane.pack(fill="both",expand=True)

        # Device tree (left)
        left=tk.Frame(pane,bg=T["bg"]); pane.add(left,width=280)

        hdr2=tk.Frame(left,bg=T["accent"],height=28); hdr2.pack(fill="x"); hdr2.pack_propagate(False)
        tk.Label(hdr2,text="  Device Manager",bg=T["accent"],fg=T["accent_fg"],
                 font=("Segoe UI",8,"bold")).pack(side="left",padx=6,pady=4)

        self._dev_tree=ttk.Treeview(left,show="tree",selectmode="browse")
        dev_vsb=ttk.Scrollbar(left,command=self._dev_tree.yview)
        self._dev_tree.configure(yscrollcommand=dev_vsb.set)
        dev_vsb.pack(side="right",fill="y"); self._dev_tree.pack(fill="both",expand=True)

        # Populate tree — like Windows Device Manager
        root_id=self._dev_tree.insert("","end",text="💻 This computer",open=True)

        inp_id=self._dev_tree.insert(root_id,"end",text="🖱 Input devices",open=True)
        self._kbd_node  =self._dev_tree.insert(inp_id,"end",text="⌨  Keyboard  [not tested]")
        self._mouse_node=self._dev_tree.insert(inp_id,"end",text="🖱  Mouse  [not tested]")

        aud_id=self._dev_tree.insert(root_id,"end",text="🔊 Sound",open=True)
        self._mic_node  =self._dev_tree.insert(aud_id,"end",text="🎤  Microphone  [not tested]")
        self._spk_node  =self._dev_tree.insert(aud_id,"end",text="🔊  Speakers  [not tested]")

        disp_id=self._dev_tree.insert(root_id,"end",text="🖥 Display",open=True)
        self._disp_node =self._dev_tree.insert(disp_id,"end",text="🖥  Display Test  [not tested]")

        pc_id=self._dev_tree.insert(root_id,"end",text="🖥 PC / System",open=True)
        self._bat_node  =self._dev_tree.insert(pc_id,"end",text="🔋  Battery  [not tested]")
        self._net_node  =self._dev_tree.insert(pc_id,"end",text="📡  Wi-Fi / Network  [not tested]")
        self._bt_node   =self._dev_tree.insert(pc_id,"end",text="🔵  Bluetooth  [not tested]")

        usb_id=self._dev_tree.insert(root_id,"end",text="🔌 USB",open=True)
        self._usb_node  =self._dev_tree.insert(usb_id,"end",text="🔌  USB ports  [not tested]")

        self._dev_tree.bind("<Double-1>",self._dev_tree_action)
        self._dev_tree.bind("<Button-3>",self._dev_tree_context_menu)

        # Right part — properties
        right=tk.Frame(pane,bg=T["bg"]); pane.add(right)

        phdr=tk.Frame(right,bg=T["bg2"],relief="groove",bd=1,height=28)
        phdr.pack(fill="x"); phdr.pack_propagate(False)
        tk.Label(phdr,text=" Device properties",bg=T["bg2"],fg=T["fg"],
                 font=("Segoe UI",8,"bold")).pack(side="left",padx=6,pady=4)

        self._icon_cache={}  # keeps PhotoImage refs alive
        tiles=tk.Frame(right,bg=T["bg"]); tiles.pack(fill="x",padx=8,pady=10)
        for col in range(3): tiles.columnconfigure(col,weight=1)

        DEVICE_TILES=[
            ("keyboard",   "⌨",  "Keyboard",    0,0,self._test_keyboard),
            ("mouse",      "🖱",  "Mouse",       0,1,self._test_mouse),
            ("microphone", "🎤",  "Microphone",  0,2,self._test_mic),
            ("speakers",   "🔊",  "Speakers",    1,0,self._test_speakers),
            ("display",    "🖥",  "Display",     1,1,lambda:DEV.display_test(self)),
            ("battery",    "🔋",  "Battery",     1,2,lambda:_BatteryWindow(self)),
            ("network",    "📡",  "Network",     2,0,lambda:_NetworkWindow(self)),
            ("bluetooth",  "🔵",  "Bluetooth",   2,1,lambda:_BTWindow(self)),
            ("usb",        "🔌",  "USB",         2,2,lambda:_USBWindow(self)),
        ]
        cards_by_key={}
        for key,emoji,label,row,col,cmd in DEVICE_TILES:
            cards_by_key[key]=self._dev_tile(tiles,key,emoji,label,row,col,cmd)
        self._kbd_card   = cards_by_key["keyboard"]
        self._mouse_card = cards_by_key["mouse"]
        self._mic_card   = cards_by_key["microphone"]

        ehdr=tk.Frame(right,bg=T["bg2"],relief="groove",bd=1,height=22)
        ehdr.pack(fill="x",padx=8,pady=(8,0)); ehdr.pack_propagate(False)
        tk.Label(ehdr,text=" Event log",bg=T["bg2"],fg=T["fg"],
                 font=("Segoe UI",8,"bold")).pack(side="left",padx=6)

        lf=tk.Frame(right,bg=T["bg"]); lf.pack(fill="both",expand=True,padx=8,pady=(0,8))
        self._dev_log=tk.Text(lf,height=6,state="disabled",font=("Consolas",8),
                               relief="sunken",bd=1,wrap="word",cursor="arrow",
                               bg=T["detail_bg"],fg=T["fg"])
        lsb=ttk.Scrollbar(lf,command=self._dev_log.yview)
        self._dev_log.configure(yscrollcommand=lsb.set)
        lsb.pack(side="right",fill="y"); self._dev_log.pack(fill="both",expand=True)
        for tag,col in (("ok",T["log_ok"]),("warn",T["log_warn"]),("err",T["log_err"]),("info",T["log_info"])):
            self._dev_log.tag_config(tag,foreground=col)

        self.bind("<KeyPress>",    self._on_key)
        self.bind("<ButtonPress>", self._on_click)
        self.bind("<MouseWheel>",  self._on_scroll)
        self.bind("<Button-4>",    self._on_scroll)
        self.bind("<Button-5>",    self._on_scroll)

    def _load_dev_icon(self,key,size=44):
        """Load Assets/<key>.png (or .jpg/.jpeg/.webp) for a device tile, if available."""
        if not HAS_PIL: return None
        if key in self._icon_cache: return self._icon_cache[key]
        for suffix in (".png",".jpg",".jpeg",".webp"):
            p=ASSETS_DIR/f"{key}{suffix}"
            if p.exists():
                try:
                    img=Image.open(p).convert("RGBA").resize((size,size),Image.LANCZOS)
                    photo=ImageTk.PhotoImage(img)
                    self._icon_cache[key]=photo
                    return photo
                except Exception:
                    return None
        self._icon_cache[key]=None
        return None

    def _dev_tile(self,parent,key,emoji,label,row,col,cmd):
        """A square, clickable device button: image (or emoji fallback) + name + status."""
        T=self.T
        SIZE=108
        tile=tk.Frame(parent,bg=T["bg2"],relief="raised",bd=1,
                       width=SIZE,height=SIZE,cursor="hand2")
        tile.grid(row=row,column=col,padx=6,pady=6)
        tile.pack_propagate(False)

        icon=self._load_dev_icon(key,size=44)
        if icon is not None:
            icon_lbl=tk.Label(tile,image=icon,bg=T["bg2"])
        else:
            icon_lbl=tk.Label(tile,text=emoji,font=("Segoe UI",22),bg=T["bg2"],fg=T["fg"])
        icon_lbl.pack(pady=(10,2))

        name_lbl=tk.Label(tile,text=label,font=("Segoe UI",8,"bold"),bg=T["bg2"],fg=T["fg"])
        name_lbl.pack()

        status_var=tk.StringVar(value="not tested")
        status_lbl=tk.Label(tile,textvariable=status_var,font=("Segoe UI",7),
                             bg=T["bg2"],fg=T["fg2"],wraplength=SIZE-10,justify="center")
        status_lbl.pack(pady=(2,6))

        widgets=(tile,icon_lbl,name_lbl,status_lbl)
        def on_enter(_e=None):
            for w in widgets: w.configure(bg=T["btn_hover"])
            tile.configure(relief="solid")
        def on_leave(_e=None):
            for w in widgets: w.configure(bg=T["bg2"])
            tile.configure(relief="raised")
        for w in widgets:
            w.bind("<Button-1>",lambda e:cmd())
            w.bind("<Enter>",on_enter)
            w.bind("<Leave>",on_leave)

        return {"sv":status_var,"frame":tile}

    def _dev_tree_context_menu(self,event):
        """RMB on a device node: Devices -> (node) -> RMB -> Test / Try to fix."""
        node=self._dev_tree.identify_row(event.y)
        if not node: return
        self._dev_tree.selection_set(node)
        T=self.T
        menu=tk.Menu(self,tearoff=0,bg=T["bg2"],fg=T["fg"],
                     activebackground=T["accent"],activeforeground=T["accent_fg"])
        if node==self._net_node:
            menu.add_command(label="🔍 Проверить интернет",command=lambda:_NetworkWindow(self))
            menu.add_command(label="🔧 Попытка починить",command=lambda:self._repair_network_quick())
        else:
            menu.add_command(label="🔍 Тест",command=lambda:self._dev_tree_action_for(node))
        try:
            menu.tk_popup(event.x_root,event.y_root)
        finally:
            menu.grab_release()

    def _dev_tree_action_for(self,node):
        """Run the same action as a double-click, for an explicit node."""
        self._dev_tree.selection_set(node)
        self._dev_tree_action(None)

    def _repair_network_quick(self):
        """Devices -> Интернет -> ПКМ -> Попытка починить (без открытия отдельного окна)."""
        self._dev_log_write("Сеть: запущена попытка починить соединение…","info")
        self._dev_tree.item(self._net_node,text="📡  Wi-Fi / Network  [⏳ fixing]")
        threading.Thread(target=self._repair_network_worker,daemon=True).start()

    def _repair_network_worker(self):
        def progress(msg):
            self._q.put(("net_repair_log",msg))
        res=DEV.network_repair(progress_cb=progress)
        self._q.put(("net_repair_done",res))

    def _dev_tree_action(self,event):
        sel=self._dev_tree.selection()
        if not sel: return
        node=sel[0]
        if node==self._bat_node:   _BatteryWindow(self)
        elif node==self._net_node: _NetworkWindow(self)
        elif node==self._bt_node:  _BTWindow(self)
        elif node==self._usb_node: _USBWindow(self)
        elif node==self._spk_node: self._test_speakers()
        elif node==self._disp_node:DEV.display_test(self)
        elif node==self._kbd_node: self._test_keyboard()
        elif node==self._mouse_node:self._test_mouse()
        elif node==self._mic_node: self._test_mic()

    # ─── Tab: Logs ────────────────────────────────────────────────────
    def _build_logs_tab(self,parent):
        T=self.T
        hdr=tk.Frame(parent,bg=T["accent"],height=28); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  📋 Sonar System Log",bg=T["accent"],fg=T["accent_fg"],
                 font=("Segoe UI",9,"bold")).pack(side="left",padx=6,pady=4)
        tb2=tk.Frame(parent,bg=T["toolbar"],relief="raised",bd=1,height=26)
        tb2.pack(fill="x"); tb2.pack_propagate(False)
        for txt,cmd in [("🗑 Clear",self._clear_logs),("💾 Export…",self._export_logs)]:
            tk.Button(tb2,text=txt,command=cmd,relief="flat",bg=T["toolbar"],fg=T["fg"],
                      font=("Segoe UI",8),cursor="hand2").pack(side="left",padx=4,pady=2)
        frame=tk.Frame(parent,bg=T["bg"]); frame.pack(fill="both",expand=True,padx=6,pady=6)
        self._log_text=tk.Text(frame,font=("Consolas",8),state="disabled",relief="sunken",bd=1,
                                wrap="word",cursor="arrow",bg=T["detail_bg"],fg=T["fg"])
        lsb=ttk.Scrollbar(frame,command=self._log_text.yview)
        self._log_text.configure(yscrollcommand=lsb.set)
        lsb.pack(side="right",fill="y"); self._log_text.pack(fill="both",expand=True)
        for tag,col in (("ok",T["log_ok"]),("warn",T["log_warn"]),("err",T["log_err"]),
                        ("info",T["log_info"]),("threat",T["log_threat"])):
            self._log_text.tag_config(tag,foreground=col)

    def _log(self,text,level="info"):
        ts=_ts()
        self._log_entries.append((ts,level,text))
        try:
            self._log_text.configure(state="normal")
            pfx={"ok":"[OK]  ","warn":"[WRN] ","err":"[ERR] ","info":"[INF] ","threat":"[!!!] "}.get(level,"[   ] ")
            self._log_text.insert("end",f"[{ts}] {pfx}{text}\n",level)
            self._log_text.see("end"); self._log_text.configure(state="disabled")
        except: pass

    def _clear_logs(self):
        self._log_entries.clear()
        self._log_text.configure(state="normal"); self._log_text.delete("1.0","end"); self._log_text.configure(state="disabled")

    def _export_logs(self):
        p=filedialog.asksaveasfilename(defaultextension=".txt",filetypes=[("Log","*.txt")],
                                        initialfile=f"sonar_log_{datetime.now():%Y%m%d_%H%M%S}")
        if not p: return
        with open(p,"w",encoding="utf-8") as f:
            for ts,lvl,msg in self._log_entries: f.write(f"[{ts}] [{lvl.upper()}] {msg}\n")

    # ─── File operations ────────────────────────────────────────────────
    def _add_files(self):
        paths=filedialog.askopenfilenames(title="Add files")
        added=0
        for p in paths:
            if p not in self._file_paths:
                self._file_paths.append(p); self._insert_pending(p); added+=1
        if added:
            msg=f"Added {added} file(s). Total: {len(self._file_paths)}"
            self._set_status(msg); self._log(msg,"info")

    def _add_folder(self):
        folder=filedialog.askdirectory(title="Add folder")
        if not folder: return
        added=0
        for root,dirs,files in os.walk(folder):
            dirs[:]=[d for d in dirs if not d.startswith('.')]
            for fn in files:
                p=os.path.join(root,fn)
                if p not in self._file_paths:
                    self._file_paths.append(p); self._insert_pending(p); added+=1
        msg=f"Added from folder: {added} file(s)"
        self._set_status(msg); self._log(msg,"info")

    def _insert_pending(self,path):
        name=os.path.basename(path)
        size=_fmt(os.path.getsize(path)) if os.path.exists(path) else "—"
        self._tree.insert("","end",iid=path,values=("○",name,"—",size,"waiting…"),tags=("pending",))

    def _start_scan(self):
        if self._running: messagebox.showinfo("Sonar","Please wait for current operation."); return
        if not self._file_paths: messagebox.showinfo("Sonar","Add files first."); return
        self._running=True; self._results=[]
        self._prog.configure(maximum=len(self._file_paths),value=0)
        self._prog_lbl.configure(text="Scan…")
        self._prog_cnt.configure(text="")
        self._log(f"Scan {len(self._file_paths)} files…","info")
        threading.Thread(target=self._scan_worker,daemon=True).start()

    def _scan_worker(self):
        total=len(self._file_paths)
        for i,path in enumerate(self._file_paths):
            result=self._quick_check(path)
            self._results.append(result)
            self._q.put(("result",i+1,total,result))
        self._q.put(("done",total))

    def _quick_check(self,path) -> dict:
        """Quick check without C-core."""
        r={"path":path,"name":os.path.basename(path),"size":0,"type":"?","status":"ok","issues":[],"details":[]}
        try: r["size"]=os.stat(path).st_size
        except OSError as e: r["status"]="error"; r["issues"].append(str(e)); return r
        if r["size"]==0: r["status"]="warn"; r["issues"].append("File is empty"); return r
        r["type"]=self._detect_type(path)
        ext=Path(path).suffix.lower()
        try:
            if ext in('.zip','.docx','.xlsx','.pptx','.odt','.jar','.apk'):
                ok,d=self._check_zip(path)
            elif ext in('.tar','.tgz'): ok,d=self._check_tar(path)
            elif ext=='.gz': ok,d=self._check_gz(path)
            elif ext in('.jpg','.jpeg','.png'): ok,d=self._check_image(path)
            elif ext=='.pdf': ok,d=self._check_pdf(path)
            elif ext=='.wav': ok,d=self._check_wav(path)
            else: ok,d=self._check_generic(path)
            r["details"].append(d); r["status"]="ok" if ok else "error"
            if not ok: r["issues"].append(d)
        except Exception as e: r["details"].append(str(e)); r["status"]="error"
        return r

    def _start_deep(self):
        sel=self._tree.selection()
        if not sel:
            if not self._file_paths: messagebox.showinfo("Sonar","Add files first."); return
            # Multi-threaded analysis of all files
            if self._running: return
            self._running=True
            paths=list(self._file_paths)
            self._results=[]
            self._prog.configure(maximum=len(paths)*10,value=0)
            self._prog_lbl.configure(text="🔬 Deep analysis…")
            self._log(f"Multi-threaded deep analysis of {len(paths)} files","info")
            threading.Thread(target=self._deep_all_worker,args=(paths,),daemon=True).start()
        else:
            self._start_deep_single(sel[0])

    def _start_deep_single(self,path):
        if self._running: messagebox.showinfo("Sonar","Please wait."); return
        self._running=True; self._prog.configure(maximum=10,value=0)
        self._prog_lbl.configure(text=f"🔬 {os.path.basename(path)}")
        self._log(f"Deep analysis: {os.path.basename(path)}","info")
        threading.Thread(target=self._deep_worker,args=(path,),daemon=True).start()

    def _deep_worker(self,path):
        def prog(done,total,name): self._q.put(("dp",done,total,name))
        result=self._deep_analyze(path,prog)
        existing=next((r for r in self._results if r["path"]==path),None)
        if existing: existing.update(result)
        else: self._results.append(result)
        self._q.put(("deep_done",result))

    def _deep_all_worker(self,paths):
        """Multi-threaded — pool of 4 workers."""
        q2=queue.Queue()
        for p in paths: q2.put(p)
        total=len(paths); done_count=[0]
        lock=threading.Lock()

        def worker():
            while True:
                try: path=q2.get(timeout=0.5)
                except: break
                r=self._deep_analyze(path,None)
                with lock:
                    self._results.append(r)
                    done_count[0]+=1
                    self._q.put(("result",done_count[0],total,r))
                q2.task_done()

        threads=[threading.Thread(target=worker,daemon=True) for _ in range(min(4,total))]
        for t in threads: t.start()
        for t in threads: t.join()
        self._q.put(("deep_all_done",total))

    def _deep_analyze(self,path,prog_cb):
        """Full analysis: everything Sonar can do."""
        steps=["Basic check","CRC-32","Entropy","Nulls/ASCII","Histogram",
               "Metadata","Archive","Steganography","Threats","Report"]
        def step(i,n):
            if prog_cb: prog_cb(i+1,len(steps),n)
            time.sleep(0.03)

        r=self._quick_check(path); r["deep"]={}
        step(0,steps[0])
        step(1,steps[1])
        r["deep"]["crc32"]=f"{CORE.crc32(path):#010x}"
        step(2,steps[2])
        ent=CORE.entropy(path); r["deep"]["entropy"]=round(ent,4)
        r["deep"]["entropy_hint"]=("Very high — compressed/encrypted" if ent>7.5 else
                                   "High — compression/mixed" if ent>6 else
                                   "Medium — text/structured" if ent>4 else "Low — text/pattern")
        step(3,steps[3])
        null_r=CORE.null_ratio(path); r["deep"]["null_ratio"]=round(null_r*100,2)
        r["deep"]["null_hint"]="Many nulls — corruption?" if null_r>0.3 else "Normal"
        asc_r=CORE.ascii_ratio(path);  r["deep"]["ascii_ratio"]=round(asc_r*100,2)
        r["deep"]["content_class"]="text" if asc_r>0.8 else "binary"
        step(4,steps[4])
        hist=CORE.histogram(path); total_b=sum(hist)
        top5=sorted(range(256),key=lambda i:hist[i],reverse=True)[:5]
        r["deep"]["histogram"]=hist
        r["deep"]["top_bytes"]=[{"byte":f"0x{b:02X}","char":chr(b) if 0x20<=b<=0x7E else "·",
                                  "count":hist[b],"pct":round(hist[b]/total_b*100,2) if total_b else 0}
                                 for b in top5]
        step(5,steps[5])
        r["deep"]["meta"]=META.read(path)
        step(6,steps[6])
        ext=Path(path).suffix.lower()
        if ext in('.zip','.docx','.xlsx','.pptx','.jar','.apk','.gz','.tar','.7z','.rar'):
            r["deep"]["archive"]=ARCH.analyze(path)
        step(7,steps[7])
        if ext in('.png','.jpg','.jpeg','.bmp','.webp') and HAS_PIL:
            r["deep"]["stego"]=STEGO.analyze(path)
        step(8,steps[8])
        try:
            with open(path,'rb') as f: first64k=f.read(65536)
        except: first64k=b''
        r["deep"]["threat"]=threat_scan(path,ent,null_r,first64k)
        step(9,steps[9])
        probs=list(r["issues"])
        if null_r>0.5: probs.append("Many null bytes")
        r["deep"]["verdict_problems"]=probs
        r["deep"]["c_backend"]=CORE.available
        return r

    def _scheduled_scan(self,paths,label):
        self._log(f"⏰ Scheduler: launch «{label}»","info")
        for path in paths:
            if os.path.exists(path):
                r=self._quick_check(path)
                self._q.put(("sched_result",r,label))

    def _on_file_changed(self,path,event,old_size,new_size):
        msg=f"File modified: {os.path.basename(path)} ({_fmt(old_size)}→{_fmt(new_size)})" if event=="modified" else f"File deleted: {os.path.basename(path)}"
        self._q.put(("monitor_event",path,event,msg))

    # ─── Queue ──────────────────────────────────────────────────────────
    def _poll_queue(self):
        try:
            while True:
                msg=self._q.get_nowait(); tag=msg[0]
                if tag=="result":
                    _,done,total,r=msg; self._update_row(r)
                    self._prog.configure(value=done)
                    self._prog_lbl.configure(text=f"Checked: {done}/{total}")
                elif tag=="done":
                    self._scan_done(msg[1])
                elif tag=="dp":
                    _,done,total,name=msg; self._prog.configure(value=done)
                    self._prog_lbl.configure(text=f"🔬 {name}"); self._set_status(name)
                elif tag=="deep_done":
                    r=msg[1]; self._update_row(r); self._running=False
                    self._prog_lbl.configure(text="Deep analysis complete")
                    self._set_status("Deep analysis complete")
                    self._render_details(r)
                    threat=r.get("deep",{}).get("threat",{})
                    if threat and threat.get("level")!="clean":
                        lvl="threat" if threat["level"]=="danger" else "warn"
                        self._log(f"THREAT: {r['name']} — {threat['level'].upper()}",lvl)
                        for r2 in threat.get("reasons",[]): self._log(f"  {r2}",lvl)
                    else:
                        self._log(f"✓ {r['name']} — clean","ok")
                elif tag=="deep_all_done":
                    n=msg[1]; self._running=False
                    ok=sum(1 for r in self._results if r.get("status")=="ok")
                    err=sum(1 for r in self._results if r.get("status")=="error")
                    self._prog_lbl.configure(text=f"Ready: {n} files")
                    self._prog_cnt.configure(text=f"✓{ok}  ✗{err}")
                    self._log(f"Multi-threaded analysis complete: {n} files, errors: {err}","ok" if not err else "warn")
                elif tag=="sched_result":
                    _,r,label=msg; self._update_row(r)
                    if r.get("status")!="ok":
                        self._log(f"⏰ [{label}] PROBLEM: {r['name']}","warn")
                elif tag=="monitor_event":
                    _,path,event,msg2=msg; self._log(f"👁 {msg2}","warn")
                elif tag=="mic_result":
                    _,text,log_text,log_tag=msg
                    self._mic_card["sv"].set(text)
                    self._mic_card.get("btn_ref") and self._mic_card["btn_ref"].configure(state="normal")
                    self._dev_log_write(log_text,log_tag)
                    self._log(f"Microphone: {log_text}",log_tag)
                    self._dev_tree.item(self._mic_node,text=f"🎤  Microphone  [{'✓' if log_tag=='ok' else '⚠'} {text}]")
                elif tag=="net_repair_log":
                    self._dev_log_write(msg[1],"info")
                elif tag=="net_repair_done":
                    res=msg[1]; retest=res.get("retest") or {}
                    ok_count=sum(1 for _,ok,_ in res["actions"] if ok)
                    total=len([a for a in res["actions"] if a[1] is not None])
                    status=retest.get("status","unknown")
                    is_ok=status=="ok"
                    self._dev_log_write(
                        f"Сеть: попытка починить завершена ({ok_count}/{total} действий) — статус: {status}",
                        "ok" if is_ok else "warn")
                    self._log(f"Network repair: {ok_count}/{total} actions, status={status}","ok" if is_ok else "warn")
                    ping=retest.get("ping_ms")
                    label=f"[{'✓' if is_ok else '⚠'} {status}" + (f", {ping}ms]" if ping else "]")
                    self._dev_tree.item(self._net_node,text=f"📡  Wi-Fi / Network  {label}")
        except queue.Empty: pass
        self.after(80,self._poll_queue)

    def _update_row(self,r):
        icon={"ok":"✓","warn":"⚠","error":"✗"}.get(r.get("status","?"),"?")
        detail=(r.get("details",[])+r.get("issues",[])+["—"])[0]
        deep=r.get("deep",{})
        if deep:
            threat=deep.get("threat",{})
            lvl=threat.get("level","clean")
            pfx={"clean":"[🔬] ","suspicious":"⚠ ","danger":"🚨 "}[lvl]
            detail=pfx+detail
        tag={"ok":"ok","warn":"warn","error":"err"}.get(r.get("status",""),"pending")
        try:
            self._tree.item(r["path"],values=(icon,r["name"],r.get("type","?"),_fmt(r.get("size",0)),detail),tags=(tag,))
        except: pass

    def _scan_done(self,total):
        self._running=False
        ok=sum(1 for r in self._results if r.get("status")=="ok")
        warn=sum(1 for r in self._results if r.get("status")=="warn")
        err=sum(1 for r in self._results if r.get("status")=="error")
        self._prog_lbl.configure(text=f"Ready: {total}")
        self._prog_cnt.configure(text=f"✓{ok}  ⚠{warn}  ✗{err}"+(f"  Damaged: {err}" if err else "  All OK"))
        msg=f"Scan: {total} files. OK:{ok} WARN:{warn} ERR:{err}"
        self._set_status(msg); self._log(msg,"ok" if not err else "warn")

    def _clear(self):
        if self._running: messagebox.showinfo("Sonar","Please wait."); return
        self._file_paths.clear(); self._results.clear()
        for item in self._tree.get_children(): self._tree.delete(item)
        self._detail_text.configure(state="normal"); self._detail_text.delete("1.0","end"); self._detail_text.configure(state="disabled")
        self._prog_lbl.configure(text="Ready"); self._prog_cnt.configure(text=""); self._prog.configure(value=0)
        self._set_status("List cleared"); self._log("List cleared","info")

    def _show_details(self):
        sel=self._tree.selection()
        if not sel: return
        path=sel[0]
        res=next((r for r in self._results if r["path"]==path),None)
        if res: self._render_details(res)
        else: self._start_deep_single(path)

    def _export_report(self):
        if not self._results: messagebox.showinfo("Sonar","No data. Run a scan first."); return
        p=filedialog.asksaveasfilename(
            defaultextension=".html",
            filetypes=[("HTML report","*.html"),("JSON","*.json"),("TXT","*.txt")],
            initialfile=f"sonar_report_{datetime.now():%Y%m%d_%H%M%S}")
        if not p: return
        if p.endswith(".html"):
            export_html(self._results,self._log_entries,p)
        elif p.endswith(".json"):
            safe=[{k:v for k,v in r.items() if k!="deep" or not isinstance(v,dict) or "histogram" not in v} for r in self._results]
            with open(p,"w",encoding="utf-8") as f: json.dump(safe,f,ensure_ascii=False,indent=2,default=str)
        else:
            with open(p,"w",encoding="utf-8") as f:
                f.write(f"SONAR v1.2 Report  {_dt()}\n{'='*70}\n\n")
                for r in self._results:
                    s={"ok":"OK","warn":"WARN","error":"DAMAGED"}.get(r.get("status"),"?")
                    f.write(f"[{s}] {r['path']}\n  Type:{r.get('type','?')}  Size:{_fmt(r.get('size',0))}\n")
                    for d in r.get("details",[]): f.write(f"  • {d}\n")
                    for i in r.get("issues",[]): f.write(f"  ! {i}\n")
                    deep=r.get("deep")
                    if deep:
                        f.write(f"  CRC:{deep.get('crc32')}  Entropy:{deep.get('entropy')}\n")
                        threat=deep.get("threat",{})
                        if threat.get("level")!="clean":
                            f.write(f"  THREAT:{threat.get('level','?').upper()}\n")
                            for r2 in threat.get("reasons",[]): f.write(f"    {r2}\n")
                    f.write("\n")
        self._log(f"Report saved: {p}","ok")
        messagebox.showinfo("Sonar",f"Report saved:\n{p}")

    # ─── Devices ───────────────────────────────────────────────────────
    def _dev_log_write(self,text,tag="info"):
        self._dev_log.configure(state="normal")
        self._dev_log.insert("end",f"[{_ts()}]  {text}\n",tag)
        self._dev_log.see("end"); self._dev_log.configure(state="disabled")

    def _test_keyboard(self):
        self._kbd_active=True
        self._dev_log_write("Press any key…","info")
        self._dev_tree.item(self._kbd_node,text="⌨  Keyboard  [⏳ waiting]")

    def _on_key(self,event):
        if self._kbd_active:
            self._kbd_active=False
            key=event.keysym
            self._dev_log_write(f"Keyboard: «{key}» — OK","ok")
            self._log(f"Keyboard: «{key}»","ok")
            self._dev_tree.item(self._kbd_node,text=f"⌨  Keyboard  [✓ {key}]")

    def _test_mouse(self):
        self._mouse_active=True
        self._dev_log_write("Click or scroll mouse…","info")
        self._dev_tree.item(self._mouse_node,text="🖱  Mouse  [⏳ waiting]")

    def _on_click(self,event):
        if self._mouse_active:
            self._mouse_active=False
            btn={1:"Left",2:"Middle",3:"Right"}.get(event.num,f"#{event.num}")
            self._dev_log_write(f"Mouse: {btn} button ({event.x_root},{event.y_root}) — OK","ok")
            self._log(f"Mouse: {btn}","ok")
            self._dev_tree.item(self._mouse_node,text=f"🖱  Mouse  [✓ {btn}]")

    def _on_scroll(self,event):
        if self._mouse_active:
            self._mouse_active=False
            self._dev_log_write("Mouse: scroll wheel — OK","ok")
            self._dev_tree.item(self._mouse_node,text="🖱  Mouse  [✓ wheel]")

    def _test_mic(self):
        self._dev_log_write("Recording 2 sec…","info")
        self._dev_tree.item(self._mic_node,text="🎤  Microphone  [⏳ record]")
        threading.Thread(target=self._mic_worker,daemon=True).start()

    def _mic_worker(self):
        try:
            try:
                import sounddevice as sd,numpy as np
                rec=sd.rec(int(2*44100),samplerate=44100,channels=1,dtype='int16'); sd.wait()
                peak=int(np.abs(rec).max())
                self._q.put(("mic_result",f"✓ Peak:{peak}" if peak>50 else "⚠ Silent",
                             f"Mic peak={peak}","ok" if peak>50 else "warn"))
                return
            except ImportError: pass
            if platform.system()=="Linux":
                r=subprocess.run(["arecord","-d","2","-f","S16_LE","-r","44100","-c","1","/tmp/sonar_mic.wav"],
                                  capture_output=True,timeout=5)
                if r.returncode==0 and os.path.exists("/tmp/sonar_mic.wav"):
                    sz=os.path.getsize("/tmp/sonar_mic.wav"); os.remove("/tmp/sonar_mic.wav")
                    self._q.put(("mic_result","✓ OK" if sz>1000 else "⚠ Silent",f"arecord {sz}b","ok" if sz>1000 else "warn"))
                    return
            self._q.put(("mic_result","? Unavailable","pip install sounddevice","warn"))
        except Exception as e: self._q.put(("mic_result",f"✗ {e}",str(e),"err"))

    def _test_speakers(self):
        self._dev_log_write("Speaker Test…","info")
        self._dev_tree.item(self._spk_node,text="🔊  Speakers  [⏳ test]")
        _SpeakerWindow(self)

    # ─── Helpers ──────────────────────────────────────────────────
    def _set_status(self,text):
        try:
            self._status_left.configure(text=f"  {text}")
            self._status_right.configure(text=f"{_ts()}  ")
        except: pass

    # ─── Quick format checks ───────────────────────────────────────
    def _detect_type(self,path):
        SIGS={b'\x89PNG\r\n\x1a\n':'PNG',b'\xff\xd8\xff':'JPEG',b'GIF8':'GIF',
              b'%PDF':'PDF',b'PK\x03\x04':'ZIP/OOXML',b'Rar!':'RAR',b'\x1f\x8b':'GZIP',
              b'BZh':'BZIP2',b'\xfd7zXZ\x00':'XZ',b'7z\xbc\xaf':'7-ZIP',b'RIFF':'WAV/AVI',
              b'ID3':'MP3',b'\xff\xfb':'MP3',b'OggS':'OGG',b'\x1aE\xdf\xa3':'MKV/WEBM',
              b'fLaC':'FLAC',b'MZ':'EXE/DLL',b'\x7fELF':'ELF'}
        try:
            with open(path,'rb') as f: h=f.read(16)
            for sig,name in SIGS.items():
                if h[:len(sig)]==sig: return name
        except: pass
        return Path(path).suffix.upper().lstrip('.') or "File"

    def _check_zip(self,path):
        try:
            with zipfile.ZipFile(path,'r') as z:
                bad=z.testzip()
                return (False,f"Damaged: {bad}") if bad else (True,f"ZIP OK · {len(z.namelist())} files")
        except zipfile.BadZipFile as e: return False,f"Bad ZIP: {e}"
        except Exception as e: return False,str(e)

    def _check_tar(self,path):
        try:
            with tarfile.open(path,'r:*') as t: return True,f"TAR OK · {len(t.getmembers())} objects"
        except Exception as e: return False,str(e)

    def _check_gz(self,path):
        try:
            with gzip.open(path,'rb') as f:
                size=sum(len(c) for c in iter(lambda:f.read(65536),b''))
            return True,f"GZIP OK · {_fmt(size)}"
        except Exception as e: return False,str(e)

    def _check_image(self,path):
        ext=Path(path).suffix.lower()
        try:
            with open(path,'rb') as f: data=f.read()
            if ext=='.png':
                if data[:8]!=b'\x89PNG\r\n\x1a\n': return False,"Bad PNG sig"
                if not data.endswith(b'IEND\xaeB`\x82'): return False,"PNG truncated"
                return True,f"PNG OK {_fmt(len(data))}"
            elif ext in('.jpg','.jpeg'):
                if data[:2]!=b'\xff\xd8': return False,"Bad JPEG sig"
                if data[-2:]!=b'\xff\xd9': return False,"JPEG truncated"
                return True,f"JPEG OK {_fmt(len(data))}"
            return True,f"Image {_fmt(len(data))}"
        except Exception as e: return False,str(e)

    def _check_pdf(self,path):
        try:
            with open(path,'rb') as f:
                if not f.read(4).startswith(b'%PDF'): return False,"Not PDF"
                f.seek(-1024,2); tail=f.read()
            if b'%%EOF' not in tail and b'%EOF' not in tail: return False,"PDF truncated"
            return True,"PDF OK"
        except Exception as e: return False,str(e)

    def _check_wav(self,path):
        try:
            with wave.open(path,'rb') as w:
                dur=w.getnframes()/w.getframerate() if w.getframerate() else 0
                return True,f"WAV {w.getnchannels()}ch {w.getframerate()}Hz {dur:.1f}s"
        except Exception as e: return False,str(e)

    def _check_generic(self,path):
        try:
            sz=os.path.getsize(path)
            with open(path,'rb') as f: f.read(512); f.seek(max(0,sz-512)); f.read(512)
            return True,f"OK {_fmt(sz)}"
        except Exception as e: return False,str(e)

    # ─── About ──────────────────────────────────────────────────────
    def _about(self):
        T=self.T
        win=tk.Toplevel(self); win.title("About — Sonar v1.2")
        win.geometry("500x460"); win.resizable(False,False); win.configure(bg=T["bg"]); win.grab_set()
        hdr=tk.Frame(win,bg=T["accent"],height=52); hdr.pack(fill="x"); hdr.pack_propagate(False)
        tk.Label(hdr,text="  🔊 Sonar  v1.2",bg=T["accent"],fg=T["accent_fg"],
                 font=("Segoe UI",15,"bold")).pack(side="left",padx=10,pady=8)
        body=tk.Frame(win,bg=T["bg"]); body.pack(fill="both",expand=True,padx=16,pady=8)
        infos=[
            (f"C-core: {'active' if CORE.available else 'Python fallback'}", T["log_ok"] if CORE.available else T["log_warn"]),
            (f"Virus DB: {len(VDB.signatures)} signatures | PIL: {'✓' if HAS_PIL else '✗'} | Mutagen: {'✓' if HAS_MUTAGEN else '✗'} | psutil: {'✓' if HAS_PSUTIL else '✗'}",T["fg2"]),
            ("",""),
            ("Features:",T["fg"]),
            ("  EXIF / ID3 / PDF / DOCX metadata",T["fg2"]),
            ("  Recursive ZIP/RAR/7z analysis",T["fg2"]),
            ("  Line-by-line file diff (RMB)",T["fg2"]),
            ("  Damaged header repair",T["fg2"]),
            ("  LSB steganography + Chi² analysis",T["fg2"]),
            ("  Deep scan: virus signatures from JSON",T["fg2"]),
            ("  Process & Autorun Scan",T["fg2"]),
            ("  Devices: display, battery, network, BT, USB",T["fg2"]),
            ("  Real-time file monitoring",T["fg2"]),
            ("  Scheduled scanning",T["fg2"]),
            ("  Multi-threaded analysis (4 threads)",T["fg2"]),
            ("  Export: TXT / JSON / HTML (Chart.js)",T["fg2"]),
            ("",""),
            ("F5 — scan  F6 — deep analysis  RMB — context menu",T["fg2"]),
        ]
        for txt,col in infos:
            if not txt: tk.Frame(body,bg=T["bg"],height=3).pack(fill="x")
            else: tk.Label(body,text=txt,bg=T["bg"],fg=col,font=("Consolas",8),anchor="w").pack(fill="x")
        sep=tk.Frame(win,bg=T["sep"],height=1); sep.pack(fill="x",padx=8)
        footer=tk.Frame(win,bg=T["bg"],height=50); footer.pack(fill="x",padx=12,pady=8)
        github_url="https://github.com"
        try:
            if not HAS_PIL: raise RuntimeError("Pillow not available")
            icon_path=ASSETS_DIR/"github_icon.png"
            if icon_path.exists():
                img=Image.open(icon_path).resize((22,22),Image.LANCZOS)
                self._gh_icon=ImageTk.PhotoImage(img)
                tk.Button(footer,image=self._gh_icon,text="  GitHub",compound="left",
                          command=lambda:webbrowser.open(github_url),
                          bg=T["bg"],fg=T["fg"],relief="flat",cursor="hand2",
                          font=("Segoe UI",9,"underline")).pack(side="left")
            else: raise FileNotFoundError
        except Exception:
            tk.Button(footer,text="⚫ GitHub →",command=lambda:webbrowser.open(github_url),
                      bg=T["bg"],fg="#6E40C9",relief="flat",cursor="hand2",
                      font=("Segoe UI",10,"underline")).pack(side="left")
        tk.Label(footer,text="© 2025 Sonar  MIT",bg=T["bg"],fg=T["fg2"],font=("Segoe UI",8)).pack(side="right")
        tk.Button(win,text="  OK  ",command=win.destroy,font=("Segoe UI",9),
                  bg=T["toolbar"],fg=T["fg"],relief="groove",cursor="hand2").pack(pady=(0,8))


def main():
    app=SonarApp()
    app.mainloop()

if __name__=="__main__":
    main()
