#!/usr/bin/env python3
"""Mirror the Supreme Commander Fandom wiki's unit pages for local agent use.

Why this exists: the wiki describes unit *behaviour and role* — hover units are
torpedo-immune, which unit outranges which — in a register no blueprint carries.
The HTML pages are 403 to any client, but api.php answers normally, so the mirror
goes through the API.

Output layout (all gitignored):
    reference/supcom-wiki/units/<Faction>/<Page>.md   cleaned prose + infobox
    reference/supcom-wiki/wikitext/<Page>.wiki        the source, for exact quoting
    reference/supcom-wiki/images/<File>.png           every image the page shows
    reference/supcom-wiki/INDEX.md                    one line per page
    reference/supcom-wiki/manifest.json               machine-readable record

Not committed, and not evidence: this is `WEB` tier. Numbers come from the
shipped blueprints (`BP-R`); this mirror is for behaviour, naming and hypotheses.
"""

import html
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from html.parser import HTMLParser

API = "https://supcom.fandom.com/api.php"
UA = "recoil-metal-research/1.0 (local mirror for engine-parity research)"
OUT = sys.argv[1] if len(sys.argv) > 1 else "reference/supcom-wiki"
DELAY = 0.25  # polite: four requests a second at most, single-threaded

FACTIONS = ["UEF units", "Aeon units", "Cybran units", "Seraphim units"]

# Navigation furniture. These tables are the same on every page and swamp the
# prose — a single unit page renders 50 KB of HTML, of which ~40 KB is these.
DROP_CLASSES = {"unitnav", "tnavbar", "navbox", "toc", "mw-editsection", "noprint"}


def api(params: dict) -> dict:
    """One API call, with three retries and a plain error on give-up."""
    params = {**params, "format": "json", "formatversion": 2}
    url = f"{API}?{urllib.parse.urlencode(params)}"
    last = None
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30) as fh:
                return json.load(fh)
        except Exception as exc:  # noqa: BLE001 - retry any transport failure
            last = exc
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"API failed after 3 tries: {url}\n{last}")


class Cleaner(HTMLParser):
    """Drops whole subtrees whose class matches DROP_CLASSES, keeps the rest.

    Written against the stdlib parser rather than BeautifulSoup so the mirror
    needs no dependency — the repo's rule about not adding one for a throwaway.
    Depth counting is what makes it safe with nested tables: once a drop starts,
    every tag is counted until its own closing tag arrives.
    """

    VOID = {"br", "img", "hr", "meta", "link", "input", "col", "source"}
    # Layout-only wrappers. Kept as content, dropped as tags: pandoc emits a real
    # pipe table for a cell holding text, and falls back to raw HTML for one
    # holding a <div>. Unwrapping these is the difference between a readable
    # stat table and 50 lines of markup per unit.
    UNWRAP = {"div", "span", "font", "center", "small", "big"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=False)
        self.out: list[str] = []
        self.drop_depth = 0        # >0 while inside a dropped subtree
        self.tag_stack: list[tuple[str, bool]] = []  # (tag, was it emitted?)
        self.images: list[tuple[str, str]] = []  # (file name, url)

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        classes = set((a.get("class") or "").split())
        if self.drop_depth == 0 and classes & DROP_CLASSES:
            self.drop_depth = 1
            self.tag_stack.append((tag, False))
            return
        if self.drop_depth:
            if tag not in self.VOID:
                self.drop_depth += 1
                self.tag_stack.append((tag, False))
            return
        if tag in self.UNWRAP:
            self.tag_stack.append((tag, False))
            return

        if tag == "img":
            # Fandom lazy-loads: the real URL hides in data-src, and src holds a
            # 1x1 base64 GIF. Prefer data-src, and never record the placeholder.
            src = a.get("data-src") or a.get("src") or ""
            name = a.get("data-image-name") or ""
            if src.startswith("http") and name:
                self.images.append((name, src))
                self.out.append(f'<img src="images/{safe_name(name)}" alt="{html.escape(name)}" />')
            return
        if tag in self.VOID:
            return
        self.out.append(f"<{tag}>")
        self.tag_stack.append((tag, True))

    def handle_endtag(self, tag):
        if tag in self.VOID:
            return
        if self.drop_depth:
            if self.tag_stack and self.tag_stack[-1][0] == tag:
                self.tag_stack.pop()
                self.drop_depth -= 1
            return
        emitted = True
        if self.tag_stack and self.tag_stack[-1][0] == tag:
            emitted = self.tag_stack.pop()[1]
        elif tag in self.UNWRAP:
            emitted = False
        if emitted:
            self.out.append(f"</{tag}>")

    def handle_data(self, data):
        if not self.drop_depth:
            self.out.append(html.escape(data))

    def handle_entityref(self, name):
        if not self.drop_depth:
            self.out.append(f"&{name};")

    def handle_charref(self, name):
        if not self.drop_depth:
            self.out.append(f"&#{name};")


def safe_name(name: str) -> str:
    """A file name that survives every filesystem, with the extension intact."""
    return re.sub(r"[^A-Za-z0-9._-]", "_", name.replace(" ", "_"))


def to_markdown(clean_html: str) -> str:
    """pandoc does the conversion; it is already on this machine."""
    proc = subprocess.run(
        ["pandoc", "-f", "html", "-t", "gfm", "--wrap=none"],
        input=clean_html.encode(), capture_output=True, check=False,
    )
    if proc.returncode != 0:
        return "<!-- pandoc failed: " + proc.stderr.decode()[:200] + " -->"
    return proc.stdout.decode()


def full_size(url: str) -> str:
    """Strip Fandom's thumbnailer so the mirror keeps the original image.

    A build icon served at scale-to-width-down/32 is 32 px; the same path without
    that segment is the source PNG.
    """
    return re.sub(r"/revision/latest/scale-to-width-down/\d+", "/revision/latest", url)


def download(url: str, path: str) -> bool:
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return True
    try:
        req = urllib.request.Request(full_size(url), headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=30) as fh:
            data = fh.read()
        if not data:
            return False
        with open(path, "wb") as out:
            out.write(data)
        return True
    except Exception:  # noqa: BLE001 - a missing image must not stop the mirror
        return False


def category_members(cat: str) -> list[str]:
    """Page titles in a category, following one level of subcategory."""
    titles: list[str] = []
    cont = {}
    while True:
        data = api({"action": "query", "list": "categorymembers",
                    "cmtitle": f"Category:{cat}", "cmlimit": 500, **cont})
        for m in data.get("query", {}).get("categorymembers", []):
            if m["title"].startswith("Category:"):
                titles.extend(category_members(m["title"][len("Category:"):]))
            elif m["ns"] == 0:
                titles.append(m["title"])
        if "continue" not in data:
            return titles
        cont = data["continue"]
        time.sleep(DELAY)


def main() -> None:
    for sub in ("units", "wikitext", "images"):
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)

    manifest: dict = {"source": "https://supcom.fandom.com", "pages": {}, "factions": {}}
    seen_images: set[str] = set()
    index_rows: list[tuple[str, str, str]] = []
    failures: list[str] = []

    for faction in FACTIONS:
        short = faction.replace(" units", "")
        titles = sorted(set(category_members(faction)))
        manifest["factions"][short] = titles
        os.makedirs(os.path.join(OUT, "units", short), exist_ok=True)
        print(f"{short}: {len(titles)} pages", flush=True)

        for i, title in enumerate(titles, 1):
            try:
                data = api({"action": "parse", "page": title, "prop": "text|wikitext|categories"})
            except RuntimeError as exc:
                failures.append(f"{title}: {exc}")
                continue
            parse = data.get("parse")
            if not parse:
                failures.append(f"{title}: no parse result")
                continue

            cleaner = Cleaner()
            cleaner.feed(parse["text"])
            body = to_markdown("".join(cleaner.out))
            wikitext = parse.get("wikitext", "")

            # Images: dedupe globally, so a build icon shared by four pages is
            # fetched once and linked four times.
            imgs = []
            for name, url in cleaner.images:
                fname = safe_name(name)
                imgs.append(fname)
                if fname in seen_images:
                    continue
                seen_images.add(fname)
                if not download(url, os.path.join(OUT, "images", fname)):
                    failures.append(f"image {name}")
                time.sleep(0.05)

            slug = safe_name(title)
            cats = [c["category"].replace("_", " ") for c in parse.get("categories", [])]
            header = "\n".join([
                "---",
                f"title: {title}",
                f"faction: {short}",
                f"url: https://supcom.fandom.com/wiki/{urllib.parse.quote(title.replace(' ', '_'))}",
                f"categories: {', '.join(cats)}",
                "evidence_tier: WEB — behaviour and role only; numbers come from BP-R blueprints",
                "---",
                "",
                f"# {title}",
                "",
            ])
            with open(os.path.join(OUT, "units", short, slug + ".md"), "w") as fh:
                fh.write(header + body)
            with open(os.path.join(OUT, "wikitext", slug + ".wiki"), "w") as fh:
                fh.write(wikitext)

            manifest["pages"][title] = {
                "faction": short,
                "path": f"units/{short}/{slug}.md",
                "images": sorted(set(imgs)),
            }
            index_rows.append((short, title, f"units/{short}/{slug}.md"))
            if i % 25 == 0:
                print(f"  {short} {i}/{len(titles)}", flush=True)
            time.sleep(DELAY)

    with open(os.path.join(OUT, "INDEX.md"), "w") as fh:
        fh.write("# Supreme Commander wiki mirror\n\n")
        fh.write("Local, gitignored, fetched through `api.php` (the HTML pages 403 to any client).\n\n")
        fh.write("**Evidence tier: `WEB`.** Good for behaviour, role and naming — what a unit *does*,\n")
        fh.write("which units counter it, why a hover tank ignores torpedoes. Bad for numbers: the\n")
        fh.write("figures here drift between vanilla, FA and FAF patches, while the shipped\n")
        fh.write("blueprints on disk (`BP-R`) do not. Never cite this mirror in a claim without\n")
        fh.write("blueprint or EXE support.\n\n")
        for short in sorted({r[0] for r in index_rows}):
            rows = sorted(r for r in index_rows if r[0] == short)
            fh.write(f"## {short} ({len(rows)} pages)\n\n")
            for _, title, path in rows:
                fh.write(f"- [{title}]({path})\n")
            fh.write("\n")

    manifest["failures"] = failures
    with open(os.path.join(OUT, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)

    print(f"\npages: {len(manifest['pages'])}  images: {len(seen_images)}  failures: {len(failures)}")
    for f in failures[:20]:
        print("  !", f)


if __name__ == "__main__":
    main()
