import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;

/** Exporta TODO el programa: functions.tsv, strings.tsv, imports.tsv, callgraph.json y decomp_NNN.c
 *  Uso headless: -postScript ExportAll.java <dirSalida>   (crea <dirSalida>/<nombrePrograma>/) */
public class ExportAll extends GhidraScript {
  static String esc(String s){ return s.replace("\\","\\\\").replace("\"","\\\"").replace("\t","\\t").replace("\n","\\n").replace("\r","\\r"); }
  public void run() throws Exception {
    File out = new File(getScriptArgs()[0], currentProgram.getName().replaceAll("[^A-Za-z0-9_.-]","_"));
    out.mkdirs();
    Listing listing = currentProgram.getListing();
    ReferenceManager rm = currentProgram.getReferenceManager();
    // ---- funciones
    PrintWriter fl = new PrintWriter(new File(out, "functions.tsv"));
    fl.println("entry\tsize\tname\tcc\tsignature\tnrefs");
    FunctionIterator it = listing.getFunctions(true);
    List<Function> funcs = new ArrayList<>();
    while (it.hasNext()) { Function f = it.next(); funcs.add(f);
      int nrefs = rm.getReferenceCountTo(f.getEntryPoint());
      fl.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t" + f.getCallingConventionName() + "\t" + esc(f.getPrototypeString(false,false)) + "\t" + nrefs); }
    fl.close();
    // ---- imports (símbolos externos) con quién los llama
    PrintWriter il = new PrintWriter(new File(out, "imports.tsv"));
    il.println("lib\tsymbol\tdemangled\taddr\tcallers");
    SymbolTable st = currentProgram.getSymbolTable();
    SymbolIterator ext = st.getExternalSymbols();
    while (ext.hasNext()) { Symbol s = ext.next();
      String lib = s.getParentNamespace()==null?"":s.getParentNamespace().getName();
      Set<String> callers = new TreeSet<>();
      for (Reference r : rm.getReferencesTo(s.getAddress())) {
        Function f = getFunctionContaining(r.getFromAddress());
        if (f != null) callers.add(f.getEntryPoint().toString());
        // referencias a través del thunk/IAT
        Data d = listing.getDataAt(r.getFromAddress());
        if (d != null) for (Reference r2 : rm.getReferencesTo(r.getFromAddress())) { Function f2 = getFunctionContaining(r2.getFromAddress()); if (f2!=null) callers.add(f2.getEntryPoint().toString()); }
      }
      Object o = s.getObject(); String dem = "";
      if (o instanceof Function) dem = ((Function)o).getName();
      il.println(lib + "\t" + s.getName() + "\t" + esc(dem) + "\t" + s.getAddress() + "\t" + String.join(",", callers));
    }
    il.close();
    // ---- strings con referencias
    PrintWriter sl = new PrintWriter(new File(out, "strings.tsv"));
    sl.println("addr\tvalue\trefs(from@func)");
    DataIterator di = listing.getDefinedData(true);
    while (di.hasNext()) { Data dd = di.next();
      if (dd.hasStringValue()) {
        StringBuilder refs = new StringBuilder();
        for (Reference r : rm.getReferencesTo(dd.getAddress())) {
          Function f = getFunctionContaining(r.getFromAddress());
          refs.append(r.getFromAddress()).append("@").append(f==null?"?":f.getEntryPoint()).append(",");
        }
        sl.println(dd.getAddress() + "\t" + esc(String.valueOf(dd.getValue())) + "\t" + refs);
      }
    }
    sl.close();
    // ---- callgraph
    PrintWriter cg = new PrintWriter(new File(out, "callgraph.json"));
    cg.println("{");
    boolean first = true;
    for (Function f : funcs) {
      Set<String> callees = new TreeSet<>();
      for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getEntryPoint().toString() + (c.isExternal()||c.isThunk()?":"+c.getName():""));
      cg.println((first?"":",") + "\"" + f.getEntryPoint() + "\":[" + (callees.isEmpty()?"":"\"" + String.join("\",\"", callees) + "\"") + "]");
      first = false;
    }
    cg.println("}"); cg.close();
    // ---- descompilado
    DecompInterface di2 = new DecompInterface();
    di2.openProgram(currentProgram);
    PrintWriter cw = null; int n = 0, chunk = 0;
    for (Function f : funcs) {
      if (monitor.isCancelled()) break;
      if (n % 500 == 0) { if (cw != null) cw.close(); cw = new PrintWriter(new File(out, String.format("decomp_%03d.c", chunk++))); }
      DecompileResults res = di2.decompileFunction(f, 60, monitor);
      cw.println("// ==== " + f.getEntryPoint() + " " + f.getName());
      if (res != null && res.decompileCompleted()) cw.println(res.getDecompiledFunction().getC());
      else cw.println("// decompile failed");
      n++;
      if (n % 1000 == 0) println("decompiled " + n + "/" + funcs.size());
    }
    if (cw != null) cw.close();
    di2.dispose();
    println("DONE functions=" + funcs.size());
  }
}
