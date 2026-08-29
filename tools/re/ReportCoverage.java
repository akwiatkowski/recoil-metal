/* How much of the binary has actually been identified, as numbers rather than impressions.
 *
 * A reverse-engineering campaign accumulates a feeling of progress that is easy to mistake
 * for coverage. This prints the denominators: how much code exists, how much of it Ghidra
 * even recognises as functions, and how much carries a name that means anything.
 *
 * "Named" here means a symbol Ghidra did not invent. Its auto-generated names all start with
 * a known prefix (FUN_, thunk_FUN_, SUB_, ...), so anything else came from RTTI, from a
 * demangled export, or from an analyst.
 *
 * Read-only.
 *
 * @category Recoil Metal
 */

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;

public class ReportCoverage extends GhidraScript {

    private static boolean isAutoName(String name) {
        return name.startsWith("FUN_")
            || name.startsWith("thunk_FUN_")
            || name.startsWith("SUB_")
            || name.startsWith("UndefinedFunction_");
    }

    @Override
    public void run() throws Exception {
        long textSize = 0;
        long initialisedSize = 0;
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isInitialized()) {
                continue;
            }
            initialisedSize += block.getSize();
            if (block.isExecute()) {
                textSize += block.getSize();
            }
        }

        long functions = 0;
        long named = 0;
        long thunks = 0;
        long bytesInFunctions = 0;
        long bytesInNamed = 0;

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            long size = f.getBody().getNumAddresses();
            ++functions;
            bytesInFunctions += size;
            if (f.isThunk()) {
                ++thunks;
            }
            if (!isAutoName(f.getName())) {
                ++named;
                bytesInNamed += size;
            }
        }

        println("executable_bytes\t" + textSize);
        println("initialised_bytes\t" + initialisedSize);
        println("functions\t" + functions);
        println("functions_thunk\t" + thunks);
        println("functions_named\t" + named);
        println("bytes_in_functions\t" + bytesInFunctions);
        println("bytes_in_named_functions\t" + bytesInNamed);
    }
}
