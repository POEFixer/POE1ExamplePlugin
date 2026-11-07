#!/usr/bin/env python3
"""Generate src/generated/Snippets.inc from the snippet blocks in src/.

    python tools/gen_snippets.py           write the generated file
    python tools/gen_snippets.py --check   exit 1 when the generated file is stale or a rule is broken

Why this exists: every lesson page of the Example plugin has a "Copy snippet" button. The text it
copies is compiled into the DLL, so it must never drift from the code it shows. A snippet IS the
code between two marker lines, and this script copies it into a C++ table:

    // [snippet:<id>]
    uint32_t SubmitSafeDemo(const P1Viewport& vp) {
        ...
    }
    // [/snippet]

Rules (each one is an error):
  - ids are [a-z0-9_.-]+ and unique; blocks do not nest and are always closed;
  - the first code line of a block (after comment lines) names a function, or a class or struct:
    that name is what the lesson's source chip shows;
  - a block is at most 2000 bytes of UTF-8 (the app puts at most about 2 KiB on the clipboard);
  - every `.snippet = "<id>"` in the sources names a block that exists.

Python 3.8+, standard library only. Line endings are ignored (git may check files out with CRLF).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # the plugin folder (Plugins/ExamplePlugin)
SRC = ROOT / "src"
OUT = SRC / "generated" / "Snippets.inc"
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp"}
MAX_BYTES = 2000
RAW_DELIM = "p1snip"

OPEN_RE = re.compile(r"^\s*//\s*\[snippet:([a-z0-9_.-]+)\]\s*$")
CLOSE_RE = re.compile(r"^\s*//\s*\[/snippet\]\s*$")
LOOKS_LIKE_MARKER_RE = re.compile(r"^\s*//\s*\[/?snippet")   # a marker line that failed the exact forms
TYPE_RE = re.compile(r"^\s*(?:template\s*<.*>\s*)?(class|struct)\s+([A-Za-z_]\w*)")
FUNC_RE = re.compile(r"([A-Za-z_~][\w]*(?:::[A-Za-z_~][\w]*)*)\s*\(")
REF_RE = re.compile(r"\.snippet\s*=\s*\"([^\"]*)\"")


class SnippetError(Exception):
    pass


def read_lines(path: Path) -> list[str]:
    text = path.read_bytes().decode("utf-8")      # strict: a non-UTF-8 source is an error
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def dedent(lines: list[str]) -> list[str]:
    body = [ln.rstrip() for ln in lines]
    indents = [len(ln) - len(ln.lstrip(" ")) for ln in body if ln.strip()]
    cut = min(indents) if indents else 0
    out = [ln[cut:] if ln.strip() else "" for ln in body]
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return out


def name_of(lines: list[str], where: str) -> str:
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith("//") or s.startswith("/*") or s.startswith("*"):
            continue
        if s.startswith("template"):
            continue
        m = TYPE_RE.match(s)
        if m:
            return f"{m.group(1)} {m.group(2)}"
        m = FUNC_RE.search(s)
        if m:
            return m.group(1) + "()"
        break
    raise SnippetError(f"{where}: the first code line must name a function, class or struct")


def collect() -> tuple[list[dict], set[str]]:
    snippets: list[dict] = []
    seen: dict[str, str] = {}
    refs: set[str] = set()
    files = sorted(p for p in SRC.rglob("*")
                   if p.is_file() and p.suffix in SOURCE_SUFFIXES and OUT.parent not in p.parents)
    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        lines = read_lines(path)
        for ln in lines:
            for m in REF_RE.finditer(ln):
                refs.add(m.group(1))
        open_id = None
        start = 0
        for i, ln in enumerate(lines):
            mo, mc = OPEN_RE.match(ln), CLOSE_RE.match(ln)
            if mo:
                if open_id is not None:
                    raise SnippetError(f"{rel}:{i + 1}: snippet '{mo.group(1)}' opens inside '{open_id}'")
                open_id, start = mo.group(1), i + 1
            elif mc:
                if open_id is None:
                    raise SnippetError(f"{rel}:{i + 1}: [/snippet] without an open block")
                where = f"{rel}:{start}"
                if open_id in seen:
                    raise SnippetError(f"{where}: duplicate snippet id '{open_id}' (also {seen[open_id]})")
                body = dedent(lines[start:i])
                if not body:
                    raise SnippetError(f"{where}: snippet '{open_id}' is empty")
                text = "\n".join(body)
                size = len(text.encode("utf-8"))
                if size > MAX_BYTES:
                    raise SnippetError(f"{where}: snippet '{open_id}' is {size} bytes (max {MAX_BYTES})")
                if f"){RAW_DELIM}\"" in text:
                    raise SnippetError(f"{where}: snippet '{open_id}' contains the raw-string delimiter")
                first = start + 1 + next(k for k, l in enumerate(lines[start:i]) if l.strip())
                seen[open_id] = where
                snippets.append({"id": open_id, "file": rel, "function": name_of(body, where),
                                 "line": first, "text": text})
                open_id = None
            elif LOOKS_LIKE_MARKER_RE.match(ln):
                raise SnippetError(f"{rel}:{i + 1}: malformed snippet marker: {ln.strip()}")
        if open_id is not None:
            raise SnippetError(f"{rel}:{start}: snippet '{open_id}' is never closed")
    return sorted(snippets, key=lambda s: s["id"]), refs


def c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(snippets: list[dict]) -> str:
    out = [
        "// Generated by tools/gen_snippets.py from the \"// [snippet:<id>]\" blocks under src/.",
        "// Do not edit: change the code between the markers, then run the script again.",
        "// One row per block: { id, file, function, first line, text }.",
        "// clang-format off",
    ]
    for s in snippets:
        out.append(f"{{ {c_string(s['id'])}, {c_string(s['file'])}, {c_string(s['function'])}, {s['line']},")
        out.append(f"R\"{RAW_DELIM}({s['text']}){RAW_DELIM}\" }},")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail when the generated file is stale")
    args = ap.parse_args()
    try:
        snippets, refs = collect()
        if not snippets:
            raise SnippetError("no snippet blocks found under src/")
        missing = sorted(r for r in refs if r not in {s["id"] for s in snippets})
        if missing:
            raise SnippetError("a lesson names a snippet that does not exist: " + ", ".join(missing))
    except (SnippetError, UnicodeDecodeError) as e:
        print(f"gen_snippets: {e}", file=sys.stderr)
        return 2
    text = render(snippets)
    if args.check:
        current = OUT.read_bytes().decode("utf-8").replace("\r\n", "\n") if OUT.exists() else None
        if current != text:
            print(f"gen_snippets: {OUT.relative_to(ROOT).as_posix()} is stale; run tools/gen_snippets.py",
                  file=sys.stderr)
            return 1
        print(f"gen_snippets: up to date ({len(snippets)} snippets)")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"gen_snippets: wrote {len(snippets)} snippets to {OUT.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
