"""Inspect headers/checksums without executing or flashing the input image."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
CHIPS={0:"ESP32",2:"ESP32-S2",5:"ESP32-C3",9:"ESP32-S3"}

def image_at(data,offset):
    if offset+24>len(data) or data[offset]!=0xe9: return None
    segments=data[offset+1]
    if not 1<=segments<=16: return None
    chip=struct.unpack_from("<H",data,offset+12)[0]
    position=offset+24; checksum=0xef; first=None
    for _ in range(segments):
        if position+8>len(data): return None
        address,length=struct.unpack_from("<II",data,position); position+=8
        if length>16*1024*1024 or position+length>len(data): return None
        segment=data[position:position+length]
        if first is None: first=segment
        for byte in segment: checksum^=byte
        position+=length
    check_position=position+((15-(position-offset)%16)%16)
    if check_position>=len(data): return None
    valid=data[check_position]==checksum
    end=check_position+1
    hash_valid=None
    if data[offset+23]:
        if end+32>len(data): return None
        hash_valid=hashlib.sha256(data[offset:end]).digest()==data[end:end+32]; end+=32
    info={"offset":hex(offset),"chip_id":chip,"chip":CHIPS.get(chip,"unknown"),"segments":segments,"image_bytes":end-offset,"checksum_valid":valid,"hash_valid":hash_valid,"kind":"bootloader_or_other"}
    if first and len(first)>=256 and struct.unpack_from("<I",first)[0]==0xabcd5432:
        def string(start,size): return first[start:start+size].split(b"\0",1)[0].decode("ascii","replace")
        info.update(kind="app",version=string(16,32),project=string(48,32),build_time=string(80,16),build_date=string(96,16),idf=string(112,32))
    return info

def inspect(path):
    data=Path(path).read_bytes()
    offsets={0,0x1000,0x10000}
    # Valid partition tables provide additional app offsets for combined images.
    for base in (0x8000,0x7000):
        for pos in range(base,min(base+0xc00,len(data)-32),32):
            if data[pos:pos+2]!=b"\xaa\x50": break
            if data[pos+2]==0: offsets.add(struct.unpack_from("<I",data,pos+4)[0])
    images=[i for offset in sorted(offsets) if (i:=image_at(data,offset))]
    valid=images and all(i["checksum_valid"] and i["hash_valid"] is not False for i in images)
    compatible=bool(valid and all(i["chip_id"]==0 for i in images))
    return {"file":Path(path).name,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"type":"combined" if len(images)>1 else (images[0]["kind"] if images else "unknown"),"images":images,"compatible_with_esp32_wroom32":compatible}

if __name__=="__main__":
    parser=argparse.ArgumentParser(); parser.add_argument("file"); parser.add_argument("--output"); parser.add_argument("--require-esp32",action="store_true"); args=parser.parse_args()
    report=inspect(args.file); text=json.dumps(report,indent=2)
    if args.output: Path(args.output).write_text(text+"\n",encoding="utf-8")
    print(text)
    if args.require_esp32 and not report["compatible_with_esp32_wroom32"]: raise SystemExit("Refusing incompatible or unverified image")
