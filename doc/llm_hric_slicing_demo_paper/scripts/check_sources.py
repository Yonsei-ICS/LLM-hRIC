#!/usr/bin/env python3
"""Lightweight checks for hosts without a TeX installation."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    tex_files = sorted(ROOT.rglob("*.tex"))
    if not tex_files:
        raise SystemExit("no TeX files found")
    combined = "\n".join(path.read_text() for path in tex_files)
    for path in tex_files:
        text = re.sub(r"(?<!\\)%.*", "", path.read_text())
        if text.count("{") != text.count("}"):
            raise SystemExit(f"unbalanced braces: {path}")
        begins = re.findall(r"\\begin\{([^}]+)\}", text)
        ends = re.findall(r"\\end\{([^}]+)\}", text)
        if sorted(begins) != sorted(ends):
            raise SystemExit(f"unbalanced environments: {path}")
    bib_keys = set(re.findall(r"^@\w+\{([^,]+),", (ROOT / "references.bib").read_text(), re.MULTILINE))
    cited = set()
    for citation in re.findall(r"\\cite\{([^}]+)\}", combined):
        cited.update(item.strip() for item in citation.split(","))
    missing = cited - bib_keys
    if missing:
        raise SystemExit(f"missing BibTeX keys: {sorted(missing)}")
    required = [
        ROOT / "generated/results_status.tex",
        ROOT / "generated/performance_table.tex",
        ROOT / "generated/control_table.tex",
        ROOT / "generated/guided_vs_ddpg_table.tex",
        ROOT / "generated/ddpg_only_phase_table.tex",
        ROOT / "generated/ddpg_only_priority_table.tex",
    ]
    absent = [str(path) for path in required if not path.exists()]
    if absent:
        raise SystemExit(f"generate results first; missing: {absent}")
    print(f"checked {len(tex_files)} TeX files and {len(cited)} citations")


if __name__ == "__main__":
    main()
