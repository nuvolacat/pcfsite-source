import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

/** Exportación rápida (sin descompilar): solo functions.tsv, lo que necesita el traductor.
 *  Uso headless: -postScript ExportFuncs.java <dirSalida>   (crea <dirSalida>/<nombrePrograma>/) */
public class ExportFuncs extends GhidraScript {
  static String esc(String s){ return s.replace("\\","\\\\").replace("\t","\\t").replace("\n","\\n").replace("\r","\\r"); }
  public void run() throws Exception {
    File out = new File(getScriptArgs()[0], currentProgram.getName().replaceAll("[^A-Za-z0-9_.-]","_"));
    out.mkdirs();
    ReferenceManager rm = currentProgram.getReferenceManager();
    PrintWriter fl = new PrintWriter(new File(out, "functions.tsv"));
    fl.println("entry\tsize\tname\tcc\tsignature\tnrefs");
    FunctionIterator it = currentProgram.getListing().getFunctions(true);
    while (it.hasNext()) { Function f = it.next();
      fl.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t" + f.getCallingConventionName() + "\t" + esc(f.getPrototypeString(false,false)) + "\t" + rm.getReferenceCountTo(f.getEntryPoint())); }
    fl.close();
  }
}
