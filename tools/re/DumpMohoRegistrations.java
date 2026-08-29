/* Dump the Moho Lua registration tables from an imported Forged Alliance executable.
 *
 * Claim C-011 established that the engine binds each Lua-visible class through a
 * generated CScrLuaMetatableFactory<T>, and that the binary contains 60 strings of
 * the form "moho.<name>_methods". Each such string is read by exactly the code that
 * builds that class's method table, and the table itself is an array of
 * { const char *name, int (*fn)(lua_State *) } pairs in read-only data.
 *
 * This script walks that structure mechanically:
 *   1. find every defined string starting with "moho."
 *   2. for each, report the functions that reference it
 *   3. scan read-only data for arrays of (pointer-to-string, pointer-to-function)
 *      pairs and emit each entry
 *
 * Output is TSV on stdout so it can be diffed between binaries and pasted into the
 * symbol ledger. Nothing is written back into the Ghidra database: this script is a
 * reader, so re-running it can never corrupt the analysis project.
 *
 * @category Recoil Metal
 */

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class DumpMohoRegistrations extends GhidraScript {

    /** A "moho.*" string found in the image, with the address it lives at. */
    private static final class MohoString {
        final Address addr;
        final String text;
        MohoString(Address addr, String text) { this.addr = addr; this.text = text; }
    }

    @Override
    public void run() throws Exception {
        List<MohoString> mohoStrings = collectMohoStrings();
        println("# moho.* strings found: " + mohoStrings.size());

        println("## SECTION strings");
        println("address\tstring\treferencing_functions");
        ReferenceManager refs = currentProgram.getReferenceManager();
        for (MohoString s : mohoStrings) {
            StringBuilder owners = new StringBuilder();
            for (Reference r : refs.getReferencesTo(s.addr)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (owners.length() > 0) {
                    owners.append(',');
                }
                owners.append(f == null ? r.getFromAddress().toString()
                                        : f.getName() + "@" + f.getEntryPoint());
            }
            println(s.addr + "\t" + s.text + "\t" + (owners.length() == 0 ? "-" : owners));
        }

        dumpLuaRegArrays();
    }

    /** Every defined string in the program whose text starts with "moho.". */
    private List<MohoString> collectMohoStrings() {
        List<MohoString> out = new ArrayList<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
            if (sdi == StringDataInstance.NULL_INSTANCE) {
                continue;
            }
            String v = sdi.getStringValue();
            if (v != null && v.startsWith("moho.")) {
                out.add(new MohoString(d.getMinAddress(), v));
            }
        }
        return out;
    }

    /*
     * Scan initialised data for luaL_reg-shaped arrays.
     *
     * A luaL_reg entry is two pointers: the method name and the C function. The array
     * is NULL-terminated. Rather than trusting any single heuristic, an entry is only
     * accepted when the first pointer really lands on a printable C string and the
     * second really lands inside a function Ghidra has already identified, which makes
     * a false positive very unlikely.
     */
    private void dumpLuaRegArrays() throws Exception {
        println("");
        println("## SECTION registration_entries");
        println("array_start\tindex\tlua_name\tnative_addr\tnative_symbol");

        int ptrSize = currentProgram.getDefaultPointerSize();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isInitialized() || block.isExecute()) {
                continue;
            }
            Address addr = block.getStart();
            Address end = block.getEnd();
            while (addr.compareTo(end) < 0 && !monitor.isCancelled()) {
                List<String[]> entries = readRegArrayAt(addr, ptrSize, end);
                if (entries.size() >= 3) {
                    for (int i = 0; i < entries.size(); i++) {
                        String[] e = entries.get(i);
                        println(addr + "\t" + i + "\t" + e[0] + "\t" + e[1] + "\t" + e[2]);
                    }
                    // Skip past the array we just consumed, plus its NULL terminator.
                    addr = addr.add((long) (entries.size() + 1) * 2 * ptrSize);
                    continue;
                }
                addr = addr.add(ptrSize);
            }
        }
    }

    /** Read a run of valid (name, function) pairs starting at {@code start}. */
    private List<String[]> readRegArrayAt(Address start, int ptrSize, Address blockEnd) {
        List<String[]> out = new ArrayList<>();
        Address cur = start;
        while (true) {
            if (cur.add((long) 2 * ptrSize - 1).compareTo(blockEnd) > 0) {
                return out;
            }
            Address namePtr, fnPtr;
            try {
                namePtr = toAddr(readPointer(cur, ptrSize));
                fnPtr = toAddr(readPointer(cur.add(ptrSize), ptrSize));
            } catch (Exception e) {
                return out;
            }
            String name = readCString(namePtr);
            if (name == null) {
                return out;
            }
            Function f = getFunctionContaining(fnPtr);
            if (f == null) {
                return out;
            }
            out.add(new String[] { name, fnPtr.toString(), f.getName() });
            cur = cur.add((long) 2 * ptrSize);
        }
    }

    private long readPointer(Address a, int size) throws Exception {
        return size == 8 ? currentProgram.getMemory().getLong(a)
                         : Integer.toUnsignedLong(currentProgram.getMemory().getInt(a));
    }

    /**
     * Read a NUL-terminated identifier-ish string, or null if the bytes there are not
     * plausibly a Lua method name. Keeping this strict is what suppresses false arrays.
     */
    private String readCString(Address a) {
        if (a == null || a.getOffset() == 0) {
            return null;
        }
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 64; i++) {
                byte b = currentProgram.getMemory().getByte(a.add(i));
                if (b == 0) {
                    // Lua method names are never empty and are always short identifiers.
                    return sb.length() == 0 ? null : sb.toString();
                }
                char c = (char) (b & 0xFF);
                boolean ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9') || c == '_';
                if (!ok) {
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
