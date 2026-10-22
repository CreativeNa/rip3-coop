// Labels IAT slots from iat_map.csv, then decompiles every function that
// references a string of interest or calls a HawkNL import.
// @category rip3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ExportNetCode extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path csv = Paths.get(args[0]);
        Path outDir = Paths.get(args[1]);
        Files.createDirectories(outDir);
        SymbolTable st = currentProgram.getSymbolTable();
        Map<Address, String> iat = new HashMap<>();
        for (String line : Files.readAllLines(csv)) {
            line = line.replace("﻿", "").trim();
            if (line.isEmpty()) continue;
            String[] p = line.split(",", 2);
            Address a = toAddr(Long.decode(p[0]));
            String name = p[1].substring(p[1].indexOf('!') + 1);
            iat.put(a, p[1]);
            try {
                createDWord(a);
                st.createLabel(a, "PTR_" + name, SourceType.USER_DEFINED);
            } catch (Exception e) { }
        }

        String[] needles = { "COOPERATIVE MODE", "CREATE SERVER", "CLIENT", "SERVER", "New player connected",
            "Trying to connect", "Connection to server", "Server accepted", "No servers available",
            "Wait for server response", "Levels/m%d.rl2", "Levels/%d.rl2", "ServerName", "m_server_list" };
        Set<Function> funcs = new LinkedHashSet<>();
        Map<Function, Set<String>> why = new HashMap<>();
        Memory mem = currentProgram.getMemory();
        for (String n : needles) {
            byte[] pat = (n + "\0").getBytes("ASCII");
            Address cur = currentProgram.getMinAddress();
            while (true) {
                Address hit = mem.findBytes(cur, pat, null, true, monitor);
                if (hit == null) break;
                cur = hit.add(1);
                for (Reference r : getReferencesTo(hit)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) continue;
                    funcs.add(f);
                    why.computeIfAbsent(f, k -> new TreeSet<>()).add("str:" + n);
                }
            }
        }
        for (Map.Entry<Address, String> e : iat.entrySet()) {
            if (!e.getValue().startsWith("HawkNL")) continue;
            for (Reference r : getReferencesTo(e.getKey())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                funcs.add(f);
                why.computeIfAbsent(f, k -> new TreeSet<>()).add(e.getValue());
            }
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter idx = new PrintWriter(outDir.resolve("index.txt").toFile(), "UTF-8");
             PrintWriter all = new PrintWriter(outDir.resolve("decompiled.c").toFile(), "UTF-8")) {
            for (Function f : funcs) {
                idx.println(f.getEntryPoint() + " " + f.getName() + " size=" + f.getBody().getNumAddresses() + " " + why.get(f));
                DecompileResults res = di.decompileFunction(f, 60, monitor);
                all.println("// ==== " + f.getEntryPoint() + " " + why.get(f));
                all.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
        println("functions exported: " + funcs.size());
    }
}
