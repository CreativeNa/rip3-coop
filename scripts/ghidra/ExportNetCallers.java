// Decompiles functions that call HawkNL thunks (and their direct callers)
// so the packet format and the synced state can be read.
// Args: <iat_map.csv (unused, kept for run_headless.cmd)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ExportNetCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        Files.createDirectories(outDir);
        Map<Function, Set<String>> why = new LinkedHashMap<>();
        Set<Function> level1 = new LinkedHashSet<>();

        for (Function thunk : currentProgram.getFunctionManager().getFunctions(true)) {
            String api = null;
            for (Reference r : getReferencesFrom(thunk.getEntryPoint())) {
                Symbol s = getSymbolAt(r.getToAddress());
                if (s != null && s.getName().startsWith("PTR_nl")) api = s.getName().substring(4);
            }
            if (api == null || thunk.getBody().getNumAddresses() > 8) continue;
            for (Reference r : getReferencesTo(thunk.getEntryPoint())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null || f.equals(thunk)) continue;
                level1.add(f);
                why.computeIfAbsent(f, k -> new TreeSet<>()).add(api);
            }
        }
        // direct callers of the send/receive wrappers
        for (Function f : new ArrayList<>(level1)) {
            Set<String> w = why.get(f);
            if (!(w.contains("nlWrite") || w.contains("nlRead"))) continue;
            for (Function c : f.getCallingFunctions(monitor)) {
                why.computeIfAbsent(c, k -> new TreeSet<>()).add("calls:" + f.getEntryPoint());
            }
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter idx = new PrintWriter(outDir.resolve("net_index.txt").toFile(), "UTF-8");
             PrintWriter all = new PrintWriter(outDir.resolve("net_decompiled.c").toFile(), "UTF-8")) {
            for (Map.Entry<Function, Set<String>> e : why.entrySet()) {
                Function f = e.getKey();
                idx.println(f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " " + e.getValue());
                DecompileResults res = di.decompileFunction(f, 90, monitor);
                all.println("// ==== " + f.getEntryPoint() + " " + e.getValue());
                all.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
        println("net functions exported: " + why.size());
    }
}
