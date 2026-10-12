"""
Artemis Engine PF8 Archive Tool (Unpack / Pack)
Specifically tested for '夏の魔女のパレード' (Wonder Fool) and other Artemis engine titles.
"""

import os
import sys
import struct
import hashlib
import argparse

def parse_pf8_index(data, encoding="cp932"):
    if data[:3] != b"pf8":
        raise ValueError("Invalid magic header, expected 'pf8'")
    
    index_size, index_count = struct.unpack("<II", data[3:11])
    
    # Calculate SHA1 key from index bytes (starting at offset 0x7 for index_size bytes)
    index_data = data[7 : 7 + index_size]
    key = hashlib.sha1(index_data).digest()
    
    entries = []
    cur = 11
    for _ in range(index_count):
        name_len = struct.unpack("<I", data[cur : cur + 4])[0]
        cur += 4
        raw_name = data[cur : cur + name_len]
        cur += name_len
        # skip 4 bytes padding
        cur += 4
        offset, size = struct.unpack("<II", data[cur : cur + 8])
        cur += 8
        
        try:
            name = raw_name.decode(encoding)
        except UnicodeDecodeError:
            name = raw_name.decode("utf-8", errors="replace")
        
        name = name.rstrip("\x00")
        entries.append({
            "name": name,
            "offset": offset,
            "size": size,
            "raw_name": raw_name
        })
        
    return entries, key

def xor_crypt(data, key):
    res = bytearray(len(data))
    klen = len(key)
    for i in range(len(data)):
        res[i] = data[i] ^ key[i % klen]
    return bytes(res)

def unpack_pf8(pfs_path, out_dir, filter_ext=None, encoding="cp932"):
    print(f"[*] Reading archive: {pfs_path}")
    with open(pfs_path, "rb") as f:
        file_bytes = f.read()
        
    entries, key = parse_pf8_index(file_bytes, encoding=encoding)
    print(f"[*] Found {len(entries)} files. Encryption SHA1 key: {key.hex()}")
    
    count = 0
    for entry in entries:
        name = entry["name"]
        if filter_ext:
            ext = os.path.splitext(name)[1].lower()
            if ext not in filter_ext:
                continue
                
        offset = entry["offset"]
        size = entry["size"]
        raw_enc = file_bytes[offset : offset + size]
        dec = xor_crypt(raw_enc, key)
        
        out_path = os.path.join(out_dir, name)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as out_f:
            out_f.write(dec)
        count += 1
        
    print(f"[+] Successfully extracted {count} files to: {out_dir}")

def pack_pf8(in_dir, out_pfs_path, encoding="cp932"):
    print(f"[*] Scanning directory: {in_dir}")
    file_list = []
    for root, _, files in os.walk(in_dir):
        for file in files:
            full_path = os.path.join(root, file)
            rel_path = os.path.relpath(full_path, in_dir)
            size = os.path.getsize(full_path)
            file_list.append((rel_path, full_path, size))
            
    print(f"[*] Total files to pack: {len(file_list)}")
    index_count = len(file_list)
    
    # Calculate index size
    # header: b'pf8' (3) + index_size (4) + index_count (4) = 11 bytes
    # per entry: name_len (4) + name_bytes + pad (4) + offset (4) + size (4) = name_len + 16
    # footer: file_count (4) + 8 * (index_count + 1) + 4
    entries_size = 0
    encoded_names = []
    for rel_path, _, _ in file_list:
        try:
            nb = rel_path.encode(encoding)
        except UnicodeEncodeError:
            nb = rel_path.encode("utf-8")
        encoded_names.append(nb)
        entries_size += len(nb) + 16
        
    index_size = 4 + entries_size + 4 + (index_count + 1) * 8 + 4
    file_offset = 7 + index_size
    
    # Build index buffer
    idx_buf = bytearray()
    idx_buf.extend(b"pf8")
    idx_buf.extend(struct.pack("<II", index_size, index_count))
    
    offsets_table = []
    cur_file_offset = file_offset
    for i, (rel_path, full_path, size) in enumerate(file_list):
        nb = encoded_names[i]
        idx_buf.extend(struct.pack("<I", len(nb)))
        idx_buf.extend(nb)
        idx_buf.extend(struct.pack("<III", 0, cur_file_offset, size))
        # record offset from 0xF
        offsets_table.append(len(idx_buf) - 4 - 0xF)
        cur_file_offset += size
        
    idx_buf.extend(struct.pack("<I", index_count + 1))
    filesize_count_offset = len(idx_buf) - 4 - 7
    for off in offsets_table:
        idx_buf.extend(struct.pack("<Q", off))
    idx_buf.extend(struct.pack("<QI", 0, filesize_count_offset))
    
    # Compute SHA1 key for encrypting data
    index_data = idx_buf[7 : 7 + index_size]
    key = hashlib.sha1(index_data).digest()
    print(f"[*] Archive index built. SHA1 key: {key.hex()}")
    
    with open(out_pfs_path, "wb") as out_f:
        out_f.write(idx_buf)
        for rel_path, full_path, size in file_list:
            with open(full_path, "rb") as in_f:
                raw_data = in_f.read()
            enc_data = xor_crypt(raw_data, key)
            out_f.write(enc_data)
            
    print(f"[+] Archive successfully created: {out_pfs_path} ({os.path.getsize(out_pfs_path)} bytes)")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Artemis Engine PF8 Tool")
    subparsers = parser.add_subparsers(dest="cmd", required=True)
    
    p_unpack = subparsers.add_parser("unpack", help="Unpack .pfs archive")
    p_unpack.add_argument("pfs_file", help="Path to .pfs file")
    p_unpack.add_argument("-o", "--output", default="unpacked", help="Output directory")
    p_unpack.add_argument("-e", "--ext", nargs="*", help="Filter extensions, e.g. .txt .lua")
    
    p_pack = subparsers.add_parser("pack", help="Pack directory into .pfs archive")
    p_pack.add_argument("input_dir", help="Directory to pack")
    p_pack.add_argument("-o", "--output", required=True, help="Path to output .pfs file")
    
    args = parser.parse_args()
    if args.cmd == "unpack":
        unpack_pf8(args.pfs_file, args.output, filter_ext=args.ext)
    elif args.cmd == "pack":
        pack_pf8(args.input_dir, args.output)
