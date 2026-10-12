# /// script
# dependencies = ["pycryptodome", "numpy"]
# ///
"""Read embedded Molebox 4.x packages, based on the GPL Molebox sources.

Reference: https://github.com/sudachen/Molebox
Outputs are written to a separate directory; the input executable is read-only.
"""

import argparse
import binascii
import collections
import hashlib
import json
import re
from pathlib import Path
import struct
import sys
import zlib

from Crypto.Cipher import Blowfish
import numpy as np


ROTOR = bytes.fromhex(
    "2089efbc667ddd48d444512556ed939546e5117c73cf21147a8f19d733b78a8e"
    "92d36ead01e4bd0e674ea224fda774ff9e2db93262a8faeb368dc3f7f03f9402"
    "e0a9d6b43e16756c13aca19fa02f2babc2afb238c47017dc5915a4829d0855fb"
    "d82c5eb3e2265a7728ca22ce2345e7f61d6d4a47b0063c91410d4d970c7f5fc7"
    "396505e896d28118b50a79bb30c18bfcdb4058e960805035bf90da0b6a849b68"
    "5b881f2af3427e871e1a57bab69af27b52a6d02798be71cd7269e15449a3636f"
    "cc3dc8d9aa0fc61cc0fe86eade07ecf8c929b19c5c8343f9f5b8cb09f1001b2e"
    "85ae4b125dd164784cd51053046b8c343a3703f461c5eee376314fe6dfa5993b"
)


def newdes_key(key):
    schedule = []
    i = 11
    while True:
        for _ in range(4):
            schedule.append(key[i])
            i = (i + 1) % 15
        i = (i + 8) % 15
        if i == 12:
            return schedule
        for _ in range(3):
            schedule.append(key[i])
            i += 1
        i = (i + 8) % 15


def newdes_decrypt(data, key, cbc=False):
    k = newdes_key(key)
    length = len(data) & ~7
    if length >= 1024:
        b = np.frombuffer(data[:length], dtype=np.uint8).reshape(-1, 8).copy()
        rotor = np.frombuffer(ROTOR, dtype=np.uint8)
        q = 0
        for _ in range(8):
            for j in range(4):
                b[:, j + 4] ^= rotor[b[:, j] ^ k[q]]
                q += 1
            b[:, 1] ^= rotor[b[:, 4] ^ k[q]]
            q += 1
            b[:, 2] ^= rotor[b[:, 4] ^ b[:, 5]]
            b[:, 3] ^= rotor[b[:, 6] ^ k[q]]
            q += 1
            b[:, 0] ^= rotor[b[:, 7] ^ k[q]]
            q += 1
        for j in range(4):
            b[:, j + 4] ^= rotor[b[:, j] ^ k[q]]
            q += 1
        if cbc:
            b[1:] ^= np.frombuffer(data[:length - 8], dtype=np.uint8).reshape(-1, 8)
        return b.tobytes() + data[length:]
    result = bytearray(data)
    previous = bytes(8)
    for start in range(0, len(data) - 7, 8):
        original = data[start:start + 8]
        b = list(original)
        q = 0
        for _ in range(8):
            for j in range(4):
                b[j + 4] ^= ROTOR[b[j] ^ k[q]]
                q += 1
            b[1] ^= ROTOR[b[4] ^ k[q]]
            q += 1
            b[2] ^= ROTOR[b[4] ^ b[5]]
            b[3] ^= ROTOR[b[6] ^ k[q]]
            q += 1
            b[0] ^= ROTOR[b[7] ^ k[q]]
            q += 1
        for j in range(4):
            b[j + 4] ^= ROTOR[b[j] ^ k[q]]
            q += 1
        if cbc:
            b = [v ^ p for v, p in zip(b, previous)]
        result[start:start + 8] = bytes(b)
        previous = original
    return bytes(result)


def swap_words(data):
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))


class PackageCipher:
    def __init__(self, key, little_endian=False):
        self.key = key
        self.little_endian = little_endian

    def decrypt(self, data, cbc=False):
        length = len(data) & ~7
        source = data[:length]
        if self.little_endian:
            source = swap_words(source)
        cipher = Blowfish.new(self.key, Blowfish.MODE_CBC, iv=bytes(8)) if cbc else Blowfish.new(self.key, Blowfish.MODE_ECB)
        result = cipher.decrypt(source)
        if self.little_endian:
            result = swap_words(result)
        return result + data[length:]


class NewdesCipher:
    def __init__(self, key):
        self.key = key

    def decrypt(self, data, cbc=False):
        return newdes_decrypt(data, self.key, cbc)


def cstring(data, offset, encoding="utf-8"):
    end = data.find(b"\0", offset)
    if not 0 <= offset < len(data) or end < 0:
        raise ValueError("Invalid string offset")
    return data[offset:end].decode(encoding)


def stub_headers(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    section_start = pe + 24 + optional_size
    end = 0
    for i in range(count):
        raw_size, raw_offset = struct.unpack_from("<II", data, section_start + i * 40 + 16)
        end = max(end, raw_offset + raw_size)
    for location in {struct.unpack_from("<I", data, 0x1C)[0], end}:
        if location + 500 > len(data):
            continue
        a, b = struct.unpack_from("<II", data, location)
        if a ^ b != 0x554A4F58 ^ 0x4A4E414D:
            continue
        source = data[location + 8:location + 500]
        key = source[368:384]
        for cbc in [False, True]:
            decoded = newdes_decrypt(source[:368], key, cbc) + key + newdes_decrypt(source[384:488], key, cbc) + source[488:]
            yield location, cbc, decoded


def unpack_lzss(data, size):
    output = bytearray()
    cursor = 0
    while cursor < len(data) and len(output) < size:
        value = data[cursor]
        if value == 0x80:
            output.append(data[cursor + 1])
            cursor += 2
        elif value == 0:
            count = data[cursor + 1] + 1
            output.extend(data[cursor + 2:cursor + 2 + count])
            cursor += count + 2
        else:
            code = struct.unpack_from("<H", data, cursor)[0]
            length, distance = code & 15, (code >> 4) + 15
            if distance > len(output):
                raise ValueError("Invalid LZSS back-reference")
            start = len(output) - distance
            output.extend(output[start:start + length])
            cursor += 2
    if len(output) != size:
        raise ValueError("Unexpected LZSS output size")
    return bytes(output)


def dump_core(data, output):
    for location, header_cbc, header in stub_headers(data):
        core_offset, core_size = struct.unpack_from("<II", header, 32)
        if core_offset + core_size > len(data) or not 4 < core_size < 10_000_000:
            continue
        key = header[368:384]
        for core_cbc in [False, True]:
            payload = newdes_decrypt(data[core_offset:core_offset + core_size], key, core_cbc)
            size = struct.unpack_from("<I", payload)[0]
            if not 0 < size < 10_000_000:
                continue
            try:
                core = unpack_lzss(payload[4:], size)
            except (ValueError, IndexError, struct.error):
                continue
            output.mkdir(parents=True, exist_ok=True)
            (output / "core.bin").write_bytes(core)
            strings = [v.decode("ascii") for v in re.findall(rb"[\x20-\x7e]{6,}", core)]
            (output / "core_strings.txt").write_text("\n".join(strings), encoding="utf-8")
            write_analysis_pe(core, output / "core_analysis.exe")
            print("Recovered core:", len(core), "header:", core[:64].hex())
            return core
    raise ValueError("Could not recover the embedded core")


def write_analysis_pe(core, output):
    """Create a synthetic analysis image; this is not an executable to launch."""
    image_base, entrypoint = struct.unpack_from("<I", core, 28)[0], struct.unpack_from("<I", core, 36)[0]
    headers = bytearray(1024)
    headers[:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, 0x80)
    headers[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", headers, 0x84, 0x14C, 1, 0, 0, 0, 224, 0x102)
    optional = 0x98
    struct.pack_into("<H", headers, optional, 0x10B)
    struct.pack_into("<III", headers, optional + 16, entrypoint, 0x1000, 0x1000)
    struct.pack_into("<III", headers, optional + 28, image_base, 0x1000, 0x200)
    struct.pack_into("<H", headers, optional + 40, 4)
    struct.pack_into("<II", headers, optional + 56, (len(core) + 4095) & ~4095, 1024)
    struct.pack_into("<H", headers, optional + 68, 3)
    struct.pack_into("<IIIIII", headers, optional + 72, 0x100000, 4096, 0x100000, 4096, 0, 16)
    section = optional + 224
    raw_size = (len(core) - 0x1000 + 511) & ~511
    headers[section:section + 8] = b".core\0\0\0"
    struct.pack_into("<IIII", headers, section + 8, len(core) - 0x1000, 0x1000, raw_size, 1024)
    struct.pack_into("<I", headers, section + 36, 0xE0000060)
    output.write_bytes(bytes(headers) + core[0x1000:] + bytes(raw_size - (len(core) - 0x1000)))


def read_catalog(data):
    positions = {len(data) - 8}
    for location, cbc, header in stub_headers(data):
        flags, catalog = struct.unpack_from("<I", header, 4)[0], struct.unpack_from("<I", header, 444)[0]
        print("Stub header", hex(location), "CBC:", cbc, "flags:", hex(flags), "catalog:", hex(catalog))
        if 0 <= catalog <= len(data) - 8:
            positions.add(catalog)
    keys = [("catalog-guid", hashlib.md5(b"7233DA04FCF24B388408701CB1F874F3").digest()),
            ("ambacaramba-md5", hashlib.md5(b"ambacaramba").digest()),
            ("ambacaramba-raw", b"ambacaramba".ljust(16, b"\0")),
            ("ambacaramba-padded", b"ambacaramba".ljust(16, b"^"))]
    ciphers = []
    for key_name, key in keys:
        ciphers.append((key_name, "newdes", 15, False, NewdesCipher(key[:15])))
        ciphers += [(key_name, "blowfish", size, little, PackageCipher(key[:size], little))
                    for size in [15, 16] for little in [False, True]]
    for key_name, algorithm, key_size, little_endian, cipher in ciphers:
        for footer_encrypted in [False, True]:
            for position in positions:
                footer = data[position:position + 8]
                if footer_encrypted:
                    footer = cipher.decrypt(footer)
                size, crc = struct.unpack("<II", footer)
                if not 0 < size <= 65536 or size > position or size % 8:
                    continue
                catalog = None
                for catalog_cbc in [False, True]:
                    candidate = cipher.decrypt(data[position - size:position], cbc=catalog_cbc)
                    checks = [zlib.crc32(candidate), zlib.crc32(candidate, 0xFFFFFFFF) ^ 0xFFFFFFFF, zlib.adler32(candidate, 0)]
                    if crc in checks:
                        catalog = candidate
                        break
                if catalog is None:
                    continue
                pkg_offset, pkg_count, mount_offset, mount_count = struct.unpack_from("<4I", catalog)
                packages, mounts = [], []
                for i in range(pkg_count):
                    offset, name = struct.unpack_from("<II", catalog, pkg_offset + i * 8)
                    packages.append(dict(offset=offset, name=cstring(catalog, name)))
                for i in range(mount_count):
                    flags, mask, pwd = struct.unpack_from("<II16s", catalog, mount_offset + i * 24)
                    mounts.append(dict(flags=flags, mask=cstring(catalog, mask), key_hex=pwd.hex()))
                return dict(offset=position, size=size, crc=crc, key_name=key_name, algorithm=algorithm, key_size=key_size,
                            little_endian=little_endian, footer_encrypted=footer_encrypted,
                            catalog_cbc=catalog_cbc, packages=packages, mounts=mounts)
    raise ValueError("No CRC-valid Molebox catalog found")


def read_package(data, package, key, catalog, output):
    cipher = NewdesCipher(key[:15]) if catalog["algorithm"] == "newdes" else PackageCipher(key[:catalog["key_size"]], catalog["little_endian"])
    position = package["offset"]
    header = None
    for cbc in [False, True]:
        candidate = cipher.decrypt(data[position:position + 48], cbc)
        if candidate[40:48] == b"STELPACK":
            header = candidate
            break
    if header is None:
        raise ValueError("Could not decode the package header")
    fields = struct.unpack("<10I8s", header)
    tree_offset, tree_size, root_offset, base_distance, crc, map_offset, map_count, _, stream_ptr, stream_size, _ = fields
    tree = cipher.decrypt(data[position - tree_size:position], cbc)
    checks = [zlib.crc32(tree), zlib.crc32(tree, 0xFFFFFFFF) ^ 0xFFFFFFFF]
    if crc not in checks:
        raise ValueError("Package tree CRC mismatch")
    print("Package header:", [hex(v) if isinstance(v, int) else repr(v) for v in fields])
    output.mkdir(parents=True, exist_ok=True)
    (output / "package_tree.bin").write_bytes(tree)
    entries = []
    for i in range(map_count):
        node = struct.unpack_from("<I", tree, map_offset + 4 * i)[0]
        words = struct.unpack_from("<13I", tree, node)
        name_offset, name_length = words[2:4]
        name = tree[name_offset:name_offset + 2 * name_length].decode("utf-16le") if name_length else ""
        flags, size, count = words[10:13]
        entries.append(dict(node=node, name=name, flags=flags, size=size, count=count,
                            sign_hex=tree[node + 20:node + 36].hex()))
    by_node = {e["node"]: e for e in entries}
    visited = set()

    def walk(node, parent=""):
        if node in visited:
            raise ValueError("Cyclic package directory tree")
        visited.add(node)
        if node == root_offset:
            entry = dict(node=node, name="", flags=0x80000000,
                         count=struct.unpack_from("<I", tree, node + 48)[0])
        else:
            entry = by_node[node]
        path = parent + ("\\" if parent else "") + entry["name"]
        if entry["name"]:
            signature = hashlib.md5(path.upper().encode("utf-16le")).hexdigest()
            if signature != entry["sign_hex"]:
                raise ValueError(f"Package path signature mismatch: {path}")
        entry["path"] = path
        if entry["flags"] & 0x80000000:
            children = struct.unpack_from(f"<{entry['count']}I", tree, node + 52)
            for child in children:
                walk(child, path)

    walk(root_offset)
    if len(visited) != len(entries) + 1:
        raise ValueError("Directory traversal did not reach every package entry")
    manifest = dict(name=package["name"], offset=position, base=position - base_distance,
                    tree_size=tree_size, root_offset=root_offset, map_count=map_count,
                    cbc=cbc, entries=entries)
    (output / "package_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Package entries:", len(entries), "types:", dict(collections.Counter(Path(e["path"]).suffix.lower() or "<directory>" for e in entries)))
    return manifest, tree, cipher


def crc16_table():
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
        table.append(value)
    return tuple(table)


CRC16_TABLE = crc16_table()


def crc16_ibm(data):
    crc = 0
    for value in data:
        crc = (crc >> 8) ^ CRC16_TABLE[(crc ^ value) & 255]
    return crc


def extract_package(data, manifest, tree, cipher, output):
    extracted = []
    crc_kind = None
    for entry in manifest["entries"]:
        if entry["flags"] & 0x80000000:
            continue
        parts = entry["path"].replace("\\", "/").split("/")
        if any(p in ("", ".", "..") or ":" in p for p in parts):
            raise ValueError(f"Unsafe package file path: {entry['path']!r}")
        content = bytearray()
        for block in range(entry["count"]):
            packed_size, real_size, flags, crc, offset = struct.unpack_from("<4HI", tree, entry["node"] + 52 + block * 12)
            position = manifest["base"] + offset
            payload = data[position:position + packed_size]
            if len(payload) != packed_size:
                raise ValueError("Truncated package block")
            if flags & 4:
                payload = cipher.decrypt(payload, cbc=manifest["cbc"])
            if flags & 2:
                payload = zlib.decompress(payload)
            payload = payload[:real_size]
            if len(payload) != real_size:
                raise ValueError("Wrong decompressed block length")
            if crc_kind is None:
                if binascii.crc_hqx(payload, 0) == crc:
                    crc_kind = "ccitt"
                elif crc16_ibm(payload) == crc:
                    crc_kind = "ibm"
                else:
                    raise ValueError("Could not identify the block CRC16")
            calculated_crc = binascii.crc_hqx(payload, 0) if crc_kind == "ccitt" else crc16_ibm(payload)
            if crc != calculated_crc:
                raise ValueError(f"Block CRC16 mismatch: {entry['path']}, block {block}")
            content.extend(payload)
        if len(content) != entry["size"]:
            raise ValueError("Reconstructed file length mismatch")
        destination = output.joinpath(*parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        extracted.append(dict(path=entry["path"], size=len(content), sha256=hashlib.sha256(content).hexdigest(), blocks=entry["count"]))
    report = dict(files=len(extracted), bytes=sum(e["size"] for e in extracted),
                  crc16=crc_kind, all_blocks_verified=True, entries=extracted)
    (output.parent / "extraction_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Extracted", report["files"], "files,", report["bytes"], "bytes; all block checks passed.")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("input", nargs="?", type=Path, default=root / "Orig" / "StrawberryNauts_cn_140401.exe")
    parser.add_argument("--output", type=Path, default=root / "tmp" / "molebox")
    parser.add_argument("--core-only", action="store_true")
    args = parser.parse_args()
    data = args.input.read_bytes()
    print("Input size:", len(data), "tail:", data[-64:].hex())
    dump_core(data, args.output)
    if args.core_only:
        return
    catalog = read_catalog(data)
    print(json.dumps(catalog, ensure_ascii=False, indent=2))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "catalog.json").write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding="utf-8")
    for package in catalog["packages"]:
        mount = next(m for m in catalog["mounts"] if m["mask"] == package["name"] and m["flags"] & 1)
        manifest, tree, cipher = read_package(data, package, bytes.fromhex(mount["key_hex"]), catalog, args.output)
        extract_package(data, manifest, tree, cipher, args.output / "files")


if __name__ == "__main__":
    main()
