#!/usr/bin/env python3
"""Second pass over the Supreme Commander wiki: everything that is not a unit page.

`fetch_wiki.py` mirrors the four factions' unit pages. This adds the pages that
explain *systems* rather than units — how weapons resolve, what the controls are,
how a faction plays — which is the register the blueprints are silent in.

Shares the image pool and merges into the same manifest, so run it after
`fetch_wiki.py`, not alongside.

    python3 tools/fetch_wiki_extras.py reference/supcom-wiki
"""

import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_wiki as fw  # noqa: E402 - path must be set first

OUT = sys.argv[1] if len(sys.argv) > 1 else "reference/supcom-wiki"

# Sections are how an agent finds these later; the split is by question asked,
# not by the wiki's own taxonomy.
SECTIONS: dict[str, dict] = {
    # How weapons resolve — the register FA-WEAPONS and FA-DAMAGE need.
    "weapons": {
        "categories": ["Weapons", "UEF weapons", "Aeon weapons",
                       "Cybran weapons", "Seraphim weapons"],
        "pages": ["UEF weaponry", "Aeon weaponry", "Cybran weaponry",
                  "Seraphim weaponry", "Blueprint/Weapon", "Lua Weapon",
                  "Death weapon", "Damage over time", "Damage per second",
                  "Muzzle velocity", "Firing randomness", "Overkill", "EMP"],
    },
    # What each faction is FOR — the design intent behind the stat lines.
    "factions": {
        "categories": ["Factions", "Faction strategies"],
        "pages": ["UEF", "Aeon", "Cybran", "Seraphim", "Faction"],
    },
    # How the game is played, which is what a UI or an AI has to support.
    "strategies": {
        "categories": ["Strategies"],
        "pages": [],
    },
    # The interface contract: what a player can actually ask the engine to do.
    "interface": {
        "categories": [],
        "pages": ["Controls", "Multiplayer", "Blueprint"],
    },
}


def fetch_page(title: str, section: str, seen_images: set[str],
               manifest: dict, failures: list[str]) -> bool:
    """One page to markdown plus its images. Returns False if it was skipped."""
    if title in manifest["pages"]:
        return False  # already mirrored as a unit page; do not duplicate
    try:
        data = fw.api({"action": "parse", "page": title,
                       "prop": "text|wikitext|categories"})
    except RuntimeError as exc:
        failures.append(f"{title}: {exc}")
        return False
    parse = data.get("parse")
    if not parse:
        failures.append(f"{title}: no parse result")
        return False

    cleaner = fw.Cleaner()
    cleaner.feed(parse["text"])
    body = fw.to_markdown("".join(cleaner.out))

    imgs = []
    for name, url in cleaner.images:
        fname = fw.safe_name(name)
        imgs.append(fname)
        if fname in seen_images:
            continue
        seen_images.add(fname)
        if not fw.download(url, os.path.join(OUT, "images", fname)):
            failures.append(f"image {name}")
        time.sleep(0.05)

    slug = fw.safe_name(title)
    cats = [c["category"].replace("_", " ") for c in parse.get("categories", [])]
    header = "\n".join([
        "---",
        f"title: {title}",
        f"section: {section}",
        f"url: https://supcom.fandom.com/wiki/{urllib.parse.quote(title.replace(' ', '_'))}",
        f"categories: {', '.join(cats)}",
        "evidence_tier: WEB — behaviour and intent only; numbers come from BP-R blueprints",
        "---",
        "",
        f"# {title}",
        "",
    ])
    os.makedirs(os.path.join(OUT, "pages", section), exist_ok=True)
    with open(os.path.join(OUT, "pages", section, slug + ".md"), "w") as fh:
        fh.write(header + body)
    with open(os.path.join(OUT, "wikitext", slug + ".wiki"), "w") as fh:
        fh.write(parse.get("wikitext", ""))

    manifest["pages"][title] = {
        "section": section,
        "path": f"pages/{section}/{slug}.md",
        "images": sorted(set(imgs)),
    }
    return True


def main() -> None:
    manifest_path = os.path.join(OUT, "manifest.json")
    with open(manifest_path) as fh:
        manifest = json.load(fh)
    failures: list[str] = list(manifest.get("failures", []))
    seen_images = {f for f in os.listdir(os.path.join(OUT, "images"))}
    manifest.setdefault("sections", {})

    for section, spec in SECTIONS.items():
        titles: list[str] = list(spec["pages"])
        for cat in spec["categories"]:
            titles.extend(fw.category_members(cat))
            time.sleep(fw.DELAY)
        titles = sorted(set(titles))
        written = []
        print(f"{section}: {len(titles)} candidate pages", flush=True)
        for title in titles:
            if fetch_page(title, section, seen_images, manifest, failures):
                written.append(title)
            time.sleep(fw.DELAY)
        manifest["sections"][section] = written
        print(f"  {section}: wrote {len(written)}", flush=True)

    manifest["failures"] = failures
    with open(manifest_path, "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)

    # Append the new sections to the index the first pass wrote.
    with open(os.path.join(OUT, "INDEX.md"), "a") as fh:
        fh.write("\n# Systems, not units\n\n")
        for section, titles in manifest["sections"].items():
            fh.write(f"## {section} ({len(titles)} pages)\n\n")
            for title in sorted(titles):
                fh.write(f"- [{title}](pages/{section}/{fw.safe_name(title)}.md)\n")
            fh.write("\n")

    total = sum(len(v) for v in manifest["sections"].values())
    print(f"\nextra pages: {total}  images now: {len(seen_images)}  failures: {len(failures)}")


if __name__ == "__main__":
    main()
