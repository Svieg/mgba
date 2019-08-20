# tsearch: Trace processing tool
Using the cpurec commands mgba will produce full execution traces for thumb
mode (most games use mainly this mode). This tool can pretty print the binary
traces produced by the emulator but also search on the fly using filters.

## Usage
```
./tsearch <options>
options:
    -i: input file (required)
    -o: Output file (default stdout)
    -m: Disassembly mode (arm/thumb, default none)
    -f: Filter (ex: "op=ldrh,r1=0x41414141)
```

## Available filters
- Registers (\<reg\>=\<value\>):
    - r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, fp, ip, sp, lr, pc
- Opcode (op=\<op str\>)
- Execution index (idx=\<index\>)
