"""Exe-side text and code patches of D.C.4 Plus Harmony for the GBK build (base: DC4PHDL_crack.exe).

  python exe_text.py export <exe> <strings.json>
  python exe_text.py fill   <strings.json> <ja_json_dir> <zh_json_dir> [--manual manual_zh.json]
  python exe_text.py build  <exe> <strings.json> <out_exe> [--font SimHei] [--mes-dir MES_CHS] [--no-acp936]

export  lists every Japanese string the GBK build has to touch: chapter titles (save/load screen,
        table at 0x4B34E8), BGM titles (0x4B4458), the speaker-name table (0x4AE008, 29 x 32 bytes),
        the phone-message names, the default player name and the UI / error strings in .rdata.
        For each string it records how much room there is in place ("max", bytes without the NUL)
        and whether every reference is a plain pointer that can be redirected ("reloc").
fill    fills "zh" from the translated VNTT JSON (scene titles "◆…◆" of sce_*.json, speaker names,
        identical messages), then from manual_zh.json ({"by_id": {...}, "by_ja": {...}}; by_id wins
        over everything, by_ja only fills what is still empty).
build   writes the GBK strings (empty "zh" = transcode the Japanese), moves strings that do not fit
        into a new section ".chs" and redirects their references, and applies the code patches:
          - main text font LOGFONT.lfCharSet 0x80 -> 0x86 (GB2312) and font face name (--font); the
            face name always lives in a fixed 32-byte slot at the start of .chs (hex-editable)
          - the two CreateFontA calls of the system dialog: charset 0x80 -> 0x86
          - CRT getSystemCP: GetACP() -> 936, so the engine's _ismbclegal parses GBK on any locale
          - script directory "MES" -> --mes-dir, so the Japanese scripts can stay in place
          - strncmp lengths of the phone-message speaker names follow the new byte length
"""
import argparse
import collections
import hashlib
import json
import os
import re
import struct
import sys

import pefile

CRACK_MD5 = "17586653BBF7844768D0BB2E54A72D76"
IMAGE_BASE = 0x400000
ENC = "gbk"

CHAPTER_TABLE = 0x4B34E8
BGM_TABLE = 0x4B4458
NAME_TABLE = 0x4AE008
NAME_SLOT = 0x20
FONT_VA = 0x48C650
FONT_MAX = 31  # LOGFONT.lfFaceName is 32 chars
MES_DIR_VA = 0x491918
STRNCMP_VA = 0x45A80B
UI_RANGE = (0x48B400, 0x492C40)
# .data copies of the default player name (part of an initialised struct, no direct references)
DEFAULT_NAME_DATA = (0x4AE570, 0x4AE580)
# ruby markers looked up as SJIS bytes by the renderer (ruby is stripped from the GBK scripts),
# and a "・" whose address is only used as a loop end marker (cmp eax, 0x48B48C)
KEEP_AS_IS = {0x48B48A, 0x48B48C, 0x48B490, 0x48B494, 0x48B498, 0x48B80C}

CODE_PATCHES = [
    (0x439A09, b"\x80", b"\x86", "main text font: LOGFONT.lfCharSet SHIFTJIS -> GB2312"),
    (0x41ACCD, b"\x80", b"\x86", "system dialog font sample: CreateFontA charset -> GB2312"),
    (0x41B5B7, b"\x80", b"\x86", "system dialog font sample: CreateFontA charset -> GB2312"),
]
ACP_PATCH = (0x4625B4, bytes.fromhex("ff1534b14800"), bytes.fromhex("b8a803000090"),
             "CRT getSystemCP(_MB_CP_ANSI): call GetACP -> mov eax, 936")

WIDE = re.compile(r"[^\x00-\x7f]")


class Exe:
    def __init__(self, path):
        self.data = bytearray(open(path, "rb").read())
        self.md5 = hashlib.md5(self.data).hexdigest().upper()
        self.pe = pefile.PE(data=bytes(self.data), fast_load=True)
        self.sections = [(s.Name.rstrip(b"\0").decode(), IMAGE_BASE + s.VirtualAddress, s.Misc_VirtualSize,
                          s.PointerToRawData, s.SizeOfRawData) for s in self.pe.sections]

    def off(self, va):
        for _, sva, vsz, raw, rsz in self.sections:
            if sva <= va < sva + max(vsz, rsz) and va - sva < rsz:
                return raw + va - sva
        raise ValueError(f"VA {va:08X} not backed by file data")

    def section_of_off(self, off):
        for name, sva, _, raw, rsz in self.sections:
            if raw <= off < raw + rsz:
                return name, sva + off - raw
        return None, None

    def u32(self, va):
        return struct.unpack_from("<I", self.data, self.off(va))[0]

    def cstr(self, va):
        o = self.off(va)
        return bytes(self.data[o:self.data.index(b"\0", o)])

    def refs(self, va):
        """All 4-byte occurrences of `va`, classified as push / movimm / ptr / other."""
        pat = struct.pack("<I", va)
        d = self.data
        out = []
        i = d.find(pat)
        while i != -1:
            sec, at = self.section_of_off(i)
            kind = "other"
            if sec == ".text":
                if d[i - 1] == 0x68:
                    kind = "push"
                elif 0xB8 <= d[i - 1] <= 0xBF:
                    kind = "movimm"
                elif ((d[i - 6] == 0xC7 and d[i - 5] == 0x05) or (d[i - 4] == 0xC7 and d[i - 3] == 0x44 and d[i - 2] == 0x24)
                      or (d[i - 7] == 0xC7 and d[i - 6] == 0x84 and d[i - 5] == 0x24)):
                    kind = "movimm"
            elif sec in (".data", ".rdata") and at % 4 == 0:
                kind = "ptr"
            if sec is not None:
                out.append({"off": i, "va": at, "sec": sec, "kind": kind})
            i = d.find(pat, i + 1)
        return out


def room_in_place(exe, va, length, refs):
    """Bytes available without the terminator. Inline copies (mov reg,[va]) copy exactly length+1."""
    if any(r["kind"] == "other" for r in refs):
        return length
    end = (va + length + 1 + 3) & ~3
    o = exe.off(va)
    tail = exe.data[o + length:o + (end - va)]
    if any(tail):
        return length
    return end - va - 1


def make_entry(exe, va, group, id_, max_override=None, reloc_override=None):
    raw = exe.cstr(va)
    refs = exe.refs(va)
    reloc = bool(refs) and all(r["kind"] in ("push", "movimm", "ptr") for r in refs)
    if reloc_override is not None:
        reloc = reloc_override
    return {
        "id": id_,
        "group": group,
        "va": f"{va:08X}",
        "ja": raw.decode("cp932"),
        "zh": "",
        "src": "",
        "bytes": len(raw),
        "max": max_override if max_override is not None else room_in_place(exe, va, len(raw), refs),
        "reloc": reloc,
        "refs": len(refs),
    }


def cmd_export(args):
    exe = Exe(args.exe)
    if exe.md5 != CRACK_MD5:
        print(f"warning: MD5 {exe.md5} is not the known DC4PHDL_crack.exe ({CRACK_MD5})")
    entries, seen = [], {}

    def add(e):
        if e["va"] in seen:
            seen[e["va"]].setdefault("also", []).append(e["id"])
            return
        seen[e["va"]] = e
        entries.append(e)

    va = CHAPTER_TABLE
    while exe.u32(va):
        mes, title = exe.u32(va), exe.u32(va + 4)
        if WIDE.search(exe.cstr(title).decode("cp932")):
            add(make_entry(exe, title, "chapter", "chapter/" + exe.cstr(mes).decode("ascii")))
        va += 8
    va = BGM_TABLE
    while exe.u32(va) != 0xFFFFFFFF:
        num, title = exe.u32(va), exe.u32(va + 4)
        if WIDE.search(exe.cstr(title).decode("cp932")):
            add(make_entry(exe, title, "bgm", f"bgm/{num:02d}"))
        va += 8
    i = 0
    while exe.data[exe.off(NAME_TABLE + i * NAME_SLOT)]:
        add(make_entry(exe, NAME_TABLE + i * NAME_SLOT, "name", f"name/{i:02d}", NAME_SLOT - 1, False))
        i += 1
    for k, va in enumerate(DEFAULT_NAME_DATA):
        add(make_entry(exe, va, "default_name", f"default_name/data{k}", len(exe.cstr(va)), False))

    lo, hi = UI_RANGE
    o, end = exe.off(lo), exe.off(hi)
    while o < end:
        if exe.data[o] == 0:
            o += 1
            continue
        z = exe.data.index(b"\0", o)
        # a string may follow the tail of a non-string constant ("K.$バージョン情報"):
        # also try every 4-byte aligned start inside the run
        starts = [o] + list(range((o + 4) & ~3, z, 4))
        o = z + 1
        for s in starts:
            raw = bytes(exe.data[s:z])
            _, va = exe.section_of_off(s)
            if va in KEEP_AS_IS or f"{va:08X}" in seen or va == FONT_VA:
                continue
            try:
                text = raw.decode("cp932")
            except UnicodeDecodeError:
                continue
            if not WIDE.search(text) or not all(c.isprintable() or c in "\r\n\t" for c in text):
                continue
            if re.search(r"[\uff61-\uff9f]", text):  # half-width katakana: binary data, not text
                continue
            if not exe.refs(va):
                continue
            add(make_entry(exe, va, "ui", f"ui/{va:08X}"))
            break

    # phone-message speaker names: strncmp(name, "<name>", n) chain in sub_458xxx
    for e in entries:
        if e["group"] == "ui" and any(strncmp_len_site(exe, r) for r in exe.refs(int(e["va"], 16))):
            e["group"] = "phone"
            e["id"] = "phone/" + e["va"]
    json.dump(entries, open(args.json, "w", encoding="utf-8", newline="\n"), ensure_ascii=False, indent=1)
    print(f"exported {len(entries)} strings: " + ", ".join(f"{k} {v}" for k, v in collections.Counter(e["group"] for e in entries).items()))


def strncmp_len_site(exe, ref):
    """For `push n / [lea r,[esp+x]] / push va / push r / call strncmp`, return the file offset of n."""
    if ref["kind"] != "push":
        return None
    d, i = exe.data, ref["off"]
    for k in (3, 7):  # 6a NN 68 <va>   |   6a NN 8d xx 24 xx 68 <va>
        if d[i - k] == 0x6A and (k == 3 or d[i - 5] == 0x8D):
            break
    else:
        return None
    for j in range(i + 4, i + 20):
        if d[j] == 0xE8:
            _, at = exe.section_of_off(j)
            if at + 5 + struct.unpack_from("<i", d, j + 1)[0] == STRNCMP_VA:
                return i - k + 1
    return None


# ---------------------------------------------------------------- fill

def load_pairs(ja_dir, zh_dir):
    for fn in sorted(os.listdir(ja_dir)):
        if not fn.endswith(".json"):
            continue
        zpath = os.path.join(zh_dir, fn)
        if not os.path.exists(zpath):
            continue
        ja = json.load(open(os.path.join(ja_dir, fn), encoding="utf-8-sig"))
        zh = json.load(open(zpath, encoding="utf-8-sig"))
        if len(ja) != len(zh):
            raise SystemExit(f"{fn}: entry count differs")
        yield fn, ja, zh


def cmd_fill(args):
    entries = json.load(open(args.json, encoding="utf-8"))
    titles, names, messages = {}, collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    for fn, ja, zh in load_pairs(args.ja_dir, args.zh_dir):
        for a, b in zip(ja, zh):
            if a.get("name"):
                names[a["name"]][b.get("name") or a["name"]] += 1
            messages[a["message"]][b["message"]] += 1
            m = re.fullmatch(r"(?:@[a-z]+\d*)*◆(.+)◆", a["message"])
            if m and fn.startswith("sce_"):
                t = re.sub(r"^◆|◆$", "", b["message"].strip())
                titles.setdefault(m.group(1), t)
    manual = {"by_id": {}, "by_ja": {}}
    if args.manual:
        manual.update(json.load(open(args.manual, encoding="utf-8")))
    stats = collections.Counter()
    for e in entries:
        zh, src = "", ""
        ja = e["ja"]
        if e["group"] == "chapter" and ja in titles:
            zh, src = titles[ja], "sce"
        elif e["group"] in ("name", "phone", "default_name") and ja in names:
            zh, src = names[ja].most_common(1)[0][0], "speaker"
        elif ja in messages and len(messages[ja]) == 1 and WIDE.search(ja):
            zh, src = next(iter(messages[ja])), "message"
        if not zh and ja in manual["by_ja"]:
            zh, src = manual["by_ja"][ja], "manual"
        if e["id"] in manual["by_id"]:
            zh, src = manual["by_id"][e["id"]], "manual"
        e["zh"], e["src"] = zh, src
        stats[src or "untranslated"] += 1
    json.dump(entries, open(args.json, "w", encoding="utf-8", newline="\n"), ensure_ascii=False, indent=1)
    print("filled:", dict(stats))
    for e in entries:
        if not e["zh"]:
            print(f"  untranslated {e['id']}: {e['ja']!r}")


# ---------------------------------------------------------------- build

# same substitutions as mes_json.py, minus the ones that would break format strings / ASCII titles
REPLACE = {"・": "·", "〜": "～", "♪": "～", "♡": "～", "◯": "○"}


def encode(text):
    for k, v in REPLACE.items():
        text = text.replace(k, v)
    b = text.encode(ENC)
    if b"\0" in b:
        raise ValueError("NUL in string")
    return b


def add_section(data, name, payload):
    pe = pefile.PE(data=bytes(data), fast_load=True)
    e_lfanew = pe.DOS_HEADER.e_lfanew
    fh = e_lfanew + 4
    oh = fh + 20
    nsec = pe.FILE_HEADER.NumberOfSections
    table = oh + pe.FILE_HEADER.SizeOfOptionalHeader
    hdr = table + nsec * 40
    sal, fal = pe.OPTIONAL_HEADER.SectionAlignment, pe.OPTIONAL_HEADER.FileAlignment
    if hdr + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders or any(data[hdr:hdr + 40]):
        raise SystemExit("no room for another section header")
    last = pe.sections[-1]
    va = (last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData) + sal - 1) // sal * sal
    raw = (len(data) + fal - 1) // fal * fal
    rsize = (len(payload) + fal - 1) // fal * fal
    data += b"\0" * (raw - len(data))
    data += payload + b"\0" * (rsize - len(payload))
    struct.pack_into("<8sIIIIIIHHI", data, hdr, name.encode().ljust(8, b"\0"), len(payload), va, rsize, raw,
                     0, 0, 0, 0, 0x40000040)  # initialized data, readable
    struct.pack_into("<H", data, fh + 2, nsec + 1)
    struct.pack_into("<I", data, oh + 56, (va + len(payload) + sal - 1) // sal * sal)  # SizeOfImage
    struct.pack_into("<I", data, oh + 8, pe.OPTIONAL_HEADER.SizeOfInitializedData + rsize)
    return IMAGE_BASE + va


def cmd_build(args):
    exe = Exe(args.exe)
    if exe.md5 != CRACK_MD5:
        raise SystemExit(f"{args.exe}: MD5 {exe.md5}, expected DC4PHDL_crack.exe ({CRACK_MD5})")
    entries = json.load(open(args.json, encoding="utf-8"))
    entries.append({"id": "font", "va": f"{FONT_VA:08X}", "ja": exe.cstr(FONT_VA).decode("cp932"), "zh": args.font,
                    "max": room_in_place(exe, FONT_VA, len(exe.cstr(FONT_VA)), exe.refs(FONT_VA)), "reloc": True})
    entries.append({"id": "mes_dir", "va": f"{MES_DIR_VA:08X}", "ja": "MES", "zh": args.mes_dir, "max": 3, "reloc": True})
    if len(args.font.encode(ENC)) > FONT_MAX:
        raise SystemExit(f"font name longer than {FONT_MAX} bytes")
    if not args.font.isascii():
        print("warning: non-ASCII font name only works where the system code page is 936; prefer the English name")
    if not re.fullmatch(r"[A-Za-z0-9_\-]{1,32}", args.mes_dir):
        raise SystemExit("--mes-dir must be a plain ASCII directory name")

    d = exe.data
    log = collections.Counter()
    for va, old, new, what in CODE_PATCHES + ([ACP_PATCH] if not args.no_acp936 else []):
        o = exe.off(va)
        if bytes(d[o:o + len(old)]) != old:
            raise SystemExit(f"unexpected bytes at {va:08X} ({what})")
        d[o:o + len(new)] = new
        log["code patches"] += 1

    moved, errors = [], []
    for e in entries:
        va = int(e["va"], 16)
        orig = exe.cstr(va)
        if orig.decode("cp932") != e["ja"]:
            errors.append(f"{e['id']}: exe has {orig.decode('cp932', 'replace')!r}, json says {e['ja']!r}")
            continue
        try:
            new = encode(e["zh"] or e["ja"])
        except (UnicodeEncodeError, ValueError) as ex:
            errors.append(f"{e['id']}: {e['zh']!r} cannot be encoded ({ex})")
            continue
        if e["id"] == "font":
            # always in a fixed 32-byte slot at the start of .chs, so it can be changed with a hex editor
            moved.insert(0, (e, va, new))
            continue
        if not e["zh"]:
            log["transcoded (no translation)"] += 1
        if new == orig:
            log["unchanged"] += 1
            continue
        if len(new) != len(orig):
            for r in exe.refs(va):
                site = strncmp_len_site(exe, r)
                if site is not None and d[site] == len(orig):
                    d[site] = len(new)
                    log["strncmp lengths"] += 1
        o = exe.off(va)
        if len(new) <= e["max"]:
            d[o:o + e["max"] + 1] = new + b"\0" * (e["max"] + 1 - len(new))
            log["in place"] += 1
        elif e["reloc"]:
            moved.append((e, va, new))
        else:
            errors.append(f"{e['id']}: {len(new)} bytes > {e['max']} available and not relocatable: {e['zh']!r}")
    if errors:
        for x in errors:
            print("ERROR", x)
        raise SystemExit(f"{len(errors)} errors")

    if moved:
        pe = pefile.PE(data=bytes(d), fast_load=True)
        last = pe.sections[-1]
        sal = pe.OPTIONAL_HEADER.SectionAlignment
        sec_va = IMAGE_BASE + (last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData) + sal - 1) // sal * sal
        payload = bytearray()
        for e, va, new in moved:
            nva = sec_va + len(payload)
            payload += new + b"\0"
            if e["id"] == "font":
                payload += b"\0" * (FONT_MAX + 1 - len(payload))
                font_slot = (nva, len(d) + (-len(d) % pe.OPTIONAL_HEADER.FileAlignment))
            payload += b"\0" * (-len(payload) % 4)
            for r in exe.refs(va):
                struct.pack_into("<I", d, r["off"], nva)
            log["relocated"] += 1
        got = add_section(d, ".chs", bytes(payload))
        assert got == sec_va
    pe = pefile.PE(data=bytes(d), fast_load=True)
    struct.pack_into("<I", d, pe.DOS_HEADER.e_lfanew + 24 + 64, pe.generate_checksum())
    os.makedirs(os.path.dirname(os.path.abspath(args.out_exe)), exist_ok=True)
    open(args.out_exe, "wb").write(d)
    print(f"wrote {args.out_exe} ({len(d)} bytes): " + ", ".join(f"{k} {v}" for k, v in log.items()))
    print(f"  font {args.font!r}: VA {font_slot[0]:08X}, file offset 0x{font_slot[1]:X}, {FONT_MAX} bytes + NUL")
    for e, va, new in moved:
        if e["id"] != "font":
            print(f"  relocated {e['id']}: {e['zh']!r} ({len(new)} > {e['max']} bytes)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("export")
    p.add_argument("exe")
    p.add_argument("json")
    p = sub.add_parser("fill")
    p.add_argument("json")
    p.add_argument("ja_dir")
    p.add_argument("zh_dir")
    p.add_argument("--manual")
    p = sub.add_parser("build")
    p.add_argument("exe")
    p.add_argument("json")
    p.add_argument("out_exe")
    p.add_argument("--font", default="SimHei", help="font face name (English GDI name, max 31 bytes)")
    p.add_argument("--mes-dir", default="MES_CHS", help="directory under Advdata the scripts are read from")
    p.add_argument("--no-acp936", action="store_true", help="keep GetACP(): GBK only parses on a CP936 system")
    args = ap.parse_args()
    {"export": cmd_export, "fill": cmd_fill, "build": cmd_build}[args.cmd](args)


if __name__ == "__main__":
    main()
