// Decompiles the functions listed in <out dir>/want.txt (one hex address per line)
// into <out dir>/want_decompiled.c, with their callers listed.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter all = new PrintWriter(outDir.resolve("want_decompiled.c").toFile(), "UTF-8")) {
            for (String line : Files.readAllLines(outDir.resolve("want.txt"))) {
                line = line.replace("п»ї", "").trim();
                if (line.isEmpty()) continue;
                java.util.List<Function> fs = new java.util.ArrayList<>();
                if (line.contains("-")) { /* "start-end": every function in the range */
                    String[] r = line.split("-");
                    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(toAddr(Long.parseLong(r[0], 16)), true);
                    while (it.hasNext()) { Function g = it.next(); if (g.getEntryPoint().getOffset() >= Long.parseLong(r[1], 16)) break; fs.add(g); }
                } else {
                    Function g = getFunctionContaining(toAddr(Long.parseLong(line, 16)));
                    if (g == null) { all.println("// ==== " + line + " no function"); continue; }
                    fs.add(g);
                }
                for (Function f : fs) {
                StringBuilder callers = new StringBuilder();
                for (Function c : f.getCallingFunctions(monitor)) callers.append(c.getEntryPoint()).append(' ');
                all.println("// ==== " + f.getEntryPoint() + " callers: " + callers);
                DecompileResults res = di.decompileFunction(f, 900, monitor);
                all.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed");
                }
            }
        }
    }
}
