<!-- Generated and maintained by Claude -->
# vendor/ — the AI corpora the adapters are written against

Nothing under `vendor/ai/` is in this repository's history. It is fetched, pinned, and
gitignored, exactly as `third_party/` is:

    make ai                  # or: tools/fetch_ai.sh
    tools/fetch_ai.sh --force

The pins live in `tools/fetch_ai.sh` and nowhere else, so they cannot drift out of agreement
with themselves. `NOTICE.md` records what each tree is, what licence it carries, and how far its
provenance can actually be trusted.

## Why this is separate from `third_party/`

Both are other people's code, both are fetched, both are ignored — but they are not the same
kind of thing, and the adapters read differently if you know which is which.

`third_party/` is **libraries this engine links**: metal-cpp, miniz. They are dependencies. You
call them and forget them.

`vendor/` is the **subject** of engine work. The AI adapters (ADR-039) exist to serve these
trees; every number in that ADR was measured on them; the binding layer is generated from
`vendor/ai/faf/engine/`. They are closer to test fixtures than to dependencies — the thing being
adapted *to*, not a tool used along the way.

## The rule

**Nothing under `vendor/ai/` is ever modified.** Not a typo fix, not a formatting pass, not a
"tiny" compatibility patch.

This is not tidiness. The whole point of adapting these AIs rather than reimplementing them is
that the FAF and BAR communities keep improving them, and a change made here is a fork — one
that silently stops receiving those improvements and that nobody remembers making. **A change
that can only be made by patching a vendored AI is a change to the adapter or to the engine, or
it is not made.**

The corollary is the useful half: when an adapter cannot make an AI work, that is information
about the adapter. Reaching into `vendor/` to make the symptom go away destroys the information.

Fetching rather than committing enforces this for free. There is no local copy to quietly edit
that survives — `--force` discards it, and nothing in the repository ever disagreed with
upstream in the first place. That is a stronger guarantee than a CI check, and it costs a
`.gitignore` line instead of 16 MB of history.

## Updating

Change the pin in `tools/fetch_ai.sh`, run `make ai --force`, and commit that one-line change
on its own. If the adapter then breaks, the commit that broke it is a single line naming the
old and new versions — which is the entire diagnosis.
