/* Dump the whole call graph as `entry<TAB>size<TAB>callee,callee,...`.
 *
 * The strategic question this exists to answer is "how much of this binary can we ignore".
 * 31,086 functions is not a tractable reading list, but the gameplay-relevant subset is not
 * 31,086 — everything the simulation does is reachable from the Lua API, because that is the
 * boundary the game's own scripts talk through. Exporting the graph lets that subset be
 * measured rather than guessed at.
 *
 * Emitted as plain text so the reachability analysis itself happens in Python, where it is
 * easy to re-run with different roots and depth limits without paying Ghidra start-up again.
 *
 * Read-only.
 *
 * @category Recoil Metal
 */

import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class DumpCallGraph extends GhidraScript {

    @Override
    public void run() throws Exception {
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        StringBuilder line = new StringBuilder();
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();

            // A sorted set so the output is stable between runs — this file gets diffed.
            Set<String> callees = new TreeSet<>();
            for (Instruction insn : currentProgram.getListing()
                     .getInstructions(f.getBody(), true)) {
                for (Reference r : insn.getReferencesFrom()) {
                    if (!r.getReferenceType().isCall()) {
                        continue;
                    }
                    Address to = r.getToAddress();
                    Function callee = getFunctionAt(to);
                    // Indirect and unresolved calls are dropped rather than guessed at; they
                    // make the reachable set an UNDER-estimate, which is the safe direction
                    // for a claim of the form "this much is enough".
                    if (callee != null) {
                        callees.add(callee.getEntryPoint().toString());
                    }
                }
            }

            line.setLength(0);
            line.append(f.getEntryPoint()).append('\t')
                .append(f.getBody().getNumAddresses()).append('\t')
                .append(String.join(",", callees));
            println(line.toString());
        }
    }
}
