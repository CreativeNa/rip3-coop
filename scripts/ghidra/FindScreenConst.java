// Finds code that uses the screen size: immediates 1024/768/512/384 (int or float bits) and
// memory operands that point at float/double constants with those values.
// Writes <out dir>/screen_const.txt: address, function, instruction.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.Reference;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class FindScreenConst extends GhidraScript {
    static final Set<Long> INTS = new HashSet<>(Arrays.asList(1024L, 768L, 512L, 384L, 1023L, 767L));
    static final Set<Float> FLOATS = new HashSet<>(Arrays.asList(1024f, 768f, 512f, 384f, 1023f, 767f));

    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        Memory mem = currentProgram.getMemory();
        try (PrintWriter out = new PrintWriter(outDir.resolve("screen_const.txt").toFile(), "UTF-8")) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction i = it.next();
                String why = null;
                for (int op = 0; op < i.getNumOperands() && why == null; op++) {
                    for (Object o : i.getOpObjects(op)) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (INTS.contains(v)) why = "int " + v;
                            else if (FLOATS.contains(Float.intBitsToFloat((int) v))) why = "float bits " + Float.intBitsToFloat((int) v);
                        }
                    }
                    for (Reference r : i.getOperandReferences(op)) {
                        Address a = r.getToAddress();
                        if (!mem.contains(a) || !r.getReferenceType().isData()) continue;
                        try {
                            float f = Float.intBitsToFloat(mem.getInt(a));
                            if (FLOATS.contains(f)) why = "float@" + a + " " + f;
                            double d = Double.longBitsToDouble(mem.getLong(a));
                            if (FLOATS.contains((float) d) && d == (float) d) why = "double@" + a + " " + d;
                        } catch (MemoryAccessException e) { }
                    }
                }
                if (why == null) continue;
                Function f = getFunctionContaining(i.getAddress());
                out.println(i.getAddress() + "\t" + (f == null ? "-" : f.getEntryPoint().toString()) + "\t" + why + "\t" + i);
            }
        }
    }
}
