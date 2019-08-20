# Tracing
## Coverage collection

Coverage information can be recorded from inside the debugger with the coverage-start/coverage-stop command
(for full coverage) and bl-start/bl-stop (for call coverage).

## Full trace recording
cpurec-start and cpurec-stop command can be used to collect full execution state, containing the registers
and the executed instruction.

### Recording format
The recording format is simple:
```
[instruction: u32][reg bitmap: u16][regs: u32...]
```

The reg bitmap uses each bit to specify the presence of the reg in the following variable length array.

## Trace processing
The tsearch tool allows to process and exploit the generated traces. You can find it in ```tools/trace-search/```.
