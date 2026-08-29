/* Reveal the layout of the Moho Lua class-registration records.
 *
 * DumpMohoRegistrations showed that every "moho.<class>_methods" string is referenced
 * from a contiguous data region rather than from code, and that the records are not
 * plain luaL_reg { name, fn } arrays -- so the generic scan misses them.
 *
 * This script takes the opposite approach: start from the known string references and
 * print the surrounding words with every pointer resolved, so the record shape can be
 * read off directly instead of guessed.
 *
 * Each word is annotated as one of:
 *   FN:<name>@<addr>   points into a function Ghidra identified
 *   STR:"<text>"       points at a printable C string
 *   DAT:<symbol>       points at a labelled datum
 *   PTR:<addr>         points somewhere mapped but unlabelled
 *   <hex>              not a plausible pointer, shown as a raw value
 *
 * Read-only: this script never writes to the program database.
 *
 * @category Recoil Metal
 */

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DumpMohoRegLayout extends GhidraScript {

    /** Words to show either side of the reference site. */
    private static final int WORDS_BEFORE = 6;
    private static final int WORDS_AFTER = 24;

    @Override
    public void run() throws Exception {
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
            if (sdi == StringDataInstance.NULL_INSTANCE) {
                continue;
            }
            String v = sdi.getStringValue();
            // Only the class method tables; the constructor-name strings share a shape.
            if (v == null || !v.startsWith("moho.") || !v.endsWith("_methods")) {
                continue;
            }
            for (Reference r : currentProgram.getReferenceManager()
                                             .getReferencesTo(d.getMinAddress())) {
                println("");
                println("### " + v + "  string@" + d.getMinAddress()
                        + "  referenced_from@" + r.getFromAddress()
                        + "  block=" + blockNameOf(r.getFromAddress()));
                dumpAround(r.getFromAddress());
            }
        }
    }

    private String blockNameOf(Address a) {
        var b = currentProgram.getMemory().getBlock(a);
        return b == null ? "?" : b.getName() + (b.isWrite() ? ":rw" : ":ro");
    }

    private void dumpAround(Address site) {
        int ps = currentProgram.getDefaultPointerSize();
        for (int i = -WORDS_BEFORE; i <= WORDS_AFTER; i++) {
            Address a;
            try {
                a = site.add((long) i * ps);
            } catch (Exception e) {
                continue;
            }
            long raw;
            try {
                raw = ps == 8 ? currentProgram.getMemory().getLong(a)
                              : Integer.toUnsignedLong(currentProgram.getMemory().getInt(a));
            } catch (Exception e) {
                continue;
            }
            println(String.format("%s %+4d  %08x  %s",
                    a, i * ps, raw, describe(raw)));
        }
    }

    private String describe(long raw) {
        if (raw == 0) {
            return "NULL";
        }
        Address t;
        try {
            t = toAddr(raw);
        } catch (Exception e) {
            return "";
        }
        if (currentProgram.getMemory().getBlock(t) == null) {
            return "";
        }
        Function f = getFunctionContaining(t);
        if (f != null) {
            return "FN:" + f.getName() + "@" + f.getEntryPoint();
        }
        String s = readIdent(t);
        if (s != null) {
            return "STR:\"" + s + "\"";
        }
        Symbol sym = getSymbolAt(t);
        if (sym != null) {
            return "DAT:" + sym.getName();
        }
        return "PTR:" + t;
    }

    /** A printable, NUL-terminated string of reasonable length, or null. */
    private String readIdent(Address a) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 96; i++) {
                byte b = currentProgram.getMemory().getByte(a.add(i));
                if (b == 0) {
                    return sb.length() == 0 ? null : sb.toString();
                }
                char c = (char) (b & 0xFF);
                if (c < 0x20 || c > 0x7e) {
                    return null;
                }
                sb.append(c);
            }
        } catch (Exception e) {
            return null;
        }
        return null;
    }
}
