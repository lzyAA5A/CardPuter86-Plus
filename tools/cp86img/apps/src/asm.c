/* ============================================================================
 * ASM.C  --  MASM-lite 16-bit 8086 assembler, emits .COM files
 *
 *   ASM source.asm              -> source.COM (same directory)
 *   ASM source.asm out.com      -> out.com
 *
 * Target: M5Stack Cardputer / CardPuter86 8086 (NEC V20) DOS emulator.
 * Toolchain: Digital Mars C/C++ 8.42n, DOS 16-bit small model:
 *      dmc -msd -w asm.c -oASM.EXE
 * The source is plain C89 (no //, no declarations after statements) and also
 * builds natively (dmc -mn or gcc) so that the *exact same* assembler logic
 * can be regression tested on the host against keystone-engine -- see
 * test/xcheck.py.
 *
 * ---------------------------------------------------------------------------
 * DESIGN
 * ---------------------------------------------------------------------------
 * Assembly is done in passes:
 *
 *   pass 1..5   "sizing" passes.  The source file is re-read from the top on
 *               every pass.  Each pass walks the file, defines every symbol
 *               and advances the program counter, but writes no bytes.
 *               The encoded length of a line is a pure function of the
 *               *syntax* of that line (register/memory operand shape, explicit
 *               BYTE/WORD PTR hints) plus, for the 0x83 "sign extended imm8"
 *               optimisation, of the immediate value.  To keep line sizes
 *               identical in every pass that optimisation is only used for
 *               plain numeric immediates; a symbolic immediate always takes
 *               the full imm16 form.  Sizes are therefore stable from pass 1
 *               and only *symbol values* have to converge (forward references,
 *               `$`, EQU chains).  Passes repeat until the whole symbol table
 *               equals the previous pass (max 5, otherwise "phase error").
 *
 *   final pass  "emit" pass.  All symbols are known, so an undefined symbol is
 *               now a hard error.  Bytes go to a temporary file in the
 *               destination directory which is renamed to the .COM name only
 *               after the whole source assembled cleanly, so a failing
 *               assembly never leaves a half written .COM behind.  Streaming
 *               to a file instead of buffering a 64 KB image in RAM is what
 *               makes a 65024 byte output possible on a machine with ~62 KB
 *               of free heap.
 *
 * Output image: bytes are emitted back to back starting at *image offset 0*.
 * ORG only sets the *address* of the first image byte (i.e. label values and
 * `$`), so
 *
 *              org 100h            ; `start` == 0x100
 *      start:  mov ax, 4C00h       ; image byte 0 = B8, image byte 1 = 00
 *
 * which is exactly what a .COM file needs.  If ORG is absent the default is
 * 100h (documented in ASM.md).
 *
 * ---------------------------------------------------------------------------
 * ENCODING TABLE STRUCTURE
 * ---------------------------------------------------------------------------
 * The 8086 integer set is described by a few small tables plus one C block per
 * encoding *shape* in do_inst().  The tables hold only opcode bases and ModRM
 * /reg extension numbers -- no byte-level table.
 *
 * 1. Register file.  regnames[16] is the canonical 8086 order: index 0..7 are
 *    the 8-bit registers in opcode order (AL,CL,DL,BL,AH,CH,DH,BH), index
 *    8..15 the 16-bit ones in opcode order (AX,CX,DX,BX,SP,BP,SI,DI).  Both
 *    halves share the same 0..7 slot number, which is why one `reg` field plus
 *    one `rsize` field (1 or 2 bytes) fully describes a register operand, and
 *    why `opcode + reg` and the ModRM reg/rm fields can both be built from the
 *    same index.
 *      segnames[]/segpfx[] : ES CS SS DS -> code 0..3 (as used by MOV 8C/8E)
 *      plus the segment override prefix bytes 26/2E/36/3E.
 *
 * 2. Operand descriptor (OP).  Every operand is reduced to
 *      kind    OK_REG | OK_SEG | OK_IMM | OK_MEM
 *      reg     0..7 slot
 *      rsize   1|2 (for OK_MEM: explicit BYTE/WORD PTR hint, 0 = unspecified)
 *      segovr  -1 or the segment override prefix byte written on the operand
 *      mod/rm/dispbytes : the ModRM form, decided at parse time
 *              mod=00 no displacement, mod=01 disp8, mod=10 disp16,
 *              direct=1 -> [disp16] absolute, encoded as mod=00 rm=110
 *      val     immediate value, or the memory displacement
 *      symref  1 if the value referenced a symbol or `$` (forces disp16 /
 *              imm16 so that a line's size never depends on the pass)
 *      unknown 1 if a symbol was not defined *yet* in this pass
 *    The 8086 has exactly 8 memory addressing forms:
 *              rm 0 [BX+SI]  1 [BX+DI]  2 [BP+SI]  3 [BP+DI]
 *              rm 4 [SI]     5 [DI]     6 [BP]     7 [BX]
 *    (rm 6 with mod 00 is the disp16 absolute form, so a bare [BP] must be
 *    encoded as mod=01 disp8=0 -- which parse_mem() takes care of).
 *    parse_mem() therefore only has to notice which of BX/BP and SI/DI were
 *    written (in any order) and how wide the displacement is.
 *
 * 3. Instruction families.
 *      alu[]    ADD OR ADC SBB AND SUB XOR CMP -- one opcode base each
 *               (00/08/10/18/20/28/30/38) and one /reg extension 0..7 for the
 *               80/81/83 immediate forms.  From the base:
 *                  +0/+1 r/m <- r     +2/+3 r <- r/m     +4/+5 AL/AX <- imm
 *      shifts[] ROL ROR RCL RCR SHL SAL SHR SAR -> ModRM /reg ext 0..5 and 7
 *      jcc[]    JO..JG, condition code 0..15 -> opcode 0x70+cc (always short)
 *      fixed[]  the zero operand opcodes (flags, string ops, misc)
 *    Everything else (MOV/TEST/XCHG/INC/DEC/MUL../PUSH/POP/LEA/LDS/LES/JMP/
 *    CALL/IN/OUT/RET/INT) is a switch on operand shapes in do_inst().
 *
 * 4. Emission.  Encoders build the instruction in ob[] (max 15 bytes, actual
 *    max used here is 5) and then flush_ob() either only advances the program
 *    counter (sizing pass) or also writes the bytes (emit pass).  That single
 *    choke point is also where the 65024 byte .COM limit is enforced.  Prefix
 *    order is fixed by begin_inst(): REP/REPNE first, then a segment override.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#define MAXSYM   400            /* symbol table entries                       */
#define NAMELEN  20             /* significant characters per symbol name     */
#define LINESZ   256            /* source line buffer (incl. NUL)             */
#define MAXOUT   65024L         /* .COM image limit                           */
#define DEFORG   0x100L         /* ORG assumed when the source has none       */

/* ------------------------------------------------------------------- types */

typedef struct {
    char name[NAMELEN];
    long val;
    int  def;                   /* 1 = defined                                */
} SYM;

#define OK_NONE 0
#define OK_REG  1               /* general purpose register                   */
#define OK_SEG  2               /* segment register                           */
#define OK_IMM  3               /* immediate / relative target                */
#define OK_MEM  4               /* [ ... ]                                    */

typedef struct {
    int  kind;
    int  reg;                   /* 0..7 slot (OK_REG / OK_SEG)                */
    int  rsize;                 /* 1|2 register size, or BYTE/WORD PTR hint   */
    int  segovr;                /* -1, or segment override prefix byte        */
    int  mod, rm, dispbytes;    /* ModRM form for OK_MEM                      */
    int  direct;                /* [disp16] absolute (mod=00 rm=110)          */
    int  usess;                 /* address is BP based => default segment SS  */
    unsigned long val;          /* immediate value / displacement             */
    int  symref;                /* value came from a symbol or `$`            */
    int  unknown;               /* referenced an undefined symbol this pass   */
} OP;

static struct { char *name; int base; int ext; } alu_tab[] = {
    { "ADD", 0x00, 0 }, { "OR",  0x08, 1 }, { "ADC", 0x10, 2 }, { "SBB", 0x18, 3 },
    { "AND", 0x20, 4 }, { "SUB", 0x28, 5 }, { "XOR", 0x30, 6 }, { "CMP", 0x38, 7 },
    { 0, 0, 0 }
};

static struct { char *name; int cc; } jcc_tab[] = {
    { "JO",  0x0 }, { "JNO", 0x1 },
    { "JB",  0x2 }, { "JNAE",0x2 }, { "JC",  0x2 },
    { "JAE", 0x3 }, { "JNB", 0x3 }, { "JNC", 0x3 },
    { "JE",  0x4 }, { "JZ",  0x4 },
    { "JNE", 0x5 }, { "JNZ", 0x5 },
    { "JBE", 0x6 }, { "JNA", 0x6 },
    { "JA",  0x7 }, { "JNBE",0x7 },
    { "JS",  0x8 }, { "JNS", 0x9 },
    { "JP",  0xA }, { "JPE", 0xA },
    { "JNP", 0xB }, { "JPO", 0xB },
    { "JL",  0xC }, { "JNGE",0xC },
    { "JGE", 0xD }, { "JNL", 0xD },
    { "JLE", 0xE }, { "JNG", 0xE },
    { "JG",  0xF }, { "JNLE",0xF },
    { 0, 0 }
};

static struct { char *name; int ext; } shift_tab[] = {
    { "ROL", 0 }, { "ROR", 1 }, { "RCL", 2 }, { "RCR", 3 },
    { "SHL", 4 }, { "SAL", 4 }, { "SHR", 5 }, { "SAR", 7 },
    { 0, 0 }
};

static struct { char *name; int op; } fixed_tab[] = {
    { "CLC", 0xF8 }, { "STC", 0xF9 }, { "CMC", 0xF5 },
    { "CLD", 0xFC }, { "STD", 0xFD }, { "CLI", 0xFA }, { "STI", 0xFB },
    { "LAHF", 0x9F }, { "SAHF", 0x9E }, { "HLT", 0xF4 }, { "NOP", 0x90 },
    { "WAIT", 0x9B }, { "CBW", 0x98 }, { "CWD", 0x99 },
    { "XLAT", 0xD7 }, { "XLATB", 0xD7 },
    { "PUSHF", 0x9C }, { "POPF", 0x9D },
    { "MOVSB", 0xA4 }, { "MOVSW", 0xA5 }, { "CMPSB", 0xA6 }, { "CMPSW", 0xA7 },
    { "STOSB", 0xAA }, { "STOSW", 0xAB }, { "LODSB", 0xAC }, { "LODSW", 0xAD },
    { "SCASB", 0xAE }, { "SCASW", 0xAF },
    { "DAA", 0x27 }, { "DAS", 0x2F }, { "AAA", 0x37 }, { "AAS", 0x3F },
    { "INTO", 0xCE }, { "IRET", 0xCF },
    { 0, 0 }
};

static char *regnames[16] = {
    "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH",
    "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"
};

static char *segnames[4] = { "ES", "CS", "SS", "DS" };
static int   segpfx[4]    = { 0x26, 0x2E, 0x36, 0x3E };

/* ------------------------------------------------------------------- state */

static SYM  symtab[MAXSYM];
static int  nsyms;
static unsigned long prevh[MAXSYM];   /* per pass symbol fingerprint          */
static int  prevn;

static char src[LINESZ];
static char work[LINESZ];
static char srcpath[128];
static char outpath[128];
static char tmppath[128];

static unsigned long pcb;       /* current address ($)                        */
static unsigned long imglen;    /* bytes emitted so far                       */
static long orgseen;            /* ORG already given?                         */
static int  finalpass;          /* 1 = hard errors + writing bytes            */
static int  keep_syms;          /* 1 = reuse the converged symbol table       */
static int  emit;               /* 1 = write bytes to outfp                   */
static int  ended;              /* END seen                                   */
static FILE *outfp;
static int  curline;
static int  errflag;            /* sticky per line                            */
static char errbuf[112];
static int  instpfx;            /* segment override for the current line      */
static int  reppfx;             /* rep/repne prefix for the current line      */
static int  shortjmp;           /* SHORT / NEAR keyword seen on JMP           */

static unsigned char ob[24];    /* current instruction image                  */
static int  obn;

/* -------------------------------------------------------------- prototypes */

static unsigned long parse_expr(char **pp, int *symref, int *unk);
static unsigned long parse_atom(char **pp, int *symref, int *unk);
static void do_inst(char *mn, char *argp);

/* ------------------------------------------------------------------- error */

static void asm_err(const char *fmt, ...)
{
    va_list ap;
    if (errflag) return;
    errflag = 1;
    va_start(ap, fmt);
    vsprintf(errbuf, fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------------------- char util */

static int isid0(int c)
{
    c = (int)(unsigned char)c;
    return isalpha(c) || c == '_' || c == '.' || c == '@' || c == '?';
}

static int isidc(int c)
{
    c = (int)(unsigned char)c;
    return isalnum(c) || c == '_' || c == '.' || c == '@' || c == '?';
}

static void skipsp(char **pp)
{
    while (**pp == ' ' || **pp == '\t') (*pp)++;
}

/* Read one identifier-ish token into buf, advance past it and blanks.
 * Returns the token length (0 = no token). */
static int read_id(char **pp, char *buf)
{
    char *s = *pp;
    int n = 0;
    while (isidc(*s)) {
        if (n < NAMELEN - 1) buf[n++] = *s;
        s++;
    }
    buf[n] = 0;
    skipsp(&s);
    *pp = s;
    return n;
}

/* Match keyword `w` at *pp as a whole word; advance past it plus blanks. */
static int kw(char **pp, const char *w)
{
    int n = (int)strlen(w);
    char *s = *pp;
    if (strncmp(s, w, (size_t)n)) return 0;
    if (isidc((int)(unsigned char)s[n])) return 0;
    s += n;
    skipsp(&s);
    *pp = s;
    return 1;
}

static int is_seg_name(const char *name)
{
    int i;
    for (i = 0; i < 4; i++)
        if (!strcmp(segnames[i], name)) return i;
    return -1;
}

static int is_gpr_name(const char *name)
{
    int i;
    for (i = 0; i < 16; i++)
        if (!strcmp(regnames[i], name)) return i;
    return -1;
}

/* -------------------------------------------------------------------- emit */

static void obbyte(int b)
{
    if (obn < (int)sizeof(ob)) ob[obn++] = (unsigned char)(b & 0xFF);
}

static void put_word(unsigned long v)
{
    obbyte((int)(v & 0xFF));
    obbyte((int)((v >> 8) & 0xFF));
}

/* Write the ModRM byte (+ displacement) for operand `m`, /reg field `regf`. */
static void put_modrm(OP *m, int regf)
{
    if (m->kind == OK_REG) {
        obbyte(0xC0 | ((regf & 7) << 3) | (m->reg & 7));
        return;
    }
    if (m->kind != OK_MEM) {
        asm_err("operand is not a register or memory operand");
        return;
    }
    obbyte((m->mod << 6) | ((regf & 7) << 3) | (m->rm & 7));
    if (m->dispbytes == 1) obbyte((int)(m->val & 0xFF));
    else if (m->dispbytes == 2) put_word(m->val);
}

/* Start a new instruction image: rep prefix, then segment override. */
static void begin_inst(void)
{
    obn = 0;
    if (reppfx) obbyte(reppfx);
    if (instpfx) obbyte(instpfx);
}

/* Close the instruction image: account for it, and write it if emitting. */
static void flush_ob(void)
{
    if (errflag) { obn = 0; return; }
    if ((long)(imglen + (unsigned long)obn) > MAXOUT) {
        asm_err("output exceeds %ld bytes (.COM limit)", (long)MAXOUT);
        obn = 0;
        return;
    }
    if (emit && outfp && obn > 0) {
        if (fwrite(ob, 1, (size_t)obn, outfp) != (size_t)obn) {
            asm_err("write error");
            obn = 0;
            return;
        }
    }
    imglen += (unsigned long)obn;
    pcb += (unsigned long)obn;
    obn = 0;
}

/* Data bytes (DB/DW): no prefixes, straight into the image. */
static void data_byte(unsigned long v)
{
    obn = 0;
    obbyte((int)(v & 0xFF));
    flush_ob();
}

static void data_word(unsigned long v)
{
    obn = 0;
    put_word(v);
    flush_ob();
}

/* ---------------------------------------------------------------- symbols */

static unsigned long name_hash(const char *s)
{
    unsigned long h = 5381UL;
    while (*s) h = h * 33UL + (unsigned long)(unsigned char)*s++;
    return h & 0x7FFFUL;
}

static int find_sym(const char *name)
{
    int i;
    for (i = 0; i < nsyms; i++)
        if (!strcmp(symtab[i].name, name)) return i;
    return -1;
}

static int add_sym(const char *name)
{
    int i = find_sym(name);
    int n;
    if (i >= 0) return i;
    if (nsyms >= MAXSYM) {
        asm_err("too many symbols (max %d)", MAXSYM);
        return -1;
    }
    n = (int)strlen(name);
    if (n > NAMELEN - 1) n = NAMELEN - 1;
    memcpy(symtab[nsyms].name, name, (size_t)n);
    symtab[nsyms].name[n] = 0;
    symtab[nsyms].val = 0;
    symtab[nsyms].def = 0;
    return nsyms++;
}

/* Define name = val.  reassign != 0 allows redefinition (EQU / =). */
static void define_sym(const char *name, unsigned long val, int reassign)
{
    int i = add_sym(name);
    if (i < 0) return;
    if (symtab[i].def && !reassign) {
        if (symtab[i].val != val) {
            asm_err("symbol redefined: %s", name);
            return;
        }
        return;                          /* identical duplicate label        */
    }
    symtab[i].val = val;
    symtab[i].def = 1;
}

/* ------------------------------------------------------------ expressions */

static int all_01(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (s[i] != '0' && s[i] != '1') return 0;
    return 1;
}

static int hex_run(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!isxdigit((int)(unsigned char)s[i])) return 0;
    return 1;
}

static int dec_run(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Parse one numeric token.  Accepts 123  1234h  0x1234  1010b  17o  12d */
static unsigned long parse_number(char **pp, int *ok)
{
    char buf[40];
    char *s = *pp;
    int n = 0, i = 0, base = 10;
    unsigned long v = 0;

    while (isalnum((int)(unsigned char)*s)) {
        if (n < 39) buf[n++] = *s;
        s++;
    }
    buf[n] = 0;
    if (n == 0) { *ok = 0; return 0; }
    *ok = 1;
    if (n > 2 && buf[0] == '0' && (buf[1] == 'X' || buf[1] == 'x')) {
        base = 16; i = 2;
    } else if (n > 1 && buf[n-1] == 'H' && hex_run(buf, n - 1)) {
        base = 16; n--;
    } else if (n > 1 && buf[n-1] == 'B' && all_01(buf, n - 1)) {
        base = 2; n--;
    } else if (n > 1 && (buf[n-1] == 'O' || buf[n-1] == 'Q')) {
        base = 8; n--;
    } else if (n > 1 && buf[n-1] == 'D' && dec_run(buf, n - 1)) {
        base = 10; n--;
    } else if (buf[0] >= '0' && buf[0] <= '9') {
        base = 10;
    } else {
        *ok = 0;
        return 0;
    }
    for (; i < n; i++) {
        int c = (int)(unsigned char)buf[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else { *ok = 0; return 0; }
        if (d >= base) { *ok = 0; return 0; }
        v = v * (unsigned long)base + (unsigned long)d;
        if (v > 0xFFFFFFUL) { *ok = 0; return 0; }
    }
    *pp = s;
    return v;
}

/* 'A' -> 0x41, 'AB' -> 0x4241 (low byte = first character) */
static unsigned long parse_charconst(char **pp, int *ok)
{
    char q = **pp;
    char *s = *pp;
    int c1, c2 = 0, n = 0;

    *ok = 0;
    s++;
    c1 = (int)(unsigned char)*s;
    if (!c1) return 0;
    s++;
    n = 1;
    if (*s && *s != q) { c2 = (int)(unsigned char)*s; s++; n = 2; }
    if (*s != q) return 0;
    s++;
    *pp = s;
    *ok = 1;
    if (n == 2) return (unsigned long)((c1 & 0xFF) | ((c2 & 0xFF) << 8));
    return (unsigned long)(c1 & 0xFF);
}

/* One atom: number | symbol | $ | 'c' | OFFSET atom | ( expr ) */
static unsigned long parse_atom(char **pp, int *symref, int *unk)
{
    char *s = *pp;
    char name[NAMELEN];
    int n, idx, ok;
    unsigned long v;

    skipsp(&s);
    *pp = s;
    if (!*s) { asm_err("expression expected"); return 0; }

    if (*s == '(') {
        (*pp)++;
        v = parse_expr(pp, symref, unk);
        skipsp(pp);
        if (**pp == ')') (*pp)++;
        else asm_err("missing ')'");
        return v;
    }
    if (*s == '$') {
        (*pp)++;
        *symref = 1;
        return pcb;
    }
    if (*s == '\'' || *s == '"') {
        v = parse_charconst(&s, &ok);
        if (!ok) { asm_err("bad character constant"); return 0; }
        *pp = s;
        return v;
    }
    if (*s >= '0' && *s <= '9') {
        v = parse_number(pp, &ok);
        if (!ok) { asm_err("bad number"); return 0; }
        return v;
    }
    if (isid0(*s)) {
        n = 0;
        while (isidc(*s)) {
            if (n < NAMELEN - 1) name[n++] = *s;
            s++;
        }
        name[n] = 0;
        *pp = s;
        skipsp(pp);
        if (!strcmp(name, "OFFSET")) return parse_atom(pp, symref, unk);
        idx = find_sym(name);
        if (idx >= 0 && symtab[idx].def) {
            *symref = 1;
            return symtab[idx].val;
        }
        /* not (yet) defined */
        *unk = 1;
        *symref = 1;
        if (finalpass) asm_err("undefined symbol: %s", name);
        return 0;
    }
    asm_err("syntax error in expression");
    return 0;
}

/* One or more + and - operators between atoms (left to right). */
static unsigned long parse_expr(char **pp, int *symref, int *unk)
{
    unsigned long v = 0;
    int sign = 1;
    int sr, ukn;

    *symref = 0;
    *unk = 0;
    for (;;) {
        skipsp(pp);
        sr = 0; ukn = 0;
        if (sign > 0) v += parse_atom(pp, &sr, &ukn);
        else          v -= parse_atom(pp, &sr, &ukn);
        if (sr) *symref = 1;
        if (ukn) *unk = 1;
        if (errflag) break;
        skipsp(pp);
        if (**pp == '+') { sign = 1; (*pp)++; }
        else if (**pp == '-') { sign = -1; (*pp)++; }
        else break;
    }
    return v;
}

/* ---------------------------------------------------------- memory operand */

/* s points at '['.  Fill in the OK_MEM part of *op. */
static int parse_mem(char *s, OP *op)
{
    int hasbx = 0, hasbp = 0, hassi = 0, hasdi = 0;
    int ndisp = 0, sign = 1, n, idx, rm;
    unsigned long disp = 0, v;
    char name[NAMELEN];
    int symref = 0, unk = 0, sr, ukn;

    op->kind = OK_MEM;
    s++;                                     /* skip '['                    */
    for (;;) {
        skipsp(&s);
        if (!*s) { asm_err("missing ']'"); return 0; }
        if (*s == ']') {
            s++;
            skipsp(&s);
            if (*s == '[') { s++; continue; }  /* [bx][si] two groups        */
            break;
        }
        if (*s == '+') { sign = 1; s++; continue; }
        if (*s == '-') { sign = -1; s++; continue; }
        if (*s == '[') { s++; continue; }     /* [bx+[si]]                  */
        n = 0;
        while (isidc(*s)) {
            if (n < NAMELEN - 1) name[n++] = *s;
            s++;
        }
        name[n] = 0;
        if (n == 0) { asm_err("bad memory operand"); return 0; }
        idx = is_gpr_name(name);
        if (idx >= 8) {
            switch (idx) {
            case 11: hasbx = 1; break;        /* BX                          */
            case 13: hasbp = 1; break;        /* BP                          */
            case 14: hassi = 1; break;        /* SI                          */
            case 15: hasdi = 1; break;        /* DI                          */
            default:
                asm_err("register %s cannot be used in an address", name);
                return 0;
            }
            if (hasbx && hasbp) {
                asm_err("invalid address: both BX and BP");
                return 0;
            }
            if (hassi && hasdi) {
                asm_err("invalid address: both SI and DI");
                return 0;
            }
        } else if (idx >= 0) {
            asm_err("register %s cannot be used in an address", name);
            return 0;
        } else {
            char *p = name;
            sr = 0; ukn = 0;
            v = parse_atom(&p, &sr, &ukn);
            if (sign > 0) disp += v;
            else          disp -= v;
            if (sr) symref = 1;
            if (ukn) unk = 1;
            ndisp++;
            sign = 1;
            if (errflag) return 0;
        }
    }
    skipsp(&s);
    if (*s) { asm_err("junk after memory operand: %s", s); return 0; }

    if (!hasbx && !hasbp && !hassi && !hasdi) {
        op->direct = 1;                       /* [disp16]                    */
        op->mod = 0;
        op->rm = 6;
        op->dispbytes = 2;
        op->val = disp;
        op->symref = symref;
        op->unknown = unk;
        return 1;
    }
    op->usess = hasbp;
    if (hasbx || hasbp) {
        if (hasbx && hassi) rm = 0;           /* [BX+SI]                     */
        else if (hasbx && hasdi) rm = 1;      /* [BX+DI]                     */
        else if (hasbp && hassi) rm = 2;      /* [BP+SI]                     */
        else if (hasbp && hasdi) rm = 3;      /* [BP+DI]                     */
        else if (hasbx) rm = 7;               /* [BX]                        */
        else rm = 6;                          /* [BP]                        */
    } else {
        rm = hassi ? 4 : 5;                   /* [SI] / [DI]                 */
    }
    if (ndisp == 0) {
        /* A BP based address defaults to SS and, unlike [BX+SI], has no
         * "no displacement" encoding in the 8086 ModRM table (mod=00 rm=110
         * is the absolute disp16 form), so force an explicit disp8 = 0.
         * LLVM/MASM emit the same explicit form. */
        if (hasbp) { op->mod = 1; op->dispbytes = 1; disp = 0; }
        else { op->mod = 0; op->dispbytes = 0; }
    } else {
        if (symref || unk) op->dispbytes = 2;
        else op->dispbytes = ((long)disp >= -128L && (long)disp <= 127L) ? 1 : 2;
        op->mod = (op->dispbytes == 1) ? 1 : 2;
    }
    op->rm = rm;
    op->val = disp;
    op->symref = symref;
    op->unknown = unk;
    return 1;
}

/* --------------------------------------------------------------- operands */

static void op_init(OP *op)
{
    op->kind = OK_NONE;
    op->reg = 0;
    op->rsize = 0;
    op->segovr = -1;
    op->mod = 0;
    op->rm = 0;
    op->dispbytes = 0;
    op->direct = 0;
    op->usess = 0;
    op->val = 0;
    op->symref = 0;
    op->unknown = 0;
}

static int parse_op(char *s, OP *op)
{
    char *save, *t;
    char name[NAMELEN];
    int n, idx, seg, sr, ukn;

    op_init(op);
    skipsp(&s);
    if (!*s) { asm_err("missing operand"); return 0; }

    /* leading segment override, e.g. "es:[bx]" */
    for (seg = 0; seg < 4; seg++) {
        if (!strncmp(s, segnames[seg], 2) && s[2] == ':') {
            op->segovr = segpfx[seg];
            s += 3;
            skipsp(&s);
            break;
        }
    }
    /* explicit size hint */
    if (kw(&s, "BYTE")) { op->rsize = 1; kw(&s, "PTR"); }
    else if (kw(&s, "WORD")) { op->rsize = 2; kw(&s, "PTR"); }
    else if (kw(&s, "SHORT")) { shortjmp = 1; kw(&s, "PTR"); }
    else if (kw(&s, "NEAR")) { shortjmp = 0; kw(&s, "PTR"); }

    if (!*s) { asm_err("missing operand"); return 0; }
    if (*s == '[') return parse_mem(s, op);

    /* a bare register is a single token and nothing else */
    save = s;
    n = 0;
    while (isidc(*s)) {
        if (n < NAMELEN - 1) name[n++] = *s;
        s++;
    }
    name[n] = 0;
    if (n > 0) {
        t = s;
        skipsp(&t);
        if (!*t && !errflag) {
            idx = is_gpr_name(name);
            if (idx >= 0) {
                op->kind = OK_REG;
                op->reg = idx & 7;
                op->rsize = (idx >= 8) ? 2 : 1;
                return 1;
            }
            seg = is_seg_name(name);
            if (seg >= 0) {
                op->kind = OK_SEG;
                op->reg = seg;
                op->rsize = 2;
                return 1;
            }
        }
    }
    s = save;
    op->kind = OK_IMM;
    sr = 0; ukn = 0;
    op->val = parse_expr(&s, &sr, &ukn);
    op->symref = sr;
    op->unknown = ukn;
    skipsp(&s);
    if (*s && !errflag) { asm_err("junk in operand: %s", s); return 0; }
    return !errflag;
}

/* Operand size in bytes (1 or 2) for a r/m + reg or r/m + imm pair. */
static int opsize(OP *a, OP *b)
{
    int s = 0;
    if (a->kind == OK_REG) s = a->rsize;
    else if (b && b->kind == OK_REG) s = b->rsize;
    else if (a->kind == OK_MEM && a->rsize > 0) s = a->rsize;
    else if (b && b->kind == OK_MEM && b->rsize > 0) s = b->rsize;
    if (a->kind == OK_REG && b && b->kind == OK_REG && a->rsize != b->rsize) {
        asm_err("operand size mismatch");
        return a->rsize;
    }
    if (s <= 0) s = 2;
    return s;
}

/* --------------------------------------------------------------- data dir */

static unsigned long lone_char(char *p, int *used)
{
    /* p points at a quote; returns packed string length 1 or 2 */
    char q = *p;
    char *s = p + 1;
    int n = 0;
    unsigned long v = 0;
    int c;

    *used = 0;
    while (*s && *s != q) {
        c = (int)(unsigned char)*s;
        if (n < 2) v |= (unsigned long)((c & 0xFF) << (8 * n));
        n++;
        s++;
    }
    if (*s != q) { asm_err("unterminated string"); return 0; }
    s++;
    *used = (int)(s - p);
    if (n > 2) { asm_err("string too long for DW (max 2 chars)"); return v; }
    return v;
}

static void do_data(char *p, int isword)
{
    char *s;
    char q;
    unsigned long v;
    int sr, ukn, used, c, n;

    for (;;) {
        skipsp(&p);
        if (!*p) break;
        if (*p == '?' ) {
            p++;
            if (isword) data_word(0); else data_byte(0);
        } else if (*p == '\'' || *p == '"') {
            q = *p;
            if (isword) {
                v = lone_char(p, &used);
                p += used;
                data_word(v);
            } else {
                s = p + 1;
                n = 0;
                while (*s && *s != q) {
                    c = (int)(unsigned char)*s;
                    data_byte((unsigned long)(c & 0xFF));
                    s++;
                    n++;
                }
                if (*s != q) { asm_err("unterminated string"); return; }
                p = s + 1;
                if (n == 0) { asm_err("empty string"); return; }
            }
        } else {
            sr = 0; ukn = 0;
            v = parse_expr(&p, &sr, &ukn);
            if (isword) data_word(v);
            else {
                if (!sr && !ukn && (long)v > 255L) {
                    asm_err("value %ld does not fit in a byte", (long)v);
                    return;
                }
                data_byte(v);
            }
        }
        if (errflag) return;
        skipsp(&p);
        if (*p == ',') { p++; continue; }
        if (*p) { asm_err("junk in data list: %s", p); return; }
        break;
    }
}

/* ----------------------------------------------------------- instruction */

static int is_rep_prefix(const char *mn)
{
    if (!strcmp(mn, "REP") || !strcmp(mn, "REPE") || !strcmp(mn, "REPZ")) {
        reppfx = 0xF3;
        return 1;
    }
    if (!strcmp(mn, "REPNE") || !strcmp(mn, "REPNZ")) {
        reppfx = 0xF2;
        return 1;
    }
    return 0;
}

/* Split an argument string into at most two top level operands. */
static int split_ops(char *p, char **o1, char **o2)
{
    int depth = 0, q = 0, n = 1;
    char *s, *e;

    *o1 = 0;
    *o2 = 0;
    skipsp(&p);
    if (!*p) return 0;
    *o1 = p;
    for (s = p; *s; s++) {
        if (q) { if (*s == q) q = 0; continue; }
        if (*s == '\'' || *s == '"') { q = *s; continue; }
        if (*s == '[') depth++;
        else if (*s == ']') depth--;
        else if (*s == ',' && depth == 0) {
            if (n >= 2) { asm_err("too many operands"); return n; }
            *s = 0;
            skipsp(&p);
            /* trim trailing blanks of the first operand */
            e = s;
            while (e > *o1 && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            s++;
            skipsp(&s);
            *o2 = s;
            n = 2;
        }
    }
    return n;
}

static void ins_alu(int base, int ext, OP *a, OP *b, int na)
{
    int sz;
    if (na != 2) { asm_err("two operands required"); return; }
    if (a->kind == OK_IMM) { asm_err("destination cannot be an immediate"); return; }
    if (a->kind == OK_SEG || b->kind == OK_SEG) { asm_err("bad operand"); return; }
    sz = opsize(a, b);
    if (errflag) return;
    if (b->kind == OK_REG) {                     /* r/m <- r                */
        begin_inst();
        obbyte(base + (sz == 2 ? 1 : 0));
        put_modrm(a, b->reg);
        flush_ob();
        return;
    }
    if (a->kind == OK_REG && b->kind == OK_MEM) { /* r <- r/m               */
        begin_inst();
        obbyte(base + (sz == 2 ? 3 : 2));
        put_modrm(b, a->reg);
        flush_ob();
        return;
    }
    if (b->kind == OK_IMM) {
        int fits = (sz == 2 && !b->symref && !b->unknown &&
                    ((long)b->val >= -128L && (long)b->val <= 127L));
        if (fits) {
            /* 0x83: sign extended imm8 into a 16 bit r/m.  Preferred over
             * the accumulator forms even for AX (same length, LLVM/MASM
             * pick it too). */
            begin_inst();
            obbyte(0x83);
            put_modrm(a, ext);
            obbyte((int)(b->val & 0xFF));
            flush_ob();
            return;
        }
        if (a->kind == OK_REG && a->reg == 0) {  /* AL / AX short form      */
            begin_inst();
            obbyte(base + (sz == 2 ? 5 : 4));
            if (sz == 2) put_word(b->val); else obbyte((int)(b->val & 0xFF));
            flush_ob();
            return;
        }
        if (sz == 2) {
            begin_inst();
            obbyte(0x81);
            put_modrm(a, ext);
            put_word(b->val);
            flush_ob();
            return;
        }
        begin_inst();
        obbyte(0x80);
        put_modrm(a, ext);
        obbyte((int)(b->val & 0xFF));
        flush_ob();
        return;
    }
    asm_err("invalid operands");
}

static void ins_test(OP *a, OP *b, int na)
{
    int sz;
    if (na != 2) { asm_err("TEST needs two operands"); return; }
    sz = opsize(a, b);
    if (errflag) return;
    if (b->kind == OK_REG) {
        begin_inst();
        obbyte(sz == 2 ? 0x85 : 0x84);
        put_modrm(a, b->reg);
        flush_ob();
        return;
    }
    if (b->kind == OK_IMM) {
        if (a->kind == OK_REG && a->reg == 0) {
            begin_inst();
            obbyte(sz == 2 ? 0xA9 : 0xA8);
            if (sz == 2) put_word(b->val); else obbyte((int)(b->val & 0xFF));
            flush_ob();
            return;
        }
        begin_inst();
        obbyte(sz == 2 ? 0xF7 : 0xF6);
        put_modrm(a, 0);
        if (sz == 2) put_word(b->val); else obbyte((int)(b->val & 0xFF));
        flush_ob();
        return;
    }
    asm_err("invalid TEST operands");
}

static void ins_mov(OP *a, OP *b, int na)
{
    int sz;
    if (na != 2) { asm_err("MOV needs two operands"); return; }

    if (a->kind == OK_SEG) {                     /* mov sreg, r/m           */
        if (b->kind != OK_REG && b->kind != OK_MEM) {
            asm_err("MOV to a segment register needs a 16-bit register or memory");
            return;
        }
        if (b->kind == OK_REG && b->rsize != 2) { asm_err("need a 16-bit register"); return; }
        begin_inst();
        obbyte(0x8E);
        put_modrm(b, a->reg);
        flush_ob();
        return;
    }
    if (b->kind == OK_SEG) {                     /* mov r/m, sreg           */
        if (a->kind != OK_REG && a->kind != OK_MEM) { asm_err("bad MOV operands"); return; }
        if (a->kind == OK_REG && a->rsize != 2) { asm_err("need a 16-bit register"); return; }
        begin_inst();
        obbyte(0x8C);
        put_modrm(a, b->reg);
        flush_ob();
        return;
    }
    sz = opsize(a, b);
    if (errflag) return;

    /* AL/AX <-> moffs16 special forms A0..A3 */
    if (b->kind == OK_MEM && b->direct && a->kind == OK_REG && a->reg == 0) {
        begin_inst();
        obbyte(a->rsize == 1 ? 0xA0 : 0xA1);
        put_word(b->val);
        flush_ob();
        return;
    }
    if (a->kind == OK_MEM && a->direct && b->kind == OK_REG && b->reg == 0) {
        begin_inst();
        obbyte(b->rsize == 1 ? 0xA2 : 0xA3);
        put_word(a->val);
        flush_ob();
        return;
    }
    if (a->kind == OK_REG && b->kind == OK_IMM) {
        begin_inst();
        obbyte((sz == 2 ? 0xB8 : 0xB0) + a->reg);
        if (sz == 2) put_word(b->val); else obbyte((int)(b->val & 0xFF));
        flush_ob();
        return;
    }
    if (a->kind == OK_MEM && b->kind == OK_IMM) {
        begin_inst();
        obbyte(sz == 2 ? 0xC7 : 0xC6);
        put_modrm(a, 0);
        if (sz == 2) put_word(b->val); else obbyte((int)(b->val & 0xFF));
        flush_ob();
        return;
    }
    if (b->kind == OK_REG) {
        begin_inst();
        obbyte(sz == 2 ? 0x89 : 0x88);
        put_modrm(a, b->reg);
        flush_ob();
        return;
    }
    if (a->kind == OK_REG && b->kind == OK_MEM) {
        begin_inst();
        obbyte(sz == 2 ? 0x8B : 0x8A);
        put_modrm(b, a->reg);
        flush_ob();
        return;
    }
    asm_err("invalid MOV operands");
}

static void ins_incdec(OP *a, int na, int dec)
{
    int sz;
    if (na != 1) { asm_err("one operand required"); return; }
    if (a->kind == OK_NONE) { asm_err("missing operand"); return; }
    if (a->kind == OK_REG && a->rsize == 2) {
        begin_inst();
        obbyte((dec ? 0x48 : 0x40) + a->reg);
        flush_ob();
        return;
    }
    sz = (a->kind == OK_REG) ? a->rsize : (a->rsize > 0 ? a->rsize : 2);
    begin_inst();
    obbyte(sz == 2 ? 0xFF : 0xFE);
    put_modrm(a, dec ? 1 : 0);
    flush_ob();
}

static void ins_grp3(OP *a, int na, int ext)
{
    int sz;
    if (na != 1) { asm_err("one operand required"); return; }
    if (a->kind == OK_IMM || a->kind == OK_SEG) { asm_err("bad operand"); return; }
    sz = (a->kind == OK_REG) ? a->rsize : (a->rsize > 0 ? a->rsize : 2);
    begin_inst();
    obbyte(sz == 2 ? 0xF7 : 0xF6);
    put_modrm(a, ext);
    flush_ob();
}

static void ins_shift(int ext, OP *a, OP *b, int na)
{
    int sz;
    if (na != 2) { asm_err("shift needs two operands"); return; }
    if (a->kind == OK_IMM || a->kind == OK_SEG) { asm_err("bad operand"); return; }
    sz = (a->kind == OK_REG) ? a->rsize : (a->rsize > 0 ? a->rsize : 2);
    if (b->kind == OK_REG) {
        if (b->rsize != 1 || b->reg != 1) { asm_err("shift count must be CL or an immediate"); return; }
        begin_inst();
        obbyte(sz == 2 ? 0xD3 : 0xD2);
        put_modrm(a, ext);
        flush_ob();
        return;
    }
    if (b->kind != OK_IMM) { asm_err("shift count must be CL or an immediate"); return; }
    if (!b->symref && b->val == 1) {
        begin_inst();
        obbyte(sz == 2 ? 0xD1 : 0xD0);
        put_modrm(a, ext);
        flush_ob();
        return;
    }
    begin_inst();
    obbyte(sz == 2 ? 0xC1 : 0xC0);
    put_modrm(a, ext);
    obbyte((int)(b->val & 0xFF));
    flush_ob();
}

static void ins_push(OP *a, int na)
{
    if (na != 1) { asm_err("PUSH needs one operand"); return; }
    if (a->kind == OK_SEG) {
        begin_inst();
        obbyte(0x06 + (a->reg << 3));
        flush_ob();
        return;
    }
    if (a->kind == OK_REG) {
        if (a->rsize != 2) { asm_err("PUSH needs a 16-bit operand"); return; }
        begin_inst();
        obbyte(0x50 + a->reg);
        flush_ob();
        return;
    }
    if (a->kind == OK_MEM) {
        begin_inst();
        obbyte(0xFF);
        put_modrm(a, 6);
        flush_ob();
        return;
    }
    asm_err("bad PUSH operand");
}

static void ins_pop(OP *a, int na)
{
    if (na != 1) { asm_err("POP needs one operand"); return; }
    if (a->kind == OK_SEG) {
        if (a->reg == 1) { asm_err("POP CS is not a valid instruction"); return; }
        begin_inst();
        obbyte(a->reg == 0 ? 0x07 : (a->reg == 2 ? 0x17 : 0x1F));
        flush_ob();
        return;
    }
    if (a->kind == OK_REG) {
        if (a->rsize != 2) { asm_err("POP needs a 16-bit operand"); return; }
        begin_inst();
        obbyte(0x58 + a->reg);
        flush_ob();
        return;
    }
    if (a->kind == OK_MEM) {
        begin_inst();
        obbyte(0x8F);
        put_modrm(a, 0);
        flush_ob();
        return;
    }
    asm_err("bad POP operand");
}

static void ins_lea(int opc, OP *a, OP *b, int na)
{
    if (na != 2) { asm_err("two operands required"); return; }
    if (a->kind != OK_REG || a->rsize != 2) { asm_err("need a 16-bit register"); return; }
    if (b->kind != OK_MEM) { asm_err("need a memory operand"); return; }
    begin_inst();
    obbyte(opc);
    put_modrm(b, a->reg);
    flush_ob();
}

static void ins_jmpcall(OP *a, int na, int iscall)
{
    long rel;
    if (na != 1) { asm_err("%s needs one operand", iscall ? "CALL" : "JMP"); return; }
    if (a->kind == OK_REG || a->kind == OK_MEM) {
        if (a->kind == OK_REG && a->rsize != 2) { asm_err("need a 16-bit register"); return; }
        begin_inst();
        obbyte(0xFF);
        put_modrm(a, iscall ? 2 : 4);
        flush_ob();
        return;
    }
    if (a->kind != OK_IMM) { asm_err("bad target"); return; }
    if (iscall) {
        rel = (long)(a->val - (pcb + 3UL));
        begin_inst();
        obbyte(0xE8);
        put_word((unsigned long)rel);
        flush_ob();
        return;
    }
    if (shortjmp) {
        rel = (long)(a->val - (pcb + 2UL));
        if (!a->unknown && (rel < -128L || rel > 127L)) {
            asm_err("jump out of range (%ld)", rel);
            return;
        }
        begin_inst();
        obbyte(0xEB);
        obbyte((int)(rel & 0xFF));
        flush_ob();
        return;
    }
    rel = (long)(a->val - (pcb + 3UL));
    begin_inst();
    obbyte(0xE9);
    put_word((unsigned long)rel);
    flush_ob();
}

static void ins_shortjump(int opc, OP *a, int na, const char *what)
{
    long rel;
    if (na != 1) { asm_err("%s needs one operand", what); return; }
    if (a->kind != OK_IMM) { asm_err("%s needs a label or offset", what); return; }
    rel = (long)(a->val - (pcb + 2UL));
    if (!a->unknown && (rel < -128L || rel > 127L)) {
        asm_err("jump out of range (%ld)", rel);
        return;
    }
    begin_inst();
    obbyte(opc);
    obbyte((int)(rel & 0xFF));
    flush_ob();
}

static void ins_inout(char *mn, OP *a, OP *b, int na)
{
    int isin;
    if (na != 2) { asm_err("%s needs two operands", mn); return; }
    isin = (mn[0] == 'I');
    if (isin) {
        /* in AL/AX, DX|imm8 */
        if (a->kind != OK_REG || a->reg != 0) { asm_err("IN dest must be AL or AX"); return; }
        if (b->kind == OK_REG) {
            if (b->rsize != 2 || b->reg != 2) { asm_err("IN port must be DX"); return; }
            begin_inst();
            obbyte(a->rsize == 1 ? 0xEC : 0xED);
            flush_ob();
            return;
        }
        if (b->kind != OK_IMM) { asm_err("IN port must be DX or an 8-bit immediate"); return; }
        begin_inst();
        obbyte(a->rsize == 1 ? 0xE4 : 0xE5);
        obbyte((int)(b->val & 0xFF));
        flush_ob();
        return;
    }
    /* out DX|imm8, AL/AX */
    if (b->kind != OK_REG || b->reg != 0) { asm_err("OUT source must be AL or AX"); return; }
    if (a->kind == OK_REG) {
        if (a->rsize != 2 || a->reg != 2) { asm_err("OUT port must be DX"); return; }
        begin_inst();
        obbyte(b->rsize == 1 ? 0xEE : 0xEF);
        flush_ob();
        return;
    }
    if (a->kind != OK_IMM) { asm_err("OUT port must be DX or an 8-bit immediate"); return; }
    begin_inst();
    obbyte(b->rsize == 1 ? 0xE6 : 0xE7);
    obbyte((int)(a->val & 0xFF));
    flush_ob();
}

static void ins_xchg(OP *a, OP *b, int na)
{
    int sz, t;
    if (na != 2) { asm_err("XCHG needs two operands"); return; }
    if (a->kind == OK_IMM || b->kind == OK_IMM) { asm_err("XCHG cannot take an immediate"); return; }
    if (a->kind == OK_SEG || b->kind == OK_SEG) { asm_err("bad XCHG operand"); return; }
    sz = opsize(a, b);
    if (errflag) return;
    if (a->kind == OK_REG && b->kind == OK_REG) {
        if (sz == 2 && (a->reg == 0 || b->reg == 0) && !(a->reg == 0 && b->reg == 0)) {
            begin_inst();
            obbyte(0x90 + (a->reg == 0 ? b->reg : a->reg));
            flush_ob();
            return;
        }
        t = a->reg; a->reg = b->reg; b->reg = t;
    }
    begin_inst();
    obbyte(sz == 2 ? 0x87 : 0x86);
    if (a->kind == OK_REG) put_modrm(b, a->reg);
    else if (b->kind == OK_REG) put_modrm(a, b->reg);
    else { asm_err("one XCHG operand must be a register"); return; }
    flush_ob();
}

static void ins_ret(char *mn, OP *a, int na)
{
    int isfar = (mn[3] == 'F');            /* RET / RETN vs RETF             */
    if (na == 0) {
        begin_inst();
        obbyte(isfar ? 0xCB : 0xC3);
        flush_ob();
        return;
    }
    if (a->kind != OK_IMM) { asm_err("RET operand must be an immediate"); return; }
    begin_inst();
    obbyte(isfar ? 0xCA : 0xC2);
    put_word(a->val);
    flush_ob();
}

/* The instruction dispatcher. */
static void do_inst(char *mn, char *argp)
{
    OP a, b;
    char *o1, *o2;
    int na, i;

    shortjmp = 0;
    na = split_ops(argp, &o1, &o2);
    op_init(&a);
    op_init(&b);
    if (errflag) return;
    if (na >= 1) { if (!parse_op(o1, &a)) return; }
    if (na >= 2) { if (!parse_op(o2, &b)) return; }
    if (errflag) return;

    /* Segment override: an operand level prefix wins over a line level one.
     * A prefix that merely restates the default segment of the address
     * (DS, or SS for a BP based address) is dropped -- it is a no-op and
     * both LLVM and MASM leave it out. */
    {
        int want = -1, ss = 0, def;
        if (a.segovr >= 0) { want = a.segovr; ss = a.usess; }
        else if (b.segovr >= 0) { want = b.segovr; ss = b.usess; }
        else if (instpfx > 0) {
            want = instpfx;
            ss = (a.kind == OK_MEM) ? a.usess : (b.kind == OK_MEM ? b.usess : 0);
        }
        instpfx = 0;
        if (want >= 0) {
            def = ss ? 0x36 : 0x3E;
            if (want != def) instpfx = want;
        }
    }

    /* ---- zero operand opcodes ---- */
    for (i = 0; fixed_tab[i].name; i++) {
        if (!strcmp(mn, fixed_tab[i].name)) {
            if (na != 0) { asm_err("%s takes no operands", mn); return; }
            begin_inst();
            obbyte(fixed_tab[i].op);
            flush_ob();
            return;
        }
    }
    /* ---- AAM / AAD (opcode + imm8, default 0Ah) ---- */
    if (!strcmp(mn, "AAM") || !strcmp(mn, "AAD")) {
        begin_inst();
        obbyte(mn[2] == 'M' ? 0xD4 : 0xD5);
        if (na == 1) {
            if (a.kind != OK_IMM) { asm_err("%s needs an immediate", mn); return; }
            obbyte((int)(a.val & 0xFF));
        } else if (na == 0) {
            obbyte(0x0A);
        } else { asm_err("%s takes one operand", mn); return; }
        flush_ob();
        return;
    }
    /* ---- ALU family ---- */
    for (i = 0; alu_tab[i].name; i++) {
        if (!strcmp(mn, alu_tab[i].name)) {
            ins_alu(alu_tab[i].base, alu_tab[i].ext, &a, &b, na);
            return;
        }
    }
    /* ---- shifts ---- */
    for (i = 0; shift_tab[i].name; i++) {
        if (!strcmp(mn, shift_tab[i].name)) {
            ins_shift(shift_tab[i].ext, &a, &b, na);
            return;
        }
    }
    /* ---- conditional jumps (always short) ---- */
    for (i = 0; jcc_tab[i].name; i++) {
        if (!strcmp(mn, jcc_tab[i].name)) {
            ins_shortjump(0x70 + jcc_tab[i].cc, &a, na, mn);
            return;
        }
    }
    /* ---- fixed two operand / special encodings ---- */
    if (!strcmp(mn, "MOV")) { ins_mov(&a, &b, na); return; }
    if (!strcmp(mn, "TEST")) { ins_test(&a, &b, na); return; }
    if (!strcmp(mn, "XCHG")) { ins_xchg(&a, &b, na); return; }
    if (!strcmp(mn, "INC")) { ins_incdec(&a, na, 0); return; }
    if (!strcmp(mn, "DEC")) { ins_incdec(&a, na, 1); return; }
    if (!strcmp(mn, "NOT")) { ins_grp3(&a, na, 2); return; }
    if (!strcmp(mn, "NEG")) { ins_grp3(&a, na, 3); return; }
    if (!strcmp(mn, "MUL")) { ins_grp3(&a, na, 4); return; }
    if (!strcmp(mn, "IMUL")) { ins_grp3(&a, na, 5); return; }
    if (!strcmp(mn, "DIV")) { ins_grp3(&a, na, 6); return; }
    if (!strcmp(mn, "IDIV")) { ins_grp3(&a, na, 7); return; }
    if (!strcmp(mn, "PUSH")) { ins_push(&a, na); return; }
    if (!strcmp(mn, "POP")) { ins_pop(&a, na); return; }
    if (!strcmp(mn, "LEA")) { ins_lea(0x8D, &a, &b, na); return; }
    if (!strcmp(mn, "LDS")) { ins_lea(0xC5, &a, &b, na); return; }
    if (!strcmp(mn, "LES")) { ins_lea(0xC4, &a, &b, na); return; }
    if (!strcmp(mn, "JMP")) { ins_jmpcall(&a, na, 0); return; }
    if (!strcmp(mn, "CALL")) { ins_jmpcall(&a, na, 1); return; }
    if (!strcmp(mn, "LOOP")) { ins_shortjump(0xE2, &a, na, mn); return; }
    if (!strcmp(mn, "LOOPE")) { ins_shortjump(0xE1, &a, na, mn); return; }
    if (!strcmp(mn, "LOOPZ")) { ins_shortjump(0xE1, &a, na, mn); return; }
    if (!strcmp(mn, "LOOPNE")) { ins_shortjump(0xE0, &a, na, mn); return; }
    if (!strcmp(mn, "LOOPNZ")) { ins_shortjump(0xE0, &a, na, mn); return; }
    if (!strcmp(mn, "JCXZ")) { ins_shortjump(0xE3, &a, na, mn); return; }
    if (!strcmp(mn, "IN")) { ins_inout(mn, &a, &b, na); return; }
    if (!strcmp(mn, "OUT")) { ins_inout(mn, &a, &b, na); return; }
    if (!strcmp(mn, "RET") || !strcmp(mn, "RETN") || !strcmp(mn, "RETF")) {
        ins_ret(mn, &a, na);
        return;
    }
    if (!strcmp(mn, "INT")) {
        if (na != 1) { asm_err("INT needs one operand"); return; }
        if (a.kind != OK_IMM) { asm_err("INT needs an immediate"); return; }
        if (!a.symref && a.val > 255L) { asm_err("interrupt number out of range"); return; }
        begin_inst();
        obbyte(0xCD);
        obbyte((int)(a.val & 0xFF));
        flush_ob();
        return;
    }
    asm_err("unknown instruction: %s", mn);
}

/* ------------------------------------------------------------- line level */

/* Strip the comment, upper case everything outside quotes. */
static void prep_line(void)
{
    int i, j, q, c;
    q = 0;
    for (i = 0, j = 0; (c = (int)(unsigned char)src[i]) != 0 && j < LINESZ - 2; i++) {
        if (c == '\r' || c == '\n') break;
        if (q) {
            if (c == q) q = 0;
            work[j++] = (char)c;
        } else if (c == ';') {
            break;
        } else if (c == '\'' || c == '"') {
            q = c;
            work[j++] = (char)c;
        } else {
            work[j++] = (char)toupper(c);
        }
    }
    work[j] = 0;
}

static void do_line(void)
{
    char *p, *peek;
    char mn[NAMELEN], nx[NAMELEN];
    int n, n2, labelled;
    unsigned long v;
    int sr, ukn;

    p = work;
    labelled = 0;
    for (;;) {
        skipsp(&p);
        if (!*p) return;
        n = read_id(&p, mn);
        if (n == 0) { asm_err("syntax error"); return; }

        /* instruction level segment override: "es: mov ..." */
        if (is_seg_name(mn) >= 0 && *p == ':') {
            instpfx = segpfx[is_seg_name(mn)];
            p++;
            continue;
        }
        /* "name:" label */
        if (*p == ':' && !labelled) {
            define_sym(mn, pcb, 0);
            if (errflag) return;
            labelled = 1;
            p++;
            continue;
        }
        if (!labelled) {
            peek = p;
            n2 = read_id(&peek, nx);
            if (n2 > 0 && !strcmp(nx, "EQU")) {
                p = peek;
                sr = 0; ukn = 0;
                v = parse_expr(&p, &sr, &ukn);
                if (errflag) return;
                skipsp(&p);
                if (*p) { asm_err("junk after expression: %s", p); return; }
                define_sym(mn, v, 1);
                return;
            }
            if (n2 > 0 && (!strcmp(nx, "DB") || !strcmp(nx, "DW"))) {
                define_sym(mn, pcb, 0);
                if (errflag) return;
                labelled = 1;
                continue;                     /* p still points at DB/DW     */
            }
            if (*p == '=') {
                p++;
                sr = 0; ukn = 0;
                v = parse_expr(&p, &sr, &ukn);
                if (errflag) return;
                skipsp(&p);
                if (*p) { asm_err("junk after expression: %s", p); return; }
                define_sym(mn, v, 1);
                return;
            }
        }
        /* directives */
        if (!strcmp(mn, "ORG")) {
            if (imglen != 0) { asm_err("ORG must come before any code"); return; }
            sr = 0; ukn = 0;
            v = parse_expr(&p, &sr, &ukn);
            if (ukn) { asm_err("ORG needs a constant expression"); return; }
            skipsp(&p);
            if (*p) { asm_err("junk after ORG: %s", p); return; }
            pcb = v;
            orgseen = 1;
            return;
        }
        if (!strcmp(mn, "END")) { ended = 1; return; }
        if (!strcmp(mn, "DB")) { do_data(p, 0); return; }
        if (!strcmp(mn, "DW")) { do_data(p, 1); return; }
        if (is_rep_prefix(mn)) continue;      /* then read the real mnemonic */
        do_inst(mn, p);
        return;
    }
}

/* ------------------------------------------------------------------ passes */

static int assemble(void)
{
    FILE *fp;
    int lineno = 0;
    size_t len;

    if (!keep_syms) {
        memset(symtab, 0, sizeof(symtab));
        nsyms = 0;
    }
    pcb = DEFORG;
    imglen = 0;
    orgseen = 0;
    ended = 0;
    errflag = 0;
    obn = 0;
    lineno = 0;
    instpfx = 0;
    reppfx = 0;

    fp = fopen(srcpath, "r");
    if (!fp) {
        printf("cannot open %s\n", srcpath);
        return -1;
    }
    for (;;) {
        if (fgets(src, LINESZ, fp) == NULL) break;
        lineno++;
        curline = lineno;
        instpfx = 0;
        reppfx = 0;
        errflag = 0;
        len = strlen(src);
        if (len == (size_t)(LINESZ - 1) && src[LINESZ - 2] != '\n')
            asm_err("line too long (max %d characters)", LINESZ - 2);
        if (!errflag) {
            prep_line();
            do_line();
        }
        if (errflag) {
            printf("%d: %s\n", curline, errbuf);
            fclose(fp);
            return 1;
        }
        if (ended) break;
    }
    fclose(fp);
    return 0;
}

/* --------------------------------------------------------------- filenames */

static void make_outname(void)
{
    char *s, *dot, *sep;
    int i, n;

    n = (int)strlen(srcpath);
    if (n > 120) n = 120;
    for (i = 0; i < n; i++) outpath[i] = srcpath[i];
    outpath[n] = 0;

    dot = 0;
    sep = 0;
    for (s = outpath; *s; s++) {
        if (*s == '.') dot = s;
        if (*s == '\\' || *s == '/' || *s == ':') sep = s;
    }
    if (dot && (!sep || dot > sep)) {
        strcpy(dot, ".COM");
    } else {
        strcat(outpath, ".COM");
    }
}

static void make_tmpname(void)
{
    char *s, *sep;
    int i, n;

    sep = 0;
    for (s = outpath; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':') sep = s;
    if (sep) {
        n = (int)(sep - outpath) + 1;
        if (n > 100) n = 100;
        for (i = 0; i < n; i++) tmppath[i] = outpath[i];
        tmppath[n] = 0;
    } else {
        tmppath[0] = 0;
    }
    strcat(tmppath, "ASMTMP.$$$");
}

/* Try rename(); fall back to a byte copy when the DOS rename fails. */
static int move_file(const char *from, const char *to)
{
    FILE *fi, *fo;
    char buf[512];
    size_t n;

    remove(to);
    if (rename(from, to) == 0) return 0;
    fi = fopen(from, "rb");
    if (!fi) return 1;
    fo = fopen(to, "wb");
    if (!fo) { fclose(fi); return 1; }
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) break;
    }
    fclose(fi);
    fclose(fo);
    remove(from);
    return 0;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char *argv[])
{
    int pass, i, same, rc;
    unsigned long expected, h;

    if (argc < 2) {
        printf("ASM - 8086 MASM-lite assembler, writes .COM files\n");
        printf("usage: ASM source.asm [output.com]\n");
        return 1;
    }
    if ((int)strlen(argv[1]) > 120) {
        printf("source path too long\n");
        return 1;
    }
    strcpy(srcpath, argv[1]);
    if (argc >= 3) {
        if ((int)strlen(argv[2]) > 120) {
            printf("output path too long\n");
            return 1;
        }
        strcpy(outpath, argv[2]);
    } else {
        make_outname();
    }
    make_tmpname();

    /* ---- sizing passes: converge on the symbol table ---- */
    prevn = 0;
    expected = 0;
    keep_syms = 0;
    for (pass = 1; pass <= 5; pass++) {
        finalpass = 0;
        emit = 0;
        outfp = 0;
        rc = assemble();
        if (rc != 0) return 1;
        same = (nsyms == prevn);
        if (same) {
            for (i = 0; i < nsyms; i++) {
                h = name_hash(symtab[i].name) * 131UL
                    + (unsigned long)symtab[i].val * 7UL
                    + (unsigned long)symtab[i].def;
                if (h != prevh[i]) { same = 0; break; }
            }
        }
        for (i = 0; i < nsyms; i++) {
            prevh[i] = name_hash(symtab[i].name) * 131UL
                     + (unsigned long)symtab[i].val * 7UL
                     + (unsigned long)symtab[i].def;
        }
        prevn = nsyms;
        expected = imglen;
        if (same) break;
    }
    if (pass > 5) {
        printf("phase error: symbol values did not converge\n");
        return 1;
    }

    /* ---- emit pass: reuse the converged symbol table ---- */
    keep_syms = 1;
    outfp = fopen(tmppath, "wb");
    if (!outfp) {
        printf("cannot create %s\n", tmppath);
        return 1;
    }
    finalpass = 1;
    emit = 1;
    rc = assemble();
    if (outfp) fclose(outfp);
    outfp = 0;
    if (rc != 0) {
        remove(tmppath);
        return 1;
    }
    if (imglen != expected) {
        printf("phase error: pass 1 size %lu, final size %lu\n",
               (unsigned long)expected, (unsigned long)imglen);
        remove(tmppath);
        return 1;
    }
    if (move_file(tmppath, outpath) != 0) {
        printf("cannot write %s\n", outpath);
        remove(tmppath);
        return 1;
    }
    printf("%lu bytes written\n", (unsigned long)imglen);
    return 0;
}
