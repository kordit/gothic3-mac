// Create functions at the addresses listed in entries/<program>.txt
// (one hex address per line) before ExportProgram runs: code Ghidra's own analysis
// left outside every function, such as MSVC's cold chunks, so the listing carries it.
// @category G3Recomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import java.nio.file.*;
import java.util.List;

public class AddEntries extends GhidraScript {
    public void run() throws Exception {
        Path list = Path.of(getScriptArgs()[0], currentProgram.getName() + ".txt");
        if (!Files.isRegularFile(list)) {
            println("no " + list);
            return;
        }
        List<String> lines = Files.readAllLines(list);
        int made = 0;
        for (String line : lines) {
            line = line.replaceAll("#.*", "").trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(Long.parseLong(line.replaceFirst("^0x", ""), 16));
            if (getInstructionAt(a) == null) {
                DisassembleCommand d = new DisassembleCommand(a, null, true);
                d.applyTo(currentProgram, monitor);
            }
            Function f = getFunctionAt(a);
            if (f == null) {
                CreateFunctionCmd cmd = new CreateFunctionCmd(a);
                if (!cmd.applyTo(currentProgram, monitor)) {
                    println("could not create a function at " + a + ": " + cmd.getStatusMsg());
                    continue;
                }
            }
            made++;
        }
        analyzeChanges(currentProgram);
        println("AddEntries: " + made + " functions at listed addresses");
    }
}
