# Order lifecycle diagnostics

Add `--log-level debug --log-file /tmp/recoil-orders.log` to a normal match or
replay invocation. The existing logger writes timestamped records to the file
and stderr. Its default `info` level omits these diagnostics.

`[order]` records contain simulation tick, semantic command ID, generation-safe
unit handle (`slot:generation`), command kind, player and stage:

- `submitted` or `intake-rejected`: entry through the shared command buffer.
- `accepted` or `rejected`: the dispatch result for each requested unit.

`[construction]` records join to orders by builder handle and tick. They report
`started`, `stalled`, `resumed`, `throttled`, `fully-funded` and `completed`.
Completion refers to construction work; unit creation still goes through the
normal spawn path. Funding records include the actual cached ratio and product
type index. They are emitted only when crossing zero, partial or full funding,
not for every percentage change or every tick.

The dispatcher currently returns accepted handles, not typed rejection reasons;
the trace reports its actual result without inventing a cause. Construction
events do not carry command IDs, so queued/repeated builds are correlated by
builder and tick, while command intake and dispatch use the exact semantic ID.
Tracing reads the existing event stream and keeps a temporary snapshot of active
funding only when debug logging is enabled; it changes no saved or hashed state.

`mise exec -- ./build/rm_tests '[order-trace]'` runs a real engineer build, an
unauthorized Stop, resource starvation, recovery and completion. It checks all
stages and bounds the output across more than 1,000 simulation ticks.
