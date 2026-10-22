// Prints the entry points of functions whose names match the regex in
// <out dir>/find.txt, into <out dir>/find_result.txt.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;

public class FindFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        String re = Files.readString(outDir.resolve("find.txt")).trim();
        try (PrintWriter out = new PrintWriter(outDir.resolve("find_result.txt").toFile(), "UTF-8")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getName().matches(re))
                    out.println(f.getEntryPoint() + " " + f.getName() + " callers=" + f.getCallingFunctions(monitor).size());
            }
        }
    }
}
