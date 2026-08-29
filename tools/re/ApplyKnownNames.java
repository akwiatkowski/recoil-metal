/* Push everything the campaign has recovered back into the Ghidra database.
 *
 * WHY THIS EXISTS. All the accumulated knowledge — 1,182 Lua callables with signatures and
 * documentation, 797 real method addresses, the BSim candidates — lives in TSV files beside
 * the project, not in it. So every Ghidra session so far has opened a binary of 31,086
 * functions called `FUN_xxxxxxxx` and had to re-derive context from a text file in another
 * window. That is a fine way to answer one question and a hopeless way to run a campaign
 * across many sessions.
 *
 * This applies the TSVs to the program: names, and a plate comment on each function carrying
 * the evidence. After a run, opening the executable shows the Lua API by name, and the
 * decompiler's call sites read as `fa_lua_Unit_GetHealth(...)` instead of `FUN_006cb7a0(...)`.
 *
 * THREE RULES, all about not destroying information:
 *
 *   1. **Never overwrite a name Ghidra derived itself.** RTTI and the demangler produce real
 *      names; ours are no better. Only `FUN_`/`SUB_`/`thunk_FUN_` placeholders are replaced.
 *   2. **Confidence is encoded in the name.** `fa_` is recovered fact — the Lua registration
 *      scan, which is arity-verified for 549 of them. `maybe_` is a BSim candidate at roughly
 *      80% accuracy. A future session must be able to see the difference without consulting
 *      the ledger, because it will not consult the ledger.
 *   3. **The comment carries the provenance**, so a name can always be traced back to the
 *      claim that produced it.
 *
 * This is the one script in `tools/re/` that WRITES to the database. Everything is
 * recoverable: the project is rebuildable from the preserved executable with
 * `build/re-fa/run-import.sh`, and it lives under the gitignored `build/`.
 *
 * Usage:
 *   -postScript ApplyKnownNames.java <annotated.tsv> [<bsim-frontier.tsv>] [--dry-run]
 *
 * @category Recoil Metal
 */

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyKnownNames extends GhidraScript {

    private int named;
    private int commented;
    private int skippedAlreadyNamed;
    private int missing;
    private boolean dryRun;

    private static boolean isPlaceholder(String name) {
        return name.startsWith("FUN_") || name.startsWith("thunk_FUN_")
            || name.startsWith("SUB_") || name.startsWith("UndefinedFunction_");
    }

    /** Ghidra symbols cannot contain arbitrary punctuation; keep it to identifier characters. */
    private static String sanitise(String s) {
        return s.replaceAll("[^A-Za-z0-9_]", "_").replaceAll("_+", "_");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: ApplyKnownNames <annotated.tsv> [<bsim.tsv>] [--dry-run]");
            return;
        }
        for (String a : args) {
            if (a.equals("--dry-run")) {
                dryRun = true;
            }
        }
        println("# mode: " + (dryRun ? "DRY RUN, nothing written" : "APPLYING"));

        applyLuaApi(args[0]);
        if (args.length > 1 && !args[1].startsWith("--")) {
            applyBsimCandidates(args[1]);
        }

        println("# named: " + named);
        println("# commented: " + commented);
        println("# skipped (already had a real name): " + skippedAlreadyNamed);
        println("# no function at address: " + missing);
    }

    /**
     * The Lua API: recovered fact. Names the real method `fa_<subsystem>_<Scope>_<Name>` and
     * the generated wrapper `fa_luathunk_<Scope>_<Name>`, and plates both with the signature,
     * the engine's own documentation where it ships one, and the evidence.
     */
    private void applyLuaApi(String tsv) throws Exception {
        List<String> lines = Files.readAllLines(Path.of(tsv));
        String[] header = lines.get(0).split("\t", -1);
        int cSub = indexOf(header, "subsystem"), cCanon = indexOf(header, "canonical");
        int cPurpose = indexOf(header, "purpose"), cSource = indexOf(header, "purpose_source");
        int cScope = indexOf(header, "scope"), cName = indexOf(header, "name");
        int cSig = indexOf(header, "signature"), cMethod = indexOf(header, "method_va");
        int cWrap = indexOf(header, "wrapper_va");

        for (String line : lines.subList(1, lines.size())) {
            String[] f = line.split("\t", -1);
            if (f.length <= cWrap) {
                continue;
            }
            String scope = f[cScope], name = f[cName], sig = f[cSig];
            String purpose = f[cPurpose], source = f[cSource];

            String plate = "Lua: " + (scope.equals("<global>") ? "" : scope + ":") + name
                + "\nsignature: " + sig
                + "\npurpose (" + source + "): " + purpose
                + "\nsubsystem: " + f[cSub]
                + "\nevidence: C-032 registration scan; C-039 arity check"
                + (f[cMethod].equals("-") ? "" : "; C-035 thunk followed");

            if (!f[cMethod].equals("-")) {
                apply(f[cMethod], sanitise(f[cCanon]), plate);
            }
            apply(f[cWrap], sanitise("fa_luathunk_" + (scope.equals("<global>") ? "" : scope + "_")
                                     + name), plate + "\n(generated Lua wrapper thunk)");
        }
    }

    /**
     * BSim candidates: leads, not facts. Prefixed `maybe_` so no future session mistakes an
     * ~80%-accurate guess for something that was read.
     */
    private void applyBsimCandidates(String tsv) throws Exception {
        for (String line : Files.readAllLines(Path.of(tsv))) {
            String[] f = line.split("\t", -1);
            if (f.length != 6 || f[0].startsWith("#") || f[0].equals("query_addr")) {
                continue;
            }
            String match = f[2];
            if (match.startsWith("FUN_")) {
                continue;   // the DLL side was unnamed too: no information to transfer
            }
            double significance = Double.parseDouble(f[5]);
            if (significance < 40.0) {
                continue;   // below this the control run showed the match is not trustworthy
            }
            String plate = "BSim candidate: " + match
                + "\nfrom: " + f[3] + "  similarity " + f[4] + "  significance " + f[5]
                + "\nevidence: C-041 (~80% accurate; NOT confirmed — verify before relying)";
            apply(f[0], sanitise("maybe_" + match), plate);
        }
    }

    private void apply(String hexAddress, String newName, String plate) {
        Address addr;
        try {
            addr = toAddr(Long.parseLong(hexAddress.trim(), 16));
        } catch (Exception e) {
            ++missing;
            return;
        }
        Function f = getFunctionContaining(addr);
        if (f == null) {
            // The registration tables know about functions Ghidra's own analysis never
            // reached, because they are only referenced through data. Creating them is the
            // point: they are the Lua API.
            if (!dryRun) {
                f = createFunction(addr, null);
            }
            if (f == null) {
                ++missing;
                return;
            }
        }
        if (!isPlaceholder(f.getName())) {
            ++skippedAlreadyNamed;
            return;     // rule 1: never overwrite what Ghidra derived
        }
        if (!dryRun) {
            try {
                f.setName(newName, SourceType.USER_DEFINED);
                setPlateComment(f.getEntryPoint(), plate);
            } catch (Exception e) {
                ++missing;
                return;
            }
        }
        ++named;
        ++commented;
    }

    private static int indexOf(String[] header, String column) {
        for (int i = 0; i < header.length; i++) {
            if (header[i].equals(column)) {
                return i;
            }
        }
        throw new IllegalArgumentException("no column named " + column);
    }
}
