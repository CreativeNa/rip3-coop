// Lists instructions for "start-end" hex ranges in <out dir>/want.txt into <out dir>/want_listing.txt.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;

public class ListInstr extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        try (PrintWriter out = new PrintWriter(outDir.resolve("want_listing.txt").toFile(), "UTF-8")) {
            for (String line : Files.readAllLines(outDir.resolve("want.txt"))) {
                line = line.trim();
                if (!line.contains("-")) continue;
                String[] r = line.split("-");
                Address a = toAddr(Long.parseLong(r[0], 16)), end = toAddr(Long.parseLong(r[1], 16));
                out.println("==== " + line);
                InstructionIterator it = currentProgram.getListing().getInstructions(a, true);
                while (it.hasNext()) {
                    Instruction i = it.next();
                    if (i.getAddress().compareTo(end) > 0) break;
                    out.println(i.getAddress() + "  " + i);
                }
            }
        }
    }
}
