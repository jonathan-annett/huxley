#!/usr/bin/env node
/*
 * asm-filter.js — pass-through filter that appends 8086 disassembly
 *
 * Usage:
 *   your-monitor | node asm-filter.js
 *   node asm-filter.js < input.log > output.log
 *
 * For every line containing `opcodes=XX YY ZZ ...`, it decodes the FIRST
 * 8086 instruction starting at those bytes and appends ` ; <mnemonic>`.
 * Lines without an `opcodes=` field are passed through unchanged.
 *
 * The disassembler handles the full 8086 instruction set including:
 *   - segment override prefixes (ES:, CS:, SS:, DS:)
 *   - REP / REPNE / LOCK prefixes
 *   - all ModR/M addressing modes
 *   - 8/16-bit displacements and immediates
 *   - group opcodes (80-83, D0-D3, F6-F7, FE-FF)
 *
 * 8086 instruction format:
 *   [prefix(es)] [opcode] [ModR/M] [displacement] [immediate]
 *
 * ModR/M byte: mmrrrmmm where
 *   mm  (mod) = addressing mode
 *   rrr (reg) = register or opcode extension
 *   mmm (r/m) = register or memory operand
 */

'use strict';

const readline = require('readline');

// ------------------------------------------------------------------
// Register / operand tables
// ------------------------------------------------------------------

const REG8  = ['AL','CL','DL','BL','AH','CH','DH','BH'];
const REG16 = ['AX','CX','DX','BX','SP','BP','SI','DI'];
const SREG  = ['ES','CS','SS','DS'];

// r/m effective-address base (when mod != 11). Index 6 is special
// when mod=00 (it becomes disp16 instead of [BP]).
const RMBASE = ['BX+SI','BX+DI','BP+SI','BP+DI','SI','DI','BP','BX'];

// Condition codes for Jcc (70-7F) — suffix after 'J'
const COND = ['O','NO','B','NB','Z','NZ','BE','A','S','NS','P','NP','L','GE','LE','G'];

// Group 1 mnemonics (00-3D arithmetic, and 80-83 with immediate)
const GRP1 = ['ADD','OR','ADC','SBB','AND','SUB','XOR','CMP'];

// Shift/rotate group (D0-D3)
const SHIFT = ['ROL','ROR','RCL','RCR','SHL','SHR','???','SAR'];

// Group F6/F7
const GRPF6 = ['TEST','???','NOT','NEG','MUL','IMUL','DIV','IDIV'];

// Group FF
const GRPFF = ['INC','DEC','CALL','CALLF','JMP','JMPF','PUSH','???'];

// ------------------------------------------------------------------
// Formatting helpers
// ------------------------------------------------------------------

function hex(n, width) {
    return n.toString(16).toUpperCase().padStart(width, '0') + 'h';
}

// Format a signed displacement like "+1Ah" / "-02h"
function dispStr(v, width) {
    if (v === 0) return '';
    return v < 0
        ? '-' + (-v).toString(16).toUpperCase().padStart(width, '0') + 'h'
        : '+' +   v .toString(16).toUpperCase().padStart(width, '0') + 'h';
}

// Format a relative jump target — we don't know absolute address here,
// so show it as a signed offset.
function relStr(v, width) {
    return (v < 0 ? '-' : '+') +
           Math.abs(v).toString(16).toUpperCase().padStart(width, '0') + 'h';
}

// ------------------------------------------------------------------
// Byte cursor
// ------------------------------------------------------------------

class Cursor {
    constructor(bytes) { this.bytes = bytes; this.pos = 0; }
    avail()  { return this.pos < this.bytes.length; }
    b()      { if (this.pos >= this.bytes.length) throw new Error('EOF');
               return this.bytes[this.pos++]; }
    w()      { const lo = this.b(), hi = this.b(); return (hi << 8) | lo; }
    sb()     { const v = this.b(); return v >= 0x80 ? v - 0x100  : v; }
    sw()     { const v = this.w(); return v >= 0x8000 ? v - 0x10000 : v; }
}

// ------------------------------------------------------------------
// ModR/M decoding
//
// Returns { mod, reg, rm, rmStr, regStr }
// `size16` selects 16-bit vs 8-bit register naming
// `seg` is the active segment-override prefix (or '' for default)
// ------------------------------------------------------------------

function modrm(c, size16, seg) {
    const byte = c.b();
    const mod = (byte >> 6) & 3;
    const reg = (byte >> 3) & 7;
    const rm  =  byte       & 7;

    const regStr = size16 ? REG16[reg] : REG8[reg];
    let rmStr;

    if (mod === 3) {
        // r/m is a register
        rmStr = size16 ? REG16[rm] : REG8[rm];
    } else {
        const sizePfx = size16 ? 'word ' : 'byte ';
        const segPfx  = seg ? seg + ':' : '';
        let inner;

        if (mod === 0 && rm === 6) {
            // Special: direct 16-bit address
            inner = hex(c.w(), 4);
        } else {
            let d = 0;
            if (mod === 1) d = c.sb();
            if (mod === 2) d = c.sw();
            inner = RMBASE[rm] + dispStr(d, mod === 1 ? 2 : 4);
        }
        rmStr = sizePfx + segPfx + '[' + inner + ']';
    }
    return { mod, reg, rm, rmStr, regStr };
}

// ------------------------------------------------------------------
// Main decoder — returns mnemonic string for the first instruction
// in `bytes`, or '??' if undecodable / truncated.
// ------------------------------------------------------------------

function decode(bytes) {
    const c = new Cursor(bytes);
    let seg = '';
    let rep = '';
    let lock = '';

    // --- consume prefixes ----------------------------------------
    prefix_loop:
    while (c.avail()) {
        switch (c.bytes[c.pos]) {
            case 0x26: seg = 'ES'; c.pos++; continue;
            case 0x2E: seg = 'CS'; c.pos++; continue;
            case 0x36: seg = 'SS'; c.pos++; continue;
            case 0x3E: seg = 'DS'; c.pos++; continue;
            case 0xF0: lock = 'LOCK ';   c.pos++; continue;
            case 0xF2: rep  = 'REPNE '; c.pos++; continue;
            case 0xF3: rep  = 'REP ';   c.pos++; continue;
            default:   break prefix_loop;
        }
    }
    if (!c.avail()) return '??';

    const op = c.b();
    let s;

    try {
        s = decodeOp(op, c, seg, rep);
    } catch (e) {
        return '??';   // usually "EOF" — not enough bytes captured
    }
    return lock + s;
}

function decodeOp(op, c, seg, rep) {

    // ---- 00-3F: arithmetic/logic block + a few odd ones ---------
    // Pattern: opcodes xxxyyy00..101 where xxx selects ADD/OR/.../CMP
    // and yyy selects operand form:
    //   000 = r/m8, r8     001 = r/m16, r16
    //   010 = r8, r/m8     011 = r16, r/m16
    //   100 = AL, imm8     101 = AX, imm16
    // yyy = 110, 111 are PUSH/POP sreg and decimal-adjust instructions.
    if (op < 0x40 && (op & 6) !== 6) {
        const mnem   = GRP1[(op >> 3) & 7];
        const form   = op & 7;
        const size16 = form & 1;

        if (form < 4) {
            const m = modrm(c, size16, seg);
            return (form & 2)
                ? `${mnem} ${m.regStr}, ${m.rmStr}`
                : `${mnem} ${m.rmStr}, ${m.regStr}`;
        }
        // AL/AX immediate form
        const imm = size16 ? c.w() : c.b();
        return `${mnem} ${size16 ? 'AX' : 'AL'}, ${hex(imm, size16 ? 4 : 2)}`;
    }

    // PUSH/POP segment register & decimal-adjust
    switch (op) {
        case 0x06: return 'PUSH ES';
        case 0x07: return 'POP ES';
        case 0x0E: return 'PUSH CS';
        case 0x16: return 'PUSH SS';
        case 0x17: return 'POP SS';
        case 0x1E: return 'PUSH DS';
        case 0x1F: return 'POP DS';
        case 0x27: return 'DAA';
        case 0x2F: return 'DAS';
        case 0x37: return 'AAA';
        case 0x3F: return 'AAS';
    }

    // INC/DEC r16 (40-4F)
    if (op >= 0x40 && op <= 0x47) return `INC ${REG16[op & 7]}`;
    if (op >= 0x48 && op <= 0x4F) return `DEC ${REG16[op & 7]}`;

    // PUSH/POP r16 (50-5F)
    if (op >= 0x50 && op <= 0x57) return `PUSH ${REG16[op & 7]}`;
    if (op >= 0x58 && op <= 0x5F) return `POP ${REG16[op & 7]}`;

    // Jcc rel8 (70-7F)
    if (op >= 0x70 && op <= 0x7F)
        return `J${COND[op & 0xF]} ${relStr(c.sb(), 2)}`;

    // Group 1 immediate (80-83)
    if (op >= 0x80 && op <= 0x83) {
        const size16   = op & 1;
        const signExt  = (op & 2) !== 0;   // 83 sign-extends an 8-bit imm to 16
        const m = modrm(c, size16, seg);
        const mnem = GRP1[m.reg];
        let imm;
        let width;
        if (!size16)      { imm = c.b();  width = 2; }
        else if (signExt) { imm = c.sb(); width = 4; }
        else              { imm = c.w();  width = 4; }
        const immStr = imm < 0
            ? '-' + (-imm).toString(16).toUpperCase().padStart(width, '0') + 'h'
            : hex(imm & (size16 ? 0xFFFF : 0xFF), width);
        return `${mnem} ${m.rmStr}, ${immStr}`;
    }

    // TEST / XCHG r/m, r (84-87)
    if (op === 0x84 || op === 0x85) {
        const m = modrm(c, op & 1, seg);
        return `TEST ${m.rmStr}, ${m.regStr}`;
    }
    if (op === 0x86 || op === 0x87) {
        const m = modrm(c, op & 1, seg);
        return `XCHG ${m.rmStr}, ${m.regStr}`;
    }

    // MOV r/m, r and r, r/m  (88-8B)
    if (op >= 0x88 && op <= 0x8B) {
        const m = modrm(c, op & 1, seg);
        return (op & 2)
            ? `MOV ${m.regStr}, ${m.rmStr}`
            : `MOV ${m.rmStr}, ${m.regStr}`;
    }

    // MOV r/m, sreg (8C) and MOV sreg, r/m (8E)
    if (op === 0x8C || op === 0x8E) {
        const m = modrm(c, 1, seg);
        const sr = SREG[m.reg & 3];
        return op === 0x8C
            ? `MOV ${m.rmStr}, ${sr}`
            : `MOV ${sr}, ${m.rmStr}`;
    }

    // LEA (8D) — address computation only, no size prefix on rm
    if (op === 0x8D) {
        const m = modrm(c, 1, seg);
        return `LEA ${m.regStr}, ${m.rmStr.replace(/^word /, '')}`;
    }

    // POP r/m16 (8F)
    if (op === 0x8F) {
        const m = modrm(c, 1, seg);
        return `POP ${m.rmStr}`;
    }

    // XCHG AX, r16 / NOP (90-97)
    if (op === 0x90) return 'NOP';
    if (op >= 0x91 && op <= 0x97) return `XCHG AX, ${REG16[op & 7]}`;

    switch (op) {
        case 0x98: return 'CBW';
        case 0x99: return 'CWD';
        case 0x9A: {
            const off = c.w(), sg = c.w();
            return `CALL FAR ${hex(sg, 4)}:${hex(off, 4)}`;
        }
        case 0x9B: return 'WAIT';
        case 0x9C: return 'PUSHF';
        case 0x9D: return 'POPF';
        case 0x9E: return 'SAHF';
        case 0x9F: return 'LAHF';
    }

    // MOV accumulator <-> memory (A0-A3) — direct address
    if (op === 0xA0) return `MOV AL, [${hex(c.w(), 4)}]`;
    if (op === 0xA1) return `MOV AX, [${hex(c.w(), 4)}]`;
    if (op === 0xA2) return `MOV [${hex(c.w(), 4)}], AL`;
    if (op === 0xA3) return `MOV [${hex(c.w(), 4)}], AX`;

    // String ops (A4-A7, AA-AF)
    const strOps = {
        0xA4:'MOVSB', 0xA5:'MOVSW', 0xA6:'CMPSB', 0xA7:'CMPSW',
        0xAA:'STOSB', 0xAB:'STOSW', 0xAC:'LODSB', 0xAD:'LODSW',
        0xAE:'SCASB', 0xAF:'SCASW'
    };
    if (op in strOps) return rep + strOps[op];

    // TEST accumulator, imm
    if (op === 0xA8) return `TEST AL, ${hex(c.b(), 2)}`;
    if (op === 0xA9) return `TEST AX, ${hex(c.w(), 4)}`;

    // MOV r8/r16, imm (B0-BF)
    if (op >= 0xB0 && op <= 0xB7) return `MOV ${REG8[op & 7]}, ${hex(c.b(), 2)}`;
    if (op >= 0xB8 && op <= 0xBF) return `MOV ${REG16[op & 7]}, ${hex(c.w(), 4)}`;

    // RET near
    if (op === 0xC2) return `RET ${hex(c.w(), 4)}`;
    if (op === 0xC3) return 'RET';

    // LES / LDS
    if (op === 0xC4) { const m = modrm(c, 1, seg); return `LES ${m.regStr}, ${m.rmStr}`; }
    if (op === 0xC5) { const m = modrm(c, 1, seg); return `LDS ${m.regStr}, ${m.rmStr}`; }

    // MOV r/m, imm
    if (op === 0xC6) { const m = modrm(c, 0, seg); return `MOV ${m.rmStr}, ${hex(c.b(), 2)}`; }
    if (op === 0xC7) { const m = modrm(c, 1, seg); return `MOV ${m.rmStr}, ${hex(c.w(), 4)}`; }

    // RET far
    if (op === 0xCA) return `RETF ${hex(c.w(), 4)}`;
    if (op === 0xCB) return 'RETF';

    if (op === 0xCC) return 'INT 3';
    if (op === 0xCD) return `INT ${hex(c.b(), 2)}`;
    if (op === 0xCE) return 'INTO';
    if (op === 0xCF) return 'IRET';

    // Shift/rotate group (D0-D3)
    if (op >= 0xD0 && op <= 0xD3) {
        const size16 = op & 1;
        const useCL  = (op & 2) !== 0;
        const m = modrm(c, size16, seg);
        return `${SHIFT[m.reg]} ${m.rmStr}, ${useCL ? 'CL' : '1'}`;
    }

    if (op === 0xD4) { const v = c.b(); return v === 0x0A ? 'AAM' : `AAM ${hex(v,2)}`; }
    if (op === 0xD5) { const v = c.b(); return v === 0x0A ? 'AAD' : `AAD ${hex(v,2)}`; }
    if (op === 0xD7) return 'XLAT';

    // ESC — reserved for coprocessor, just emit a placeholder
    if (op >= 0xD8 && op <= 0xDF) {
        const m = modrm(c, 1, seg);
        return `ESC ${op & 7}, ${m.rmStr}`;
    }

    // LOOP family + JCXZ
    if (op === 0xE0) return `LOOPNE ${relStr(c.sb(), 2)}`;
    if (op === 0xE1) return `LOOPE ${relStr(c.sb(), 2)}`;
    if (op === 0xE2) return `LOOP ${relStr(c.sb(), 2)}`;
    if (op === 0xE3) return `JCXZ ${relStr(c.sb(), 2)}`;

    // IN/OUT imm8
    if (op === 0xE4) return `IN AL, ${hex(c.b(), 2)}`;
    if (op === 0xE5) return `IN AX, ${hex(c.b(), 2)}`;
    if (op === 0xE6) return `OUT ${hex(c.b(), 2)}, AL`;
    if (op === 0xE7) return `OUT ${hex(c.b(), 2)}, AX`;

    // Direct jumps / calls
    if (op === 0xE8) return `CALL ${relStr(c.sw(), 4)}`;
    if (op === 0xE9) return `JMP ${relStr(c.sw(), 4)}`;
    if (op === 0xEA) {
        const off = c.w(), sg = c.w();
        return `JMP FAR ${hex(sg, 4)}:${hex(off, 4)}`;
    }
    if (op === 0xEB) return `JMP SHORT ${relStr(c.sb(), 2)}`;

    // IN/OUT DX
    if (op === 0xEC) return 'IN AL, DX';
    if (op === 0xED) return 'IN AX, DX';
    if (op === 0xEE) return 'OUT DX, AL';
    if (op === 0xEF) return 'OUT DX, AX';

    if (op === 0xF4) return 'HLT';
    if (op === 0xF5) return 'CMC';

    // Group F6/F7
    if (op === 0xF6 || op === 0xF7) {
        const size16 = op & 1;
        const m = modrm(c, size16, seg);
        if (m.reg === 0 || m.reg === 1) {     // TEST r/m, imm
            const imm = size16 ? c.w() : c.b();
            return `TEST ${m.rmStr}, ${hex(imm, size16 ? 4 : 2)}`;
        }
        return `${GRPF6[m.reg]} ${m.rmStr}`;
    }

    // Flag manipulation
    if (op === 0xF8) return 'CLC';
    if (op === 0xF9) return 'STC';
    if (op === 0xFA) return 'CLI';
    if (op === 0xFB) return 'STI';
    if (op === 0xFC) return 'CLD';
    if (op === 0xFD) return 'STD';

    // INC/DEC r/m8 (FE)
    if (op === 0xFE) {
        const m = modrm(c, 0, seg);
        if (m.reg < 2) return `${['INC','DEC'][m.reg]} ${m.rmStr}`;
        return `??`;
    }

    // Group FF: INC/DEC/CALL/JMP/PUSH r/m16
    if (op === 0xFF) {
        const m = modrm(c, 1, seg);
        return `${GRPFF[m.reg]} ${m.rmStr}`;
    }

    return `DB ${hex(op, 2)}`;
}

// ------------------------------------------------------------------
// Parse "89 C3 8E C2 ..." into a Uint8Array
// ------------------------------------------------------------------

function parseHexBytes(s) {
    const out = [];
    for (const tok of s.trim().split(/\s+/)) {
        if (!/^[0-9A-Fa-f]{1,2}$/.test(tok)) return null;
        out.push(parseInt(tok, 16));
    }
    return out;
}

// ------------------------------------------------------------------
// Line processor: locate opcodes=..., disassemble, append.
// The regex captures the bytes up to the end of the line (or a
// trailing separator), which matches the monitor's output format.
// ------------------------------------------------------------------

const OPCODES_RE = /\bopcodes=([0-9A-Fa-f]{1,2}(?:\s+[0-9A-Fa-f]{1,2})*)/;

function processLine(line) {
    const m = line.match(OPCODES_RE);
    if (!m) return line;

    const bytes = parseHexBytes(m[1]);
    if (!bytes || bytes.length === 0) return line;

    const asm = decode(bytes);
    return `${line} ; ${asm}`;
}

// ------------------------------------------------------------------
// Self-test (run with `node asm-filter.js --test`)
// ------------------------------------------------------------------

function selfTest() {
    const cases = [
        ['89 C3',                   'MOV BX, AX'],
        ['8E C2',                   'MOV ES, DX'],
        ['89 C6',                   'MOV SI, AX'],
        ['56',                      'PUSH SI'],
        ['57',                      'PUSH DI'],
        ['55',                      'PUSH BP'],
        ['89 E5',                   'MOV BP, SP'],
        ['83 EC 18',                'SUB SP, 0018h'],
        ['03 46 FE',                'ADD AX, word [BP-02h]'],
        ['8B 56 E6',                'MOV DX, word [BP-1Ah]'],
        ['E8 96 07',                'CALL +0796h'],
        ['07',                      'POP ES'],
        ['5E',                      'POP SI'],
        ['5A',                      'POP DX'],
        ['59',                      'POP CX'],
        ['5B',                      'POP BX'],
        ['C3',                      'RET'],
        ['53',                      'PUSH BX'],
        ['52',                      'PUSH DX'],
        ['08 E4',                   'OR AH, AH'],
        ['74 05',                   'JZ +05h'],
        ['80 FC 0C',                'CMP AH, 0Ch'],
        ['81 7E FE 00 02',          'CMP word [BP-02h], 0200h'],
        ['74 1B',                   'JZ +1Bh'],
        ['A3 00 0D',                'MOV [0D00h], AX'],
        ['89 16 02 0D',             'MOV word [0D02h], DX'],
        ['26 89 07',                'MOV word ES:[BX], AX'],
        ['F3 A4',                   'REP MOVSB'],
        ['CD 21',                   'INT 21h'],
    ];
    let pass = 0, fail = 0;
    for (const [hexstr, expected] of cases) {
        const bytes = parseHexBytes(hexstr);
        const got = decode(bytes);
        const ok = got === expected;
        console.log(`${ok ? 'PASS' : 'FAIL'}  ${hexstr.padEnd(20)}  => ${got}${ok ? '' : `   (expected: ${expected})`}`);
        ok ? pass++ : fail++;
    }
    console.log(`\n${pass} passed, ${fail} failed`);
    process.exit(fail ? 1 : 0);
}

// ------------------------------------------------------------------
// Main: readline pass-through
// ------------------------------------------------------------------

if (process.argv.includes('--test')) {
    selfTest();
} else {
    const rl = readline.createInterface({ input: process.stdin, terminal: false });
    rl.on('line', (line) => process.stdout.write(processLine(line) + '\n'));
}
