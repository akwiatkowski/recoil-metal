/* Query the stripped executable's functions against the named DLL's BSim signatures.
 *
 * `ART-E001` has 943 meaningful function names out of 31,086. `ART-D001` has 21,152 out of
 * 43,877, and `C-002` established that both were built from the same Perforce tree. BSim
 * compares decompiled-function feature vectors rather than bytes, so it can survive the
 * recompilation drift between the 2007 DLL and the 2011 executable, which byte matching
 * cannot.
 *
 * REPORTS BY DEFAULT, and only renames when explicitly told to. That ordering is the whole
 * point: a name transferred wrongly is worse than no name, because every later session would
 * trust it. Run it once to measure, check the control set, then decide a threshold.
 *
 * Output is TSV on stdout:
 *   query_addr  query_name  match_name  match_exe  similarity  significance
 *
 * Usage (analyzeHeadless -postScript BSimTransferNames.java <args>):
 *   <bsimURL>                       required, e.g. file:/path/to/fadb
 *   --threshold <0..1>              minimum similarity to report   (default 0.70)
 *   --significance <n>              minimum significance           (default 0.0)
 *   --limit <n>                     query only the first n functions (default: all)
 *   --named-only                    query only functions that ALREADY have a real name;
 *                                   this is the control set, used to measure false positives
 *   --apply                         actually rename; without it nothing is written
 *
 * @category Recoil Metal
 */

import java.net.URL;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import generic.lsh.vector.LSHVectorFactory;
import ghidra.app.script.GhidraScript;
import ghidra.features.bsim.query.BSimClientFactory;
import ghidra.features.bsim.query.FunctionDatabase;
import ghidra.features.bsim.query.GenSignatures;
import ghidra.features.bsim.query.description.FunctionDescription;
import ghidra.features.bsim.query.protocol.QueryNearest;
import ghidra.features.bsim.query.protocol.ResponseNearest;
import ghidra.features.bsim.query.protocol.SimilarityNote;
import ghidra.features.bsim.query.protocol.SimilarityResult;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;

public class BSimTransferNames extends GhidraScript {

    private static boolean isAutoName(String name) {
        return name.startsWith("FUN_") || name.startsWith("thunk_FUN_")
            || name.startsWith("SUB_") || name.startsWith("UndefinedFunction_");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: BSimTransferNames <bsimURL> [--threshold f] [--limit n] "
                    + "[--named-only] [--apply]");
            return;
        }
        String dbUrl = args[0];
        double threshold = 0.70;
        double significance = 0.0;
        int limit = Integer.MAX_VALUE;
        boolean namedOnly = false;
        boolean apply = false;
        String addressFile = null;
        for (int i = 1; i < args.length; i++) {
            switch (args[i]) {
                case "--threshold" -> threshold = Double.parseDouble(args[++i]);
                case "--significance" -> significance = Double.parseDouble(args[++i]);
                case "--limit" -> limit = Integer.parseInt(args[++i]);
                case "--named-only" -> namedOnly = true;
                case "--apply" -> apply = true;
                case "--addresses" -> addressFile = args[++i];
                default -> println("# ignoring unknown argument: " + args[i]);
            }
        }

        URL url = BSimClientFactory.deriveBSimURL(dbUrl);
        try (FunctionDatabase client = BSimClientFactory.buildClient(url, false)) {
            if (!client.initialize()) {
                println("# cannot connect: " + client.getLastError());
                return;
            }
            LSHVectorFactory vectorFactory = client.getLSHVectorFactory();

            // Collect the functions to ask about. Thunks are skipped: they are five bytes of
            // jump, they match everything, and naming them teaches nothing.
            List<Function> targets = new ArrayList<>();
            if (addressFile != null) {
                // An explicit list, which is how the validation runs work: query functions
                // whose identity is already known by other means and see whether BSim agrees.
                for (String line : java.nio.file.Files.readAllLines(
                        java.nio.file.Path.of(addressFile))) {
                    String text = line.trim();
                    if (text.isEmpty() || text.startsWith("#")) {
                        continue;
                    }
                    Function f = getFunctionAt(toAddr(Long.decode("0x" + text.replace("0x", ""))));
                    if (f != null && !f.isThunk()) {
                        targets.add(f);
                    }
                }
            } else {
                FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
                while (it.hasNext() && targets.size() < limit) {
                    Function f = it.next();
                    if (f.isThunk()) {
                        continue;
                    }
                    if (namedOnly == isAutoName(f.getName())) {
                        continue;   // --named-only wants named; otherwise wants unnamed
                    }
                    targets.add(f);
                }
            }
            println("# querying " + targets.size() + " functions, threshold " + threshold);

            GenSignatures gensig = new GenSignatures(false);
            gensig.setVectorFactory(vectorFactory);
            gensig.openProgram(currentProgram, null, null, null, null, null);
            Iterator<Function> iter = targets.iterator();
            gensig.scanFunctions(iter, targets.size(), monitor);

            QueryNearest query = new QueryNearest();
            query.manage = gensig.getDescriptionManager();
            query.max = 1;                       // only the best match is actionable
            query.thresh = threshold;
            query.signifthresh = significance;

            ResponseNearest response = query.execute(client);
            if (response == null) {
                println("# query failed: " + client.getLastError());
                return;
            }

            println("query_addr\tquery_name\tmatch_name\tmatch_exe\tsimilarity\tsignificance");
            int renamed = 0;
            for (SimilarityResult result : response.result) {
                FunctionDescription base = result.getBase();
                for (SimilarityNote note : result) {
                    FunctionDescription match = note.getFunctionDescription();
                    String matchName = match.getFunctionName();
                    println(String.format("%08x\t%s\t%s\t%s\t%.4f\t%.2f",
                            base.getAddress(), base.getFunctionName(), matchName,
                            match.getExecutableRecord().getNameExec(),
                            note.getSimilarity(), note.getSignificance()));

                    if (apply && !isAutoName(matchName)) {
                        Function f = getFunctionAt(toAddr(base.getAddress()));
                        if (f != null && isAutoName(f.getName())) {
                            f.setName(matchName, SourceType.ANALYSIS);
                            ++renamed;
                        }
                    }
                }
            }
            println("# results: " + response.result.size()
                    + (apply ? (", renamed: " + renamed) : ", renamed: 0 (report only)"));
        }
    }
}
