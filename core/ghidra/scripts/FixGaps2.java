import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.util.*;

/** Pasada 3: (a) código ya desensamblado pero sin función → crear función al inicio de cada racha;
 *  (b) en huecos sin nada, posiciones alineadas a 16 precedidas de relleno (90/CC) con opcode plausible → desensamblar + función. */
public class FixGaps2 extends GhidraScript {
  public void run() throws Exception {
    MemoryBlock text = currentProgram.getMemory().getBlock(".text");
    Listing listing = currentProgram.getListing();
    Memory mem = currentProgram.getMemory();
    int total = 0;
    for (int round = 0; round < 5; round++) {
      int created = 0;
      // (a) rachas de instrucciones sin función
      Address p = text.getStart();
      while (p.compareTo(text.getEnd()) < 0 && !monitor.isCancelled()) {
        Instruction ins = listing.getInstructionAt(p);
        if (ins == null) { Data d = listing.getDefinedDataContaining(p); p = (d != null) ? d.getMaxAddress().add(1) : p.add(1); continue; }
        if (listing.getFunctionContaining(p) == null) {
          Address start = p;
          // avanzar hasta el final de la racha
          Address q = p;
          while (true) { Instruction i2 = listing.getInstructionAt(q); if (i2 == null || listing.getFunctionContaining(q) != null) break; q = i2.getMaxAddress().add(1); }
          CreateFunctionCmd cmd = new CreateFunctionCmd(start);
          if (cmd.applyTo(currentProgram, monitor)) created++;
          p = q; continue;
        }
        p = ins.getMaxAddress().add(1);
      }
      // (b) huecos vacíos: entradas alineadas tras relleno
      p = text.getStart();
      while (p.compareTo(text.getEnd()) < 0 && !monitor.isCancelled()) {
        Instruction ins = listing.getInstructionContaining(p);
        if (ins != null) { p = ins.getMaxAddress().add(1); continue; }
        Data d = listing.getDefinedDataContaining(p);
        if (d != null) { p = d.getMaxAddress().add(1); continue; }
        try {
          long off = p.getOffset();
          int b0 = mem.getByte(p) & 0xff;
          int prev = mem.getByte(p.subtract(1)) & 0xff;
          boolean aligned = (off & 0xf) == 0 && (prev == 0x90 || prev == 0xcc || prev == 0xc3 || prev == 0xc2);
          boolean plausible = b0==0x55||b0==0x53||b0==0x56||b0==0x57||b0==0x6A||b0==0x8B||b0==0x83||b0==0x81||b0==0xB8||b0==0x33||b0==0xA1||b0==0xE9||b0==0xD9||b0==0x8A||b0==0x0F||b0==0x68||b0==0xE8||b0==0x64||b0==0xC7||b0==0x66||b0==0x80||b0==0x39||b0==0x3B||b0==0xF6;
          if (aligned && plausible && b0 != 0x90 && b0 != 0xcc) {
            DisassembleCommand dc = new DisassembleCommand(p, null, true);
            if (dc.applyTo(currentProgram, monitor) && listing.getInstructionAt(p) != null) {
              CreateFunctionCmd cmd = new CreateFunctionCmd(p);
              if (cmd.applyTo(currentProgram, monitor)) { created++; Function f = listing.getFunctionAt(p); if (f != null) { p = f.getBody().getMaxAddress().add(1); continue; } }
            }
          }
        } catch (Exception e) {}
        p = p.add(1);
      }
      println("FixGaps2 ronda " + round + ": " + created);
      total += created;
      if (created == 0) break;
    }
    println("FixGaps2 TOTAL " + total);
  }
}
