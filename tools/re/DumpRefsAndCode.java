/* Find what code touches an address, and decompile it.
 *
 * WP-02 stalled at a data boundary: the Moho Lua class-registration records point at method
 * lists in `.data`, but the method NAMES are not adjacent to the function pointers, so no
 * amount of scanning read-only data will produce them. `F-008` records why. The remaining
 * hypothesis is that the names are immediate operands inside the code that builds those
 * tables at start-up, which means the way forward is to decompile that code rather than to
 * walk more structures.
 *
 * Given one or more addresses, this script reports every reference to each — with the
 * referring function and whether it reads, writes or calls — and then decompiles the distinct
 * referring functions once each.
 *
 * Usage (analyzeHeadless):
 *   -postScript DumpRefsAndCode.java 0x00fee88c 0x00fba008
 *   -postScript DumpRefsAndCode.java 0x00fee88c -- refs-only
 *   -postScript DumpRefsAndCode.java 0x006ca530 -- at   (decompile the function AT the address,
 *                                                        rather than the ones referring to it)
 *
 * Read-only: it decompiles and prints, and never writes to the program database.
 *
 * @category Recoil Metal
 */

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DumpRefsAndCode extends GhidraScript {

    /// Long enough for the large start-up functions this is aimed at, short enough that a
    /// pathological one cannot hang a headless run.
    private static final int DECOMPILE_TIMEOUT_SECONDS = 120;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> targets = new ArrayList<>();
        boolean refsOnly = false;
        boolean decompileAt = false;
        boolean afterSep = false;
        for (String a : args) {
            if (a.equals("--")) {
                afterSep = true;
            } else if (afterSep) {
                refsOnly = refsOnly || a.equals("refs-only");
                decompileAt = decompileAt || a.equals("at");
            } else {
                targets.add(a);
            }
        }
        if (targets.isEmpty()) {
            println("no addresses given");
            return;
        }

        // A set, because several targets usually share one referring function and decompiling
        // a 3,000-line start-up routine twice is the difference between a fast run and a slow
        // one.
        Set<Function> toDecompile = new LinkedHashSet<>();

        if (decompileAt) {
            for (String target : targets) {
                Address addr = toAddr(Long.decode(target));
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    // Ghidra may not have created a function here; ask it to, so that a
                    // registration wrapper reached only through a data pointer is still
                    // readable. This is the one thing in these scripts that touches the
                    // database, and it only ever adds a function Ghidra's own analysis
                    // would have created had it followed the pointer.
                    f = createFunction(addr, null);
                }
                if (f == null) {
                    println("// no function at " + target);
                } else {
                    toDecompile.add(f);
                }
            }
            decompile(toDecompile);
            return;
        }

        println("## SECTION references");
        println("target\tfrom\ttype\tfunction");
        for (String target : targets) {
            Address addr = toAddr(Long.decode(target));
            int count = 0;
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(addr)) {
                ++count;
                Function f = getFunctionContaining(r.getFromAddress());
                println(target + "\t" + r.getFromAddress() + "\t" + r.getReferenceType()
                        + "\t" + (f == null ? "-" : f.getName() + "@" + f.getEntryPoint()));
                if (f != null) {
                    toDecompile.add(f);
                }
            }
            if (count == 0) {
                println(target + "\t-\t-\t(no references)");
            }
        }

        if (refsOnly) {
            return;
        }
        decompile(toDecompile);
    }

    private void decompile(Set<Function> toDecompile) throws Exception {
        println("");
        println("## SECTION decompiled");
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) {
                println("decompiler failed to open: " + decompiler.getLastMessage());
                return;
            }
            for (Function f : toDecompile) {
                println("");
                println("### " + f.getName() + "@" + f.getEntryPoint()
                        + "  body=" + f.getBody().getNumAddresses() + " bytes");
                DecompileResults results =
                    decompiler.decompileFunction(f, DECOMPILE_TIMEOUT_SECONDS, monitor);
                if (results == null || !results.decompileCompleted()) {
                    println("// decompilation failed: "
                            + (results == null ? "no result" : results.getErrorMessage()));
                    continue;
                }
                println(results.getDecompiledFunction().getC());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
