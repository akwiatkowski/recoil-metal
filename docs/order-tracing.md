# Order lifecycle diagnostics

Add `--log-level debug --log-file build/recoil-orders.log` to a normal match or
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

## Resource flow

`mise exec -- make skirmish LOG_LEVEL=debug LOG_FILE=build/economy.log` enables
resource diagnostics every five seconds of simulation time. `[economy]` records
show each army's stored resources, production, actual spending and requested
spending per second, followed by contributing units with their blueprint IDs.
Construction (including assisted work) is attributed to its founding builder;
repairs and ammunition are charged to their builder and owner respectively.
Maintenance charges are apportioned across units from the allocator's aggregate
maintenance request. These readings do not change the simulation or saved state.
Reclaim credits, one-shot weapon charges and allied overflow are not included in
the per-unit production/spending breakdown; this is the recurring economy pass.

Selecting a unit or building shows `MASS /s` and `ENERGY /s` as `+production /
-spending`. A group of one type shows the sum. Spending includes funded
construction, repairs, ammunition and maintenance, so a stalled builder shows
what it actually receives. The top resource panel uses the same actual spending.

ACUs spawn unenhanced. Their enhancement-only missile weapons must not create
ammunition production records: doing so previously charged an idle UEF ACU
120 energy/s for tactical ammunition, and later started its nuclear ammunition.
The SCMP_009 opening with one extractor and one power generator now has
5 + 20 - 2 = +23 energy/s and refills storage. The commander's +5 energy/s is the
existing engine starting trickle, not the retail blueprint's +20.
