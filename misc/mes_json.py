"""Export / import D.C.4 Plus Harmony (Circus, mes version 0xABB6) text as VNTranslationTools JSON.

  python mes_json.py export <mes_dir> <json_dir>
  python mes_json.py import <mes_dir> <json_dir> <out_mes_dir> [--enc gbk] [--width 25] [--ruby strip|keep]

JSON format is the one written by VNTextPatch (JsonScript): [{"name"?, "message"}], read by LinguaGacha
as MESSAGEJSON. Export never emits "names" (see collect()); import still accepts it.

Text opcodes (0x6A-0x6D are "encstr", bytes stored as b - 0x20; 0x5D is a plain string):
  0x6A  speaker name
  0x6B  dialogue / narration (hand-wrapped, ~25 full-width chars per line, max 3 lines)
  0x6D  phone/chat message ("「name」text"), always a single line in the original (up to 34 chars)
  0x6C  choice / map destination / config sample text
  0x5D  scene-replay titles (sce_*.mes)

Export strips the leading control codes (@t105@d1 ...) and the narration indent, and joins
the hand-wrapped lines of 0x6B. Import restores them from the original script and re-wraps 0x6B.
Untranslated strings (identical to the exported text) are written back byte-for-byte.
"""
import argparse
import collections
import json
import os
import re
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from circus_mes import Mes, encode_enc, encode_str, op_kind  # noqa: E402

NAME_OP = 0x6A
# only 0x6B is hand-wrapped; phone messages (0x6D) are single lines laid out by the phone UI
WRAP_OPS = {0x6B}
TEXT_OPS = {0x5D, 0x6A, 0x6B, 0x6C, 0x6D}
SRC_ENC = "cp932"

CODE_RE = re.compile(r"@[a-z]+[0-9]*")
PREFIX_RE = re.compile(r"^((?:@[a-z]+[0-9]*|\u3000)*)([「『（]?)((?:@[a-z]+[0-9]*)*)(.*)$", re.S)
RUBY_RE = re.compile(r"｛([^／｝]*)／([^｝]*)｝")
LONE_RUBY_RE = re.compile(r"｛([^／｛｝]*)｝")
NO_LINE_START = set("、。，．・：；？！ー―…‥」』）】〉》〕］｝ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ～〜♪,.!?)]}")
SJIS_RUBY = {"｛": b"\x81\x6f", "／": b"\x81\x5e", "｝": b"\x81\x70"}
# "%" and "&" are made full-width: the engine expands "&f"/"&n" (player name) and the text may
# pass through printf-style formatting; the original scripts only use the full-width forms.
DEFAULT_REPLACE = {"・": "·", "〜": "～", "♪": "～", "♡": "～", "◯": "○", " ": "\u3000", "%": "％", "&": "＆"}


def split_prefix(s):
    m = PREFIX_RE.match(s)
    return m.group(1), m.group(2), m.group(3), m.group(4)


def join_lines(body, dialogue):
    if dialogue:
        body = body.replace("\n\u3000", "")
    return body.replace("\n", "")


def export_view(op, raw_text):
    """Return text shown to translators, or None if nothing translatable."""
    pre, br, post, body = split_prefix(raw_text)
    text = br + body
    if op in WRAP_OPS:
        text = join_lines(text, bool(br))
    if not CODE_RE.sub("", text).strip("\u3000\n"):
        return None
    return text


def collect(mes):
    slots = []
    for i, t in enumerate(mes.tokens):
        if t.op not in TEXT_OPS:
            continue
        raw = t.text_bytes().decode(SRC_ENC)
        view = export_view(t.op, raw)
        if view is None:
            continue
        slots.append({"idx": i, "op": t.op, "raw": raw, "text": view})
    # A name not followed by a message is never shown (dc4_nin20190425b.mes has one right before
    # another speaker). Exporting it would produce "names": [...], and LinguaGacha only uses names[0].
    # Import translates such names via the file's other name entries.
    return [s for k, s in enumerate(slots)
            if s["op"] != NAME_OP or (k + 1 < len(slots) and slots[k + 1]["op"] != NAME_OP)]


def slots_to_entries(slots):
    entries, names = [], []
    for s in slots:
        if s["op"] == NAME_OP:
            names.append(s["text"])
            continue
        e = {}
        if len(names) == 1:
            e["name"] = names[0]
        elif names:
            e["names"] = names
        e["message"] = s["text"]
        entries.append(e)
        names = []
    if names:
        raise ValueError("trailing speaker name without message")
    return entries


def entries_to_strings(entries):
    out = []
    for e in entries:
        if e.get("name"):
            out.append(e["name"])
        elif e.get("names"):
            out.extend(e["names"])
        out.append(e["message"])
    return out


def char_width(c):
    return 2 if unicodedata.east_asian_width(c) in "WFA" else 1


def units_of(text, ruby_mode):
    """Split into unbreakable units: (string, display width). Codes stick to the next char."""
    units, pending = [], ""
    pos = 0
    while pos < len(text):
        m = CODE_RE.match(text, pos)
        if m:
            pending += m.group(0)
            pos = m.end()
            continue
        m = RUBY_RE.match(text, pos)
        if m:
            base = m.group(2)
            s = m.group(0) if ruby_mode == "keep" else base
            units.append((pending + s, sum(char_width(c) for c in base), base[:1]))
            pending = ""
            pos = m.end()
            continue
        c = text[pos]
        units.append((pending + c, 0 if c == "\n" else char_width(c), c))
        pending = ""
        pos += 1
    if pending:
        units.append((pending, 0, ""))
    return units


def wrap(text, width, indent, first_width=0, hang=4):
    """Greedy wrap; `indent` starts every continuation line.

    Kinsoku: closing punctuation never starts a line. It may hang past `width` by up to `hang`
    half-width units (the original scripts go to 27 characters at most); beyond that, the trailing
    characters of the line are pushed down together with the punctuation.
    """
    indent_w = char_width("\u3000") * len(indent)
    lines, cur, w = [], [], first_width

    def content(units):
        return any(s.strip("\u3000") for s, _, _ in units)

    for unit in text:
        s, uw, first = unit
        if first == "\n":
            lines.append(cur)
            cur, w = [], indent_w
            continue
        if w + uw > width and content(cur):
            if first not in NO_LINE_START:
                lines.append(cur)
                cur, w = [], indent_w
            elif w + uw > width + hang:
                k = len(cur)
                while k > 0 and cur[k - 1][2] in NO_LINE_START:
                    k -= 1
                k -= 1  # also move the character the punctuation belongs to
                if k > 0 and content(cur[:k]):
                    lines.append(cur[:k])
                    cur = cur[k:]
                    w = indent_w + sum(u[1] for u in cur)
        cur.append(unit)
        w += uw
    lines.append(cur)
    out = []
    for i, ln in enumerate(lines):
        s = "".join(u[0] for u in ln)
        out.append(s if i == 0 else indent + s)
    return "\n".join(out)


def rebuild_text(op, raw, new, width, ruby_mode, max_lines=3):
    pre, br, post, _ = split_prefix(raw)
    new = new.replace("\r\n", "\n")
    if "\u3000" in pre and new.startswith("\u3000"):
        new = new[1:]
    if br and new.startswith(br):
        body = br + post + new[1:]
    else:
        body = post + new
    if ruby_mode == "strip":
        body = RUBY_RE.sub(r"\2", body)
        # translators sometimes keep only the braces around the base text: ｛三美｝
        body = LONE_RUBY_RE.sub(r"\1", body)

    orig_codes = CODE_RE.findall(raw)
    have = CODE_RE.findall(pre + body)
    missing = list(orig_codes)
    for c in have:
        if c in missing:
            missing.remove(c)
    if missing:
        body = "".join(missing) + body

    if op in WRAP_OPS and width > 0:
        dialogue = bool(br) or CODE_RE.sub("", new)[:1] in ("「", "『", "（")
        first_w = sum(char_width(c) for c in CODE_RE.sub("", pre))
        units = units_of(body, ruby_mode)
        wrapped = wrap(units, width * 2, "\u3000" if dialogue else "", first_w)
        if wrapped.count("\n") >= max_lines:
            # the original scripts occasionally use 26 characters per line; try that before overflowing
            wider = wrap(units, (width + 1) * 2, "\u3000" if dialogue else "", first_w)
            if wider.count("\n") < wrapped.count("\n"):
                wrapped = wider
        body = wrapped
    return pre + body, missing


def fallback_char(c, enc):
    """Strip diacritics from letters the target encoding lacks (Doppelgänger -> Doppelganger)."""
    base = "".join(ch for ch in unicodedata.normalize("NFKD", c) if not unicodedata.combining(ch))
    if base and base != c:
        try:
            return base.encode(enc)
        except UnicodeEncodeError:
            pass
    return None


def encode_text(s, enc, replace, ruby_mode, folded=None):
    for k, v in replace.items():
        s = s.replace(k, v)
    out, bad = bytearray(), set()
    pos = 0
    while pos < len(s):
        c = s[pos]
        if ruby_mode == "keep" and enc != SRC_ENC and c in SJIS_RUBY:
            out += SJIS_RUBY[c]
        else:
            try:
                out += c.encode(enc)
            except UnicodeEncodeError:
                fb = fallback_char(c, enc)
                if fb is not None:
                    out += fb
                    if folded is not None:
                        folded.add(c)
                else:
                    bad.add(c)
                    out += b"?"
        pos += 1
    if 0x00 in out:
        raise ValueError("text contains byte 0x00")
    return bytes(out), bad


def encode_token(op, text_bytes):
    if op_kind(op) == "enc":
        if 0x20 in text_bytes:
            raise ValueError("byte 0x20 would become the 0x00 terminator in an encrypted string")
        return encode_enc(op, text_bytes)
    return encode_str(op, text_bytes)


def cmd_export(args):
    os.makedirs(args.json_dir, exist_ok=True)
    nfiles = nstr = 0
    for name in sorted(os.listdir(args.mes_dir)):
        if not name.lower().endswith(".mes"):
            continue
        mes = Mes(open(os.path.join(args.mes_dir, name), "rb").read())
        slots = collect(mes)
        if not slots:
            continue
        entries = slots_to_entries(slots)
        with open(os.path.join(args.json_dir, os.path.splitext(name)[0] + ".json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(entries, f, ensure_ascii=False, indent=2)
        nfiles += 1
        nstr += len(slots)
    print(f"exported {nstr} strings from {nfiles} files")


def cmd_import(args):
    os.makedirs(args.out_dir, exist_ok=True)
    replace = dict(DEFAULT_REPLACE) if args.enc != SRC_ENC else {}
    total_changed = 0
    bad_chars = {}
    folded = set()
    warnings = []
    for name in sorted(os.listdir(args.mes_dir)):
        if not name.lower().endswith(".mes"):
            continue
        src_path = os.path.join(args.mes_dir, name)
        data = open(src_path, "rb").read()
        jpath = os.path.join(args.json_dir, os.path.splitext(name)[0] + ".json")
        mes = Mes(data)
        translated = {}
        if os.path.exists(jpath):
            slots = collect(mes)
            strings = entries_to_strings(json.load(open(jpath, encoding="utf-8-sig")))
            if len(strings) != len(slots):
                raise SystemExit(f"{name}: json has {len(strings)} strings, script has {len(slots)}")
            translated = {s["idx"]: new for s, new in zip(slots, strings) if new != s["text"]}
            name_map = collections.defaultdict(collections.Counter)
            for s, new in zip(slots, strings):
                if s["op"] == NAME_OP:
                    name_map[s["text"]][new] += 1
            exported = {s["idx"] for s in slots}
            for idx, tok in enumerate(mes.tokens):
                if tok.op == NAME_OP and idx not in exported:
                    view = export_view(tok.op, tok.text_bytes().decode(SRC_ENC))
                    if view in name_map:
                        new = name_map[view].most_common(1)[0][0]
                        if new != view:
                            translated[idx] = new
        repl = {}
        for idx, tok in enumerate(mes.tokens):
            if tok.op not in TEXT_OPS:
                continue
            raw = tok.text_bytes().decode(SRC_ENC)
            if idx in translated:
                new = translated[idx]
                for k, v in replace.items():
                    new = new.replace(k, v)  # before wrapping: " " -> "　" changes the width
                text, missing = rebuild_text(tok.op, raw, new, args.width, args.ruby, args.max_lines)
                if missing:
                    warnings.append(f"{name}: re-inserted dropped codes {missing} at line start: {new[:30]}")
                if args.ruby == "strip" and ("｛" in text or "｝" in text):
                    warnings.append(f"{name}: malformed ruby (expected ｛ruby／base｝) left as is: {new[:30]}")
                if tok.op in WRAP_OPS and text.count("\n") >= args.max_lines:
                    warnings.append(f"{name}: {text.count(chr(10)) + 1} lines (box has {args.max_lines}): {new[:30]}")
            elif args.enc != SRC_ENC:
                # untranslated text still has to be transcoded, otherwise SJIS bytes show up as garbage
                text = RUBY_RE.sub(r"\2", raw) if args.ruby == "strip" else raw
            else:
                continue
            b, bad = encode_text(text, args.enc, replace, args.ruby, folded)
            for c in bad:
                bad_chars.setdefault(c, name)
            repl[idx] = encode_token(tok.op, b)
        open(os.path.join(args.out_dir, name), "wb").write(mes.build(repl))
        total_changed += len(repl)
    for w in warnings[:args.max_warnings]:
        print("WARN", w)
    if len(warnings) > args.max_warnings:
        print(f"... {len(warnings) - args.max_warnings} more warnings")
    if folded:
        print("FOLDED (diacritics removed):", "".join(sorted(folded)))
    if bad_chars:
        print("UNENCODABLE (written as '?'):", {c: f for c, f in bad_chars.items()})
    print(f"imported {total_changed} changed strings, {len(warnings)} warnings")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("mes_dir")
    e.add_argument("json_dir")
    i = sub.add_parser("import")
    i.add_argument("mes_dir")
    i.add_argument("json_dir")
    i.add_argument("out_dir")
    i.add_argument("--enc", default="gbk", help="gbk (needs exe charset patch) or cp932 (for SjisTunnel/VNTextProxy route)")
    i.add_argument("--width", type=int, default=25, help="full-width chars per line for 0x6B, 0 = no auto wrap")
    i.add_argument("--max-lines", type=int, default=3, help="text box height; longer text is retried at width+1, then warned")
    i.add_argument("--ruby", choices=("strip", "keep"), default="strip")
    i.add_argument("--max-warnings", type=int, default=30)
    args = ap.parse_args()
    {"export": cmd_export, "import": cmd_import}[args.cmd](args)


if __name__ == "__main__":
    main()
