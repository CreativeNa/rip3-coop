// Decompiles code relevant to lockstep: time sources, the RNG (MSVC LCG
// constant 0x343FD) and input (DirectInput / key-state APIs), with callers.
// Args: <iat_map.csv (unused)> <out dir>
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ExportLoopRngInput extends GhidraScript {
    Map<Function, Set<String>> why = new LinkedHashMap<>();

    void tag(Function f, String t) {
        if (f != null) why.computeIfAbsent(f, k -> new TreeSet<>()).add(t);
    }

    void tagCallers(Function f, String t, int depth) throws Exception {
        if (depth == 0) return;
        for (Function c : f.getCallingFunctions(monitor)) {
            tag(c, t);
            tagCallers(c, t + "<", depth - 1);
        }
    }

    @Override
    public void run() throws Exception {
        Path outDir = Paths.get(getScriptArgs()[1]);
        Files.createDirectories(outDir);
        String[] apis = { "timeGetTime", "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount",
            "DirectInput8Create", "GetAsyncKeyState", "GetKeyState", "GetKeyboardState", "PeekMessageA", "PeekMessageW" };

        // API users: slot -> thunk (jmp [slot]) or direct call [slot].
        // Slots come from iat_map.csv: the game's own IAT (0x4B9xxx) has no labels.
        Map<String, List<Address>> slots = new HashMap<>();
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            line = line.replace("﻿", "").trim();
            if (line.isEmpty()) continue;
            String[] p = line.split(",", 2);
            String name = p[1].substring(p[1].indexOf('!') + 1);
            slots.computeIfAbsent(name, k -> new ArrayList<>()).add(toAddr(Long.decode(p[0])));
        }
        for (String api : apis) {
            for (Address slot : slots.getOrDefault(api, List.of())) {
                for (Reference r : getReferencesTo(slot)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) continue;
                    if (f.getBody().getNumAddresses() <= 8) {
                        for (Function c : f.getCallingFunctions(monitor)) { tag(c, api); tagCallers(c, api + "<", 1); }
                    } else { tag(f, api); tagCallers(f, api + "<", 1); }
                }
            }
        }

        // MSVC rand(): imul ..., 0x343FD
        Memory mem = currentProgram.getMemory();
        byte[] pat = { (byte) 0xFD, 0x43, 0x03, 0x00 };
        Address cur = currentProgram.getMinAddress();
        while (true) {
            Address hit = mem.findBytes(cur, pat, null, true, monitor);
            if (hit == null) break;
            cur = hit.add(1);
            Function f = getFunctionContaining(hit);
            if (f == null) continue;
            tag(f, "RNG@" + hit);
            for (Function c : f.getCallingFunctions(monitor)) tag(c, "calls RNG " + f.getEntryPoint());
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter idx = new PrintWriter(outDir.resolve("lri_index.txt").toFile(), "UTF-8");
             PrintWriter all = new PrintWriter(outDir.resolve("lri_decompiled.c").toFile(), "UTF-8")) {
            for (Map.Entry<Function, Set<String>> e : why.entrySet()) {
                Function f = e.getKey();
                idx.println(f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " callers=" + f.getCallingFunctions(monitor).size() + " " + e.getValue());
                if (f.getBody().getNumAddresses() > 20000) { all.println("// ==== " + f.getEntryPoint() + " (too big, skipped) " + e.getValue()); continue; }
                DecompileResults res = di.decompileFunction(f, 90, monitor);
                all.println("// ==== " + f.getEntryPoint() + " " + e.getValue());
                all.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
        println("lri functions exported: " + why.size());
    }
}
