# /// script
# dependencies = ["fonttools"]
# ///
"""Recover the patch's character substitutions by exact glyph-outline matching."""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import sys

from fontTools.ttLib import TTFont


def canonical_contour(points):
    if not points:
        return ()
    candidates = []
    for direction in [points, list(reversed(points))]:
        smallest = min(direction)
        for i, point in enumerate(direction):
            if point == smallest:
                candidates.append(tuple(direction[i:] + direction[:i]))
    return min(candidates)


def glyph_fingerprint(font, glyph_name, ignore_metrics=False, normalize_position=False):
    glyph = font["glyf"][glyph_name]
    coordinates, endpoints, flags = glyph.getCoordinates(font["glyf"])
    units = font["head"].unitsPerEm
    origin_x = min((p[0] for p in coordinates), default=0) if normalize_position else 0
    origin_y = min((p[1] for p in coordinates), default=0) if normalize_position else 0
    contours = []
    start = 0
    for end in endpoints:
        points = [(round((coordinates[i][0] - origin_x) * 256 / units, 8),
                   round((coordinates[i][1] - origin_y) * 256 / units, 8), flags[i] & 1) for i in range(start, end + 1)]
        contours.append(canonical_contour(points))
        start = end + 1
    # Hinting and contour order can change when a font is subsetted/remapped.
    # Outline geometry and metrics must still be identical.
    advance, bearing = font["hmtx"][glyph_name]
    value = (round(advance * 256 / units, 8), round(bearing * 256 / units, 8), tuple(sorted(contours)))
    if ignore_metrics:
        value = tuple(sorted(contours))
    elif normalize_position:
        value = (round(advance * 256 / units, 8), tuple(sorted(contours)))
    return hashlib.sha256(repr(value).encode("ascii")).hexdigest()


def select_character(encoded, candidates):
    candidates = sorted(set(candidates))
    if encoded < 128 and encoded in candidates:
        return encoded
    cjk = [c for c in candidates if 0x3400 <= c <= 0x9FFF]
    if len(cjk) == 1:
        return cjk[0]
    noncompat = [c for c in candidates if not (0xF900 <= c <= 0xFAFF or 0x2F00 <= c <= 0x2FDF)]
    if len(noncompat) == 1:
        return noncompat[0]
    if len(candidates) == 1:
        return candidates[0]
    return None


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, default=root / "tmp" / "molebox" / "files" / "Font")
    parser.add_argument("--reference", type=Path, action="append")
    parser.add_argument("--font-number", type=int, default=0)
    parser.add_argument("--ignore-metrics", action="store_true")
    parser.add_argument("--normalize-position", action="store_true")
    parser.add_argument("--overrides", type=Path)
    parser.add_argument("--output", type=Path, default=root / "tmp" / "translation_memory")
    args = parser.parse_args()
    target = TTFont(str(args.font))
    target_cmap = target.getBestCmap()
    modes = [False, True] if args.normalize_position else [False]
    target_fingerprints = {mode: {c: glyph_fingerprint(target, g, args.ignore_metrics, mode)
                                 for c, g in target_cmap.items()} for mode in modes}
    mapping, ambiguous, unmatched = {}, [], []
    provenance = {}
    references = []
    for path in args.reference or [root / "tools" / "simhei_302.ttf"]:
        reference = TTFont(str(path), fontNumber=args.font_number)
        reference_cmap = reference.getBestCmap()
        added = 0
        for mode in modes:
            fingerprints = collections.defaultdict(list)
            cached = {}
            for codepoint, glyph_name in reference_cmap.items():
                if glyph_name not in cached:
                    cached[glyph_name] = glyph_fingerprint(reference, glyph_name, args.ignore_metrics, mode)
                fingerprints[cached[glyph_name]].append(codepoint)
            for codepoint, fingerprint in target_fingerprints[mode].items():
                if chr(codepoint) in mapping:
                    continue
                candidates = fingerprints.get(fingerprint, [])
                chosen = select_character(codepoint, candidates)
                if chosen is not None:
                    mapping[chr(codepoint)] = chr(chosen)
                    provenance[chr(codepoint)] = dict(reference=str(path), original=chr(chosen),
                                                     normalize_position=mode, candidates=[chr(c) for c in sorted(set(candidates))])
                    added += 1
        references.append(dict(path=str(path), added=added,
                               names=[(n.nameID, n.toUnicode()) for n in reference["name"].names if n.nameID in (1, 5)]))
        print("Reference:", references[-1])
        reference.close()
    if args.overrides:
        overrides = json.loads(args.overrides.read_text(encoding="utf-8"))
        for encoded, original in overrides.items():
            if len(encoded) != 1 or len(original) != 1 or ord(encoded) not in target_cmap:
                raise ValueError("Invalid manually checked character mapping")
            mapping[encoded] = original
            provenance[encoded] = dict(method="glyph-sheet-review", original=original,
                                       source=str(args.overrides))
    for codepoint, glyph_name in target_cmap.items():
        if chr(codepoint) not in mapping:
            unmatched.append(dict(encoded=chr(codepoint), codepoint=hex(codepoint), glyph=glyph_name,
                                  contours=target["glyf"][glyph_name].numberOfContours))
    print("Mapping:", len(mapping), "ambiguous:", len(ambiguous), "unmatched:", len(unmatched))
    print("Ambiguous:", ambiguous[:30])
    print("Unmatched:", unmatched[:40])
    print("Decoded sample:", "悘柈枍換熉暒悙".translate(str.maketrans(mapping)))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "font_mapping.json").write_text(json.dumps(mapping, ensure_ascii=False, indent=2), encoding="utf-8")
    report = dict(references=references, font_number=args.font_number,
                  ignore_metrics=args.ignore_metrics,
                  normalize_position=args.normalize_position,
                  font_sha256=hashlib.sha256(args.font.read_bytes()).hexdigest(),
                  mapped=len(mapping), total=len(target_cmap), ambiguous=ambiguous, unmatched=unmatched)
    (args.output / "font_mapping_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.output / "font_mapping_provenance.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
