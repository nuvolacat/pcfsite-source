import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.util.*;

/** Crea funciones en el código que el análisis automático dejó sin definir:
 *  1) destinos de punteros desde datos (vtables) que caen en .text sin instrucción;
 *  2) prólogos típicos de MSVC6 en los huecos (SEH `6A FF 68 xx xx xx xx 64 A1 00 00 00 00`, `55 8B EC`).
 *  Itera hasta que no haya cambios. */
public class FixGaps extends GhidraScript {
  MemoryBlock text;
  boolean isUndefined(Address a) {
    return text.contains(a) && currentProgram.getListing().getInstructionAt(a) == null
        && currentProgram.getListing().getDefinedDataAt(a) == null && currentProgram.getListing().getInstructionContaining(a) == null;
  }
  boolean makeFunction(Address a) {
    if (!isUndefined(a)) return false;
    DisassembleCommand dc = new DisassembleCommand(a, null, true);
    if (!dc.applyTo(currentProgram, monitor)) return false;
    if (currentProgram.getListing().getInstructionAt(a) == null) return false;
    CreateFunctionCmd cmd = new CreateFunctionCmd(a);
    return cmd.applyTo(currentProgram, monitor);
  }
  public void run() throws Exception {
    text = currentProgram.getMemory().getBlock(".text");
    Listing listing = currentProgram.getListing();
    ReferenceManager rm = currentProgram.getReferenceManager();
    int round = 0, total = 0;
    while (true) {
      int created = 0;
      // 1) punteros desde datos hacia .text
      for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
        if (!b.isInitialized() || b == text) continue;
        Address p = b.getStart();
        while (p.compareTo(b.getEnd().subtract(3)) < 0 && !monitor.isCancelled()) {
          try {
            long v = currentProgram.getMemory().getInt(p) & 0xffffffffL;
            Address t = toAddr(v);
            if (text.contains(t) && isUndefined(t)) {
              // exigir prólogo plausible para no crear basura
              byte b0 = currentProgram.getMemory().getByte(t);
              int u = b0 & 0xff;
              if (u==0x55||u==0x53||u==0x56||u==0x57||u==0x6A||u==0x8B||u==0x83||u==0x81||u==0xB8||u==0x33||u==0xA1||u==0xE9||u==0xC2||u==0xC3||u==0xD9||u==0x8A||u==0x0F||u==0x68||u==0xE8||u==0x64) {
                if (makeFunction(t)) created++;
              }
            }
          } catch (Exception e) {}
          p = p.add(4);
        }
      }
      // 2) prólogos en huecos de .text
      byte[] seh = {(byte)0x6A,(byte)0xFF,(byte)0x68}; 
      Address p = text.getStart();
      while (p.compareTo(text.getEnd()) < 0 && !monitor.isCancelled()) {
        Instruction ins = listing.getInstructionContaining(p);
        if (ins != null) { p = ins.getMaxAddress().add(1); continue; }
        Data d = listing.getDefinedDataContaining(p);
        if (d != null) { p = d.getMaxAddress().add(1); continue; }
        try {
          byte[] w = new byte[13]; currentProgram.getMemory().getBytes(p, w);
          boolean isSeh = w[0]==seh[0] && w[1]==seh[1] && w[2]==seh[2] && w[7]==(byte)0x64 && w[8]==(byte)0xA1 && w[9]==0 && w[10]==0 && w[11]==0 && w[12]==0;
          boolean isEbp = w[0]==(byte)0x55 && w[1]==(byte)0x8B && w[2]==(byte)0xEC;
          if ((isSeh || isEbp) && makeFunction(p)) { created++; continue; }
        } catch (Exception e) {}
        p = p.add(1);
      }
      total += created; round++;
      println("FixGaps ronda " + round + ": " + created + " funciones nuevas");
      if (created == 0 || round > 6) break;
    }
    println("FixGaps TOTAL " + total);
  }
}
