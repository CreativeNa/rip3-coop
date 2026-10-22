// Exports every printable string in the image that something references,
// with the referencing functions, to <out dir>/strings.tsv:
//   address  length  refcount  functions  text
// Also dumps the EN/RU menu pointer tables (0x4F1340..0x4F1520) to lang_table.tsv.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.Charset;
import java.nio.file.*;
import java.util.*;

public class ExportStrings extends GhidraScript {
    Charset cp = Charset.forName("windows-1251");

    String readStr(Address a, int max) throws Exception {
        Memory m = currentProgram.getMemory();
        byte[] buf = new byte[max];
        int n = 0;
        for (; n < max; n++) {
            byte b = m.getByte(a.add(n));
            if (b == 0) break;
            int u = b & 0xFF;
            if (u < 0x20 && u != '\n' && u != '\r' && u != '\t') return null;
            buf[n] = b;
        }
        if (n == max) return null;
        return new String(buf, 0, n, cp);
    }

    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        ReferenceManager rm = currentProgram.getReferenceManager();
        Memory m = currentProgram.getMemory();
        Address lo = toAddr(0x4B9000L), hi = toAddr(0x4F0000L);

        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(
                new FileOutputStream(outDir.resolve("strings.tsv").toFile()), "UTF-8"))) {
            AddressIterator it = rm.getReferenceDestinationIterator(new AddressSet(lo, hi), true);
            while (it.hasNext()) {
                Address a = it.next();
                String s = readStr(a, 1024);
                if (s == null || s.length() < 2) continue;
                Set<String> funcs = new TreeSet<>();
                int refs = 0;
                for (Reference r : getReferencesTo(a)) {
                    refs++;
                    Function f = getFunctionContaining(r.getFromAddress());
                    funcs.add(f == null ? r.getFromAddress().toString() : f.getEntryPoint().toString());
                }
                byte[] raw = s.getBytes(cp);
                out.println(a + "\t" + raw.length + "\t" + refs + "\t" + String.join(",", funcs) + "\t"
                    + s.replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r"));
            }
        }

        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(
                new FileOutputStream(outDir.resolve("lang_table.tsv").toFile()), "UTF-8"))) {
            for (long p = 0x4F1300L; p < 0x4F1560L; p += 4) {
                Address slot = toAddr(p);
                long v = m.getInt(slot) & 0xFFFFFFFFL;
                String s = (v >= 0x400000L && v < 0x520000L) ? readStr(toAddr(v), 256) : null;
                out.println(slot + "\t" + Long.toHexString(v) + "\t" + (s == null ? "" : s));
            }
        }
        println("strings exported");
    }
}
