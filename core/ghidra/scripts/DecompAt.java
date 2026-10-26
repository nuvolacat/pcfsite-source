import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;

public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    PrintWriter out = new PrintWriter(new File(args[0]));
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    for (int i = 1; i < args.length; i++) {
      Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
      Function f = getFunctionAt(a);
      if (f == null) {
        CreateFunctionCmd cmd = new CreateFunctionCmd(a);
        cmd.applyTo(currentProgram, monitor);
        f = getFunctionAt(a);
      }
      out.println("// ==== " + args[i] + " " + (f == null ? "SIN FUNCION" : f.getName()));
      if (f != null) {
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        out.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// fallo");
      }
    }
    out.close();
  }
}
