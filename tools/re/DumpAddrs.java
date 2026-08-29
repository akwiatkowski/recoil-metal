/* Dump annotated words at arbitrary addresses, for following pointer chains by hand.
 *
 * DumpMohoRegLayout recovered the shape of the Moho Lua class-registration record but
 * left two of its fields as raw .data pointers. This script exists to chase those:
 * pass addresses and a word count via the script arguments and it prints the same
 * annotated dump, so a chain can be walked without editing and recompiling a script
 * for every hop.
 *
 * Usage (analyzeHeadless):
 *   -postScript DumpAddrs.java 0x00fee88c 0x00e6d388 -- 32
 * Everything before "--" is an address; the value after it is the word count.
 *
 * Read-only: never writes to the program database.
 *
 * @category Recoil Metal
 */

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

public class DumpAddrs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> addrs = new ArrayList<>();
        int words = 16;
        boolean afterSep = false;
        for (String a : args) {
            if (a.equals("--")) {
                afterSep = true;
            } else if (afterSep) {
                words = Integer.parseInt(a);
            } else {
                addrs.add(a);
            }
        }
        for (String a : addrs) {
            Address base = toAddr(Long.decode(a));
            println("");
            println("### dump " + a + "  block=" + blockNameOf(base));
            dump(base, words);
        }
    }

    private String blockNameOf(Address a) {
        var b = currentProgram.getMemory().getBlock(a);
        return b == null ? "?" : b.getName() + (b.isWrite() ? ":rw" : ":ro");
    }

    private void dump(Address base, int words) {
        int ps = currentProgram.getDefaultPointerSize();
        for (int i = 0; i < words; i++) {
            Address a;
            long raw;
            try {
                a = base.add((long) i * ps);
                raw = ps == 8 ? currentProgram.getMemory().getLong(a)
                              : Integer.toUnsignedLong(currentProgram.getMemory().getInt(a));
            } catch (Exception e) {
                return;
            }
            println(String.format("%s %+5d  %08x  %s", a, i * ps, raw, describe(raw)));
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
