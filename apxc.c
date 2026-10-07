#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <elf.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#define KW_PRINT  "print"   // Ausgabe
#define KW_IF     "if"      // Bedingung
#define KW_ELSE   "else"    // Sonst Zweig
#define KW_END    "end"     // Blockende
#define KW_LOOP   "loop"    // Bedingungsschleife
#define KW_REPEAT "repeat"  // Zählschleife
#define KW_AS     "as"      // Typ oder Zähler
#define KW_AND    "and"     // Logisches Und
#define KW_OR     "or"      // Logisches Oder
#define KW_NOT    "not"     // Logische Negation
#define KW_MOD    "mod"     // Restwert
#define KW_FUNC   "func"    // Funktion
#define KW_GIVE   "give"    // Rueckgabe
#define KW_STOP   "stop"    // Schleife abbrechen
#define KW_NEXT   "next"    // Nächster Durchlauf
#define KW_UNDO   "undo"    // Variablenhistorie zuruecksetzen
#define KW_TEXT   "text"    // Texttyp
#define KW_INT    "int"     // Zahlentyp
#define KW_LEN    "len"     // Länge
#define KW_STR    "str"     // Zahl zu Text
#define KW_INPUT  "input"   // Texteingabe
#define KW_NUMBER "zahl"    // Text zu Zahl
#define KW_LIST   "liste"   // Leere Liste
#define KW_APPEND "attach"  // Element an Liste anhängen
#define KW_GET    "holen"   // Listenelement lesen
#define KW_SHOW   "zeige"   // Ausgabe
#define KW_IF_DE  "wenn"    // Bedingung
#define KW_ELSE_DE "sonst"  // Sonst-Zweig
#define KW_END_DE "ende"    // Blockende
#define KW_LOOP_DE "solange" // Bedingungsschleife
#define KW_REPEAT_DE "wiederhole" // Zählschleife
#define KW_FUNC_DE "funktion" // Funktion
#define KW_GIVE_DE "zurueck" // Rueckgabe
#define KW_STOP_DE "abbruch" // Schleife abbrechen
#define KW_NEXT_DE "weiter" // Naechster Durchlauf
#define KW_UNDO_DE "ruecksetzen" // Historie zuruecksetzen
#define KW_ASK "frage"      // Frage mit Eingabe
#define KW_CHANGED "changed" // Historienvergleich
#define KW_CHANGED_DE "anders" // Seit letztem Wert geaendert?
#define KW_TRUE "wahr"      // Wahr
#define KW_FALSE "falsch"   // Falsch

#define SYM_COMMENT "~"
#define SYM_ARROW   "->"
#define SYM_EQ      "="
#define SYM_NE      "<>"
#define SYM_LT      "<"
#define SYM_GT      ">"
#define SYM_LE      "<="
#define SYM_GE      ">="
#define SYM_AT      "@"

#define A_CODE     0x400000ULL
#define A_STR      0x10000000
#define A_DATA     0x20000000
#define A_HEAPBASE 0x40000000
#define A_HEAPSIZE 0x20000000ULL
#define A_HEAP     (A_DATA)
#define A_OUTLEN   (A_DATA + 8)
#define A_NUMBUF   (A_DATA + 16)
#define A_OUTBUF   (A_DATA + 0x100)
#define A_GLOB     (A_DATA + 0x11100)
#define A_INBUF    (A_DATA + 0x10100)
#define INBUFSIZE  4096
#define OUTSIZE    65536

#define MAXVARS   1024
#define MAXFUNCS  256
#define MAXPARAMS 16
#define MAXLOOPS  64
#define MAXDEPTH  64

#define TY_INT  0
#define TY_TEXT 1
#define TY_LIST 2

static const char *srcname = "?";
static char *source_text = NULL;
static int diagnostic_line = 1, diagnostic_column = 1;

static void die(int line, const char *msg, const char *detail) {
    int column = diagnostic_line == line ? diagnostic_column : 1;
    fprintf(stderr, "%s:%d:%d: Fehler: %s%s%s\n", srcname, line, column, msg,
            detail ? ": " : "", detail ? detail : "");
    if (source_text) {
        const char *start = source_text;
        int current = 1;
        while (current < line && *start) {
            if (*start++ == '\n') current++;
        }
        const char *end = start;
        while (*end && *end != '\n') end++;
        if (end >= start) {
            size_t length = (size_t)(end - start);
            int caret_column = column;
            if ((size_t)caret_column > length + 1) caret_column = (int)length + 1;
            fprintf(stderr, "%5d | %.*s\n      | ", line, (int)length, start);
            for (int i = 1; i < caret_column; i++)
                fputc(start[i - 1] == '\t' ? '\t' : ' ', stderr);
            fputs("^\n", stderr);
        }
    }
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "Kein Speicher\n"); exit(1); }
    return p;
}

static char *fmt(const char *f, ...) {
    va_list ap;
    char *s = NULL;
    va_start(ap, f);
    if (vasprintf(&s, f, ap) < 0) { fprintf(stderr, "Kein Speicher\n"); exit(1); }
    va_end(ap);
    return s;
}

typedef enum {
    T_EOF, T_NEWLINE, T_IDENT, T_NUMBER, T_STRING,
    T_PRINT, T_IF, T_ELSE, T_END, T_LOOP, T_REPEAT, T_AS,
    T_AND, T_OR, T_NOT, T_MOD,
    T_FUNC, T_GIVE, T_STOP, T_NEXT, T_UNDO, T_TEXTKW, T_INTKW, T_LISTKW,
    T_TRUE, T_FALSE,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_LPAREN, T_RPAREN, T_COMMA,
    T_ARROW, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE, T_AT
} TokType;

typedef struct { TokType type; char *text; int line, column; } Token;

static const struct { const char *word; TokType type; } keywords[] = {
    { KW_PRINT, T_PRINT }, { KW_IF, T_IF },     { KW_ELSE, T_ELSE },
    { KW_END, T_END },     { KW_LOOP, T_LOOP }, { KW_REPEAT, T_REPEAT },
    { KW_AS, T_AS },       { KW_AND, T_AND },   { KW_OR, T_OR },
    { KW_NOT, T_NOT },     { KW_MOD, T_MOD },   { KW_FUNC, T_FUNC },
    { KW_GIVE, T_GIVE },   { KW_STOP, T_STOP }, { KW_NEXT, T_NEXT },
    { KW_UNDO, T_UNDO },   { KW_TEXT, T_TEXTKW }, { KW_INT, T_INTKW },
    { KW_LIST, T_LISTKW }, { KW_INPUT, T_IDENT }, { KW_NUMBER, T_IDENT },
    { KW_APPEND, T_IDENT }, { KW_GET, T_IDENT },
    { KW_SHOW, T_PRINT }, { KW_IF_DE, T_IF }, { KW_ELSE_DE, T_ELSE },
    { KW_END_DE, T_END }, { KW_LOOP_DE, T_LOOP }, { KW_REPEAT_DE, T_REPEAT },
    { KW_FUNC_DE, T_FUNC }, { KW_GIVE_DE, T_GIVE }, { KW_STOP_DE, T_STOP },
    { KW_NEXT_DE, T_NEXT }, { KW_UNDO_DE, T_UNDO }, { KW_ASK, T_IDENT },
    { KW_CHANGED, T_IDENT }, { KW_CHANGED_DE, T_IDENT },
    { KW_TRUE, T_TRUE }, { KW_FALSE, T_FALSE },
};

static const struct { const char *text; TokType type; } symbols[] = {
    { SYM_ARROW, T_ARROW }, { SYM_EQ, T_EQ }, { SYM_NE, T_NE },
    { SYM_LT, T_LT },       { SYM_GT, T_GT }, { SYM_LE, T_LE }, { SYM_GE, T_GE },
    { SYM_AT, T_AT },
    { "+", T_PLUS },  { "-", T_MINUS },  { "*", T_STAR }, { "/", T_SLASH },
    { "(", T_LPAREN }, { ")", T_RPAREN }, { ",", T_COMMA },
};

static Token *tokens = NULL;
static int ntokens = 0, captokens = 0;

static void push_tok(TokType t, const char *s, size_t n, int line, int column) {
    if (ntokens == captokens) {
        captokens = captokens ? captokens * 2 : 64;
        tokens = xrealloc(tokens, captokens * sizeof(Token));
    }
    char *copy = malloc(n + 1);
    if (!copy) { fprintf(stderr, "Kein Speicher\n"); exit(1); }
    memcpy(copy, s, n);
    copy[n] = '\0';
    tokens[ntokens].type = t;
    tokens[ntokens].text = copy;
    tokens[ntokens].line = line;
    tokens[ntokens].column = column;
    ntokens++;
}

static void lex(const char *src) {
    int line = 1, column = 1;
    const char *p = src;
    while (*p) {
        char c = *p;
        diagnostic_line = line;
        diagnostic_column = column;
        if (c == '\n') { push_tok(T_NEWLINE, "", 0, line, column); line++; column = 1; p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { p++; column++; continue; }

        if (strncmp(p, SYM_COMMENT, strlen(SYM_COMMENT)) == 0) {
            while (*p && *p != '\n') { p++; column++; }
            continue;
        }

        if (isdigit((unsigned char)c)) {
            const char *s = p;
            while (isdigit((unsigned char)*p)) p++;
            push_tok(T_NUMBER, s, p - s, line, column);
            column += (int)(p - s);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t n = p - s;
            TokType type = T_IDENT;
            for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
                if (strlen(keywords[i].word) == n && strncmp(s, keywords[i].word, n) == 0)
                    type = keywords[i].type;
            push_tok(type, s, n, line, column);
            column += (int)n;
            continue;
        }

        if (c == '"') {
            int start_column = column;
            p++;
            column++;
            const char *s = p;
            while (*p != '"') {
                if (*p == '\0' || *p == '\n') die(line, "Text nicht geschlossen", NULL);
                if (*p == '\\' && p[1] != '\0') { p++; column++; }
                p++;
                column++;
            }
            push_tok(T_STRING, s, p - s, line, start_column);
            p++;
            column++;
            continue;
        }

        int best = -1;
        size_t bestlen = 0;
        for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
            size_t l = strlen(symbols[i].text);
            if (l > bestlen && strncmp(p, symbols[i].text, l) == 0) { best = (int)i; bestlen = l; }
        }
        if (best < 0) {
            char tmp[2] = { c, '\0' };
            die(line, "Unbekanntes Zeichen", tmp);
        }
        push_tok(symbols[best].type, p, bestlen, line, column);
        p += bestlen;
        column += (int)bestlen;
    }
    push_tok(T_EOF, "", 0, line, column);
}

static unsigned char *code = NULL;
static int clen = 0, ccap = 0;

static void eb(int b) {
    if (clen == ccap) { ccap = ccap ? ccap * 2 : 4096; code = xrealloc(code, ccap); }
    code[clen++] = (unsigned char)b;
}

static void e32(unsigned v) { for (int i = 0; i < 4; i++) eb((v >> (8 * i)) & 255); }
static void e64(unsigned long long v) { for (int i = 0; i < 8; i++) eb((v >> (8 * i)) & 255); }

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11 };
enum { A_ADD = 0, A_OR = 1, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };
enum { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };

static int cmp_start = -1, cmp_end = -1, cmp_cc = 0;

typedef struct { int start, end, kind; long long imm; void *v; int j; } Simple;
static Simple ls;

static void trunc_to(int to) {
    clen = to;
    cmp_end = -1;
    ls.kind = 0;
}

static void opc(int op) { if (op > 0xFF) eb(0x0F); eb(op & 0xFF); }
static void rex(int reg, int rm) { eb(0x48 | ((reg >> 3) << 2) | (rm >> 3)); }

static void op_rr(int op, int reg, int rm) {
    rex(reg, rm);
    opc(op);
    eb(0xC0 | ((reg & 7) << 3) | (rm & 7));
}

static void op_rm(int op, int reg, int base, int disp) {
    rex(reg, base);
    opc(op);
    eb(0x80 | ((reg & 7) << 3) | (base & 7));
    if ((base & 7) == 4) eb(0x24);
    e32((unsigned)disp);
}

static void op_ra(int op, int reg, int addr) {
    rex(reg, 0);
    opc(op);
    eb(((reg & 7) << 3) | 4);
    eb(0x25);
    e32((unsigned)addr);
}

static void mov_rr(int dst, int src) { op_rr(0x89, src, dst); }
static void load(int reg, int base, int disp) { op_rm(0x8B, reg, base, disp); }
static void store(int base, int disp, int reg) { op_rm(0x89, reg, base, disp); }
static void load_byte(int reg, int base, int disp) { op_rm(0x0FB6, reg, base, disp); }
static void load_indexed_byte(int reg, int base, int index) {
    rex(reg, base);
    opc(0x0FB6);
    eb(0x04 | ((reg & 7) << 3));
    eb(((index & 7) << 3) | (base & 7));
}
static void lea(int reg, int base, int disp) { op_rm(0x8D, reg, base, disp); }
static void load_abs(int reg, int addr) { op_ra(0x8B, reg, addr); }
static void store_abs(int addr, int reg) { op_ra(0x89, reg, addr); }
static void alu_rr(int ext, int dst, int src) { op_rr(ext * 8 + 1, src, dst); }
static void imul_rr(int dst, int src) { op_rr(0x0FAF, dst, src); }
static void test_rr(int r) { op_rr(0x85, r, r); }

static void alu_ri(int ext, int reg, int imm) {
    rex(0, reg);
    if (imm >= -128 && imm <= 127) {
        eb(0x83);
        eb(0xC0 | (ext << 3) | (reg & 7));
        eb(imm & 255);
    } else {
        eb(0x81);
        eb(0xC0 | (ext << 3) | (reg & 7));
        e32((unsigned)imm);
    }
}

static void shl_ri(int reg, int imm) {
    rex(0, reg);
    eb(0xC1);
    eb(0xE0 | (reg & 7));
    eb(imm);
}

static void mov_ri(int reg, long long v) {
    if (v >= 0 && v <= 0xFFFFFFFFLL) {
        if (reg >= 8) eb(0x41);
        eb(0xB8 + (reg & 7));
        e32((unsigned)v);
    } else if (v >= INT32_MIN && v < 0) {
        rex(0, reg);
        eb(0xC7);
        eb(0xC0 | (reg & 7));
        e32((unsigned)(int)v);
    } else {
        rex(0, reg);
        eb(0xB8 + (reg & 7));
        e64((unsigned long long)v);
    }
}

static void neg_r(int reg) { rex(0, reg); eb(0xF7); eb(0xD8 | (reg & 7)); }
static void div_r(int reg) { rex(0, reg); eb(0xF7); eb(0xF0 | (reg & 7)); }
static void idiv_r(int reg) { rex(0, reg); eb(0xF7); eb(0xF8 | (reg & 7)); }
static void cqo(void) { eb(0x48); eb(0x99); }
static void push_r(int r) { if (r >= 8) eb(0x41); eb(0x50 + (r & 7)); }
static void pop_r(int r) { if (r >= 8) eb(0x41); eb(0x58 + (r & 7)); }
static void ret(void) { eb(0xC3); }
static void leave(void) { eb(0xC9); }
static void syscall_(void) { eb(0x0F); eb(0x05); }
static void rep_movsb(void) { eb(0xF3); eb(0xA4); }
static void repe_cmpsb(void) { eb(0xF3); eb(0xA6); }

static void setcc(int cc) {
    eb(0x0F); eb(0x90 + cc); eb(0xC0);
    eb(0x48); eb(0x0F); eb(0xB6); eb(0xC0);
}

enum { L_MAIN, L_FLUSH, L_WRITE, L_PRTEXT, L_FMTINT, L_PRINT, L_MKTEXT,
       L_STRINT, L_CONCAT, L_STREQ, L_DIVZ, L_INPUT, L_PARSEINT,
       L_NEWLIST, L_APPEND, L_GET, L_RTERR, L_BADNUM, L_BOUNDS,
       L_INPUTLONG, L_HEAPERR, L_IOERR, L_OVERFLOW, L_FIRST };

static int *lpos = NULL;
static int nlab = 0, lcap = 0;

typedef struct { int pos, label; } Fix;
static Fix *fixes = NULL;
static int nfix = 0, fixcap = 0;

static int newlabel(void) {
    if (nlab == lcap) { lcap = lcap ? lcap * 2 : 256; lpos = xrealloc(lpos, lcap * sizeof(int)); }
    lpos[nlab] = -1;
    return nlab++;
}

static void bind(int l) { lpos[l] = clen; }

static void ref(int l) {
    if (nfix == fixcap) { fixcap = fixcap ? fixcap * 2 : 256; fixes = xrealloc(fixes, fixcap * sizeof(Fix)); }
    fixes[nfix].pos = clen;
    fixes[nfix].label = l;
    nfix++;
    e32(0);
}

static void jmp(int l) { eb(0xE9); ref(l); }
static void jcc(int cc, int l) { eb(0x0F); eb(0x80 + cc); ref(l); }
static void call(int l) { eb(0xE8); ref(l); }

static unsigned char *sdata = NULL;
static int slen = 0, scap = 0;

static void sb(int b) {
    if (slen == scap) { scap = scap ? scap * 2 : 1024; sdata = xrealloc(sdata, scap); }
    sdata[slen++] = (unsigned char)b;
}

static unsigned add_string(const char *s, size_t n) {
    size_t limit = (size_t)(A_DATA - A_STR);
    if (n > SIZE_MAX - 7) {
        fprintf(stderr, "Fehler: Zeichenbereich zu gross\n");
        exit(1);
    }
    size_t padded = (n + 7) & ~(size_t)7;
    if (slen < 0 || padded > limit - 8 || (size_t)slen > limit - 8 - padded) {
        fprintf(stderr, "Fehler: Zeichenbereich ist groesser als 256 MiB\n");
        exit(1);
    }
    unsigned addr = A_STR + slen;
    for (int i = 0; i < 8; i++) sb((int)(((unsigned long long)n >> (8 * i)) & 255));
    for (size_t i = 0; i < n; i++) sb((unsigned char)s[i]);
    while (slen % 8) sb(0);
    return addr;
}

static unsigned str_empty, str_nl, str_divz, str_badnum, str_bounds, str_inputlong, str_heaperr, str_ioerr, str_overflow;
static const char divz_msg[] = "Laufzeitfehler: Division durch 0\n";
static const char badnum_msg[] = "Laufzeitfehler: Eingabe ist keine gueltige Zahl\n";
static const char bounds_msg[] = "Laufzeitfehler: Listenindex ausserhalb der Liste\n";
static const char inputlong_msg[] = "Laufzeitfehler: Eingabe ist laenger als 4095 Bytes\n";
static const char heaperr_msg[] = "Laufzeitfehler: APX-Speicher ist voll\n";
static const char ioerr_msg[] = "Laufzeitfehler: Ein-/Ausgabefehler\n";
static const char overflow_msg[] = "Laufzeitfehler: Zahlenueberlauf bei Division\n";

static void gen_runtime(void) {
    for (int i = 0; i < L_FIRST; i++) newlabel();
    str_divz = add_string(divz_msg, sizeof(divz_msg) - 1);
    str_badnum = add_string(badnum_msg, sizeof(badnum_msg) - 1);
    str_bounds = add_string(bounds_msg, sizeof(bounds_msg) - 1);
    str_inputlong = add_string(inputlong_msg, sizeof(inputlong_msg) - 1);
    str_heaperr = add_string(heaperr_msg, sizeof(heaperr_msg) - 1);
    str_ioerr = add_string(ioerr_msg, sizeof(ioerr_msg) - 1);
    str_overflow = add_string(overflow_msg, sizeof(overflow_msg) - 1);

    jmp(L_MAIN);

    bind(L_FLUSH);
    load_abs(RDX, A_OUTLEN);
    int flush_done = newlabel(), flush_loop = newlabel();
    test_rr(RDX);
    jcc(CC_E, flush_done);
    mov_ri(RSI, A_OUTBUF);
    bind(flush_loop);
    mov_ri(RAX, 1);
    mov_ri(RDI, 1);
    syscall_();
    test_rr(RAX);
    jcc(CC_LE, L_IOERR);
    alu_rr(A_SUB, RDX, RAX);
    alu_rr(A_ADD, RSI, RAX);
    test_rr(RDX);
    jcc(CC_NE, flush_loop);
    mov_ri(RAX, 0);
    store_abs(A_OUTLEN, RAX);
    bind(flush_done);
    ret();

    int copy = newlabel();
    bind(L_WRITE);
    load_abs(RAX, A_OUTLEN);
    mov_rr(RCX, RAX);
    alu_rr(A_ADD, RCX, RDX);
    alu_ri(A_CMP, RCX, OUTSIZE);
    jcc(CC_BE, copy);
    push_r(RSI);
    push_r(RDX);
    call(L_FLUSH);
    pop_r(RDX);
    pop_r(RSI);
    mov_ri(RAX, 0);
    alu_ri(A_CMP, RDX, OUTSIZE);
    jcc(CC_BE, copy);
    mov_rr(R8, RDX);
    int direct_loop = newlabel();
    bind(direct_loop);
    mov_ri(RAX, 1);
    mov_ri(RDI, 1);
    syscall_();
    test_rr(RAX);
    jcc(CC_LE, L_IOERR);
    alu_rr(A_SUB, R8, RAX);
    alu_rr(A_ADD, RSI, RAX);
    test_rr(R8);
    jcc(CC_NE, direct_loop);
    ret();
    bind(copy);
    mov_ri(RDI, A_OUTBUF);
    alu_rr(A_ADD, RDI, RAX);
    alu_rr(A_ADD, RAX, RDX);
    store_abs(A_OUTLEN, RAX);
    mov_rr(RCX, RDX);
    rep_movsb();
    ret();

    bind(L_PRTEXT);
    load(RDX, RAX, 0);
    lea(RSI, RAX, 8);
    jmp(L_WRITE);

    int posl = newlabel(), digl = newlabel(), donel = newlabel();
    bind(L_FMTINT);
    mov_rr(RCX, RAX);
    mov_ri(RBX, 0);
    test_rr(RCX);
    jcc(CC_GE, posl);
    neg_r(RCX);
    mov_ri(RBX, 1);
    bind(posl);
    mov_ri(RDI, A_NUMBUF + 32);
    mov_ri(RSI, 10);
    bind(digl);
    mov_ri(RDX, 0);
    mov_rr(RAX, RCX);
    div_r(RSI);
    mov_rr(RCX, RAX);
    eb(0x80); eb(0xC2); eb(0x30);
    alu_ri(A_SUB, RDI, 1);
    eb(0x88); eb(0x17);
    test_rr(RCX);
    jcc(CC_NE, digl);
    test_rr(RBX);
    jcc(CC_E, donel);
    alu_ri(A_SUB, RDI, 1);
    eb(0xC6); eb(0x07); eb('-');
    bind(donel);
    mov_rr(RSI, RDI);
    mov_ri(RDX, A_NUMBUF + 32);
    alu_rr(A_SUB, RDX, RSI);
    ret();

    bind(L_PRINT);
    call(L_FMTINT);
    jmp(L_WRITE);

    bind(L_MKTEXT);
    load_abs(RAX, A_HEAP);
    lea(R8, RAX, 8);
    alu_rr(A_ADD, R8, RDX);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_ADD, R8, 7);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_AND, R8, -8);
    mov_ri(R9, A_HEAPBASE + A_HEAPSIZE);
    alu_rr(A_CMP, R8, R9);
    jcc(CC_A, L_HEAPERR);
    store(RAX, 0, RDX);
    lea(RDI, RAX, 8);
    mov_rr(RCX, RDX);
    rep_movsb();
    alu_ri(A_ADD, RDI, 7);
    alu_ri(A_AND, RDI, -8);
    store_abs(A_HEAP, RDI);
    ret();

    bind(L_STRINT);
    call(L_FMTINT);
    jmp(L_MKTEXT);

    bind(L_CONCAT);
    load_abs(RAX, A_HEAP);
    load(RDX, RDI, 0);
    load(RCX, RSI, 0);
    mov_rr(R8, RDX);
    alu_rr(A_ADD, R8, RCX);
    jcc(CC_B, L_HEAPERR);
    mov_rr(R9, RAX);
    alu_ri(A_ADD, R9, 8);
    jcc(CC_B, L_HEAPERR);
    alu_rr(A_ADD, R9, R8);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_ADD, R9, 7);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_AND, R9, -8);
    mov_ri(R10, A_HEAPBASE + A_HEAPSIZE);
    alu_rr(A_CMP, R9, R10);
    jcc(CC_A, L_HEAPERR);
    store(RAX, 0, R8);
    push_r(RAX);
    push_r(RCX);
    push_r(RSI);
    lea(RSI, RDI, 8);
    lea(RDI, RAX, 8);
    mov_rr(RCX, RDX);
    rep_movsb();
    pop_r(RSI);
    pop_r(RCX);
    alu_ri(A_ADD, RSI, 8);
    rep_movsb();
    alu_ri(A_ADD, RDI, 7);
    alu_ri(A_AND, RDI, -8);
    store_abs(A_HEAP, RDI);
    pop_r(RAX);
    ret();

    int nel = newlabel(), eql = newlabel();
    bind(L_STREQ);
    load(RCX, RDI, 0);
    load(RDX, RSI, 0);
    alu_rr(A_CMP, RCX, RDX);
    jcc(CC_NE, nel);
    test_rr(RCX);
    jcc(CC_E, eql);
    alu_ri(A_ADD, RDI, 8);
    alu_ri(A_ADD, RSI, 8);
    repe_cmpsb();
    jcc(CC_NE, nel);
    bind(eql);
    mov_ri(RAX, 1);
    ret();
    bind(nel);
    mov_ri(RAX, 0);
    ret();

    bind(L_DIVZ);
    call(L_FLUSH);
    mov_ri(RAX, 1);
    mov_ri(RDI, 2);
    mov_ri(RSI, str_divz + 8);
    mov_ri(RDX, sizeof(divz_msg) - 1);
    syscall_();
    mov_ri(RAX, 60);
    mov_ri(RDI, 1);
    syscall_();

    bind(L_RTERR);
    push_r(RSI);
    push_r(RDX);
    call(L_FLUSH);
    pop_r(RDX);
    pop_r(RSI);
    mov_ri(RAX, 1);
    mov_ri(RDI, 2);
    syscall_();
    mov_ri(RAX, 60);
    mov_ri(RDI, 1);
    syscall_();

    int input_top = newlabel(), input_done = newlabel(), input_check_cr = newlabel();
    int input_full = newlabel(), input_finish = newlabel();
    bind(L_INPUT);
    call(L_FLUSH);
    mov_ri(R8, 0);
    bind(input_top);
    mov_ri(RAX, INBUFSIZE - 1);
    alu_rr(A_CMP, R8, RAX);
    jcc(CC_AE, input_full);
    mov_ri(RAX, 0);
    mov_ri(RDI, 0);
    mov_ri(RSI, A_INBUF);
    alu_rr(A_ADD, RSI, R8);
    mov_ri(RDX, 1);
    syscall_();
    test_rr(RAX);
    jcc(CC_L, L_IOERR);
    mov_ri(RCX, 1);
    alu_rr(A_CMP, RAX, RCX);
    jcc(CC_NE, input_done);
    load_byte(RDX, RSI, 0);
    alu_ri(A_CMP, RDX, '\n');
    jcc(CC_E, input_done);
    alu_ri(A_ADD, R8, 1);
    jmp(input_top);
    bind(input_full);
    mov_ri(RAX, 0);
    mov_ri(RDI, 0);
    mov_ri(RSI, A_INBUF + INBUFSIZE - 1);
    mov_ri(RDX, 1);
    syscall_();
    test_rr(RAX);
    jcc(CC_E, input_finish);
    jcc(CC_L, L_IOERR);
    load_byte(RCX, RSI, 0);
    alu_ri(A_CMP, RCX, '\n');
    jcc(CC_NE, L_INPUTLONG);
    bind(input_finish);
    jmp(input_done);
    bind(input_done);
    test_rr(R8);
    jcc(CC_E, input_check_cr);
    mov_ri(RSI, A_INBUF);
    mov_rr(RCX, R8);
    alu_ri(A_SUB, RCX, 1);
    load_indexed_byte(RDX, RSI, RCX);
    alu_ri(A_CMP, RDX, '\r');
    jcc(CC_NE, input_check_cr);
    alu_ri(A_SUB, R8, 1);
    bind(input_check_cr);
    mov_ri(RSI, A_INBUF);
    mov_rr(RDX, R8);
    call(L_MKTEXT);
    ret();

    int parse_loop = newlabel(), parse_positive = newlabel(), parse_negative = newlabel();
    int parse_signed = newlabel(), parse_done = newlabel(), parse_accumulate = newlabel();
    int parse_negative_limit = newlabel(), parse_limit_check = newlabel();
    bind(L_PARSEINT);
    load(RCX, RAX, 0);
    test_rr(RCX);
    jcc(CC_E, L_BADNUM);
    lea(RSI, RAX, 8);
    mov_ri(RBX, 1);
    load_byte(R8, RSI, 0);
    alu_ri(A_CMP, R8, '-');
    jcc(CC_E, parse_negative);
    alu_ri(A_CMP, R8, '+');
    jcc(CC_E, parse_positive);
    jmp(parse_signed);
    bind(parse_negative);
    mov_ri(RBX, -1);
    alu_ri(A_ADD, RSI, 1);
    alu_ri(A_SUB, RCX, 1);
    jmp(parse_signed);
    bind(parse_positive);
    alu_ri(A_ADD, RSI, 1);
    alu_ri(A_SUB, RCX, 1);
    bind(parse_signed);
    mov_ri(RDX, 0);
    test_rr(RCX);
    jcc(CC_E, L_BADNUM);
    bind(parse_loop);
    load_byte(R8, RSI, 0);
    alu_ri(A_CMP, R8, '0');
    jcc(CC_B, L_BADNUM);
    alu_ri(A_CMP, R8, '9');
    jcc(CC_A, L_BADNUM);
    alu_ri(A_SUB, R8, '0');
    mov_rr(RAX, RDX);
    mov_ri(R9, 0x0CCCCCCCCCCCCCCCULL);
    alu_rr(A_CMP, RAX, R9);
    jcc(CC_A, L_BADNUM);
    jcc(CC_NE, parse_accumulate);
    alu_ri(A_CMP, RBX, 0);
    jcc(CC_L, parse_negative_limit);
    mov_ri(R9, 7);
    jmp(parse_limit_check);
    bind(parse_negative_limit);
    mov_ri(R9, 8);
    bind(parse_limit_check);
    alu_rr(A_CMP, R8, R9);
    jcc(CC_A, L_BADNUM);
    bind(parse_accumulate);
    mov_ri(R9, 10);
    imul_rr(RAX, R9);
    alu_rr(A_ADD, RAX, R8);
    mov_rr(RDX, RAX);
    alu_ri(A_ADD, RSI, 1);
    alu_ri(A_SUB, RCX, 1);
    test_rr(RCX);
    jcc(CC_NE, parse_loop);
    alu_ri(A_CMP, RBX, 0);
    jcc(CC_GE, parse_done);
    neg_r(RDX);
    bind(parse_done);
    mov_rr(RAX, RDX);
    ret();

    bind(L_BADNUM);
    mov_ri(RSI, str_badnum + 8);
    mov_ri(RDX, sizeof(badnum_msg) - 1);
    jmp(L_RTERR);

    bind(L_INPUTLONG);
    mov_ri(RSI, str_inputlong + 8);
    mov_ri(RDX, sizeof(inputlong_msg) - 1);
    jmp(L_RTERR);

    bind(L_HEAPERR);
    mov_ri(RSI, str_heaperr + 8);
    mov_ri(RDX, sizeof(heaperr_msg) - 1);
    jmp(L_RTERR);

    bind(L_IOERR);
    mov_ri(RAX, 1);
    mov_ri(RDI, 2);
    mov_ri(RSI, str_ioerr + 8);
    mov_ri(RDX, sizeof(ioerr_msg) - 1);
    syscall_();
    mov_ri(RAX, 60);
    mov_ri(RDI, 1);
    syscall_();

    bind(L_OVERFLOW);
    mov_ri(RSI, str_overflow + 8);
    mov_ri(RDX, sizeof(overflow_msg) - 1);
    jmp(L_RTERR);

    bind(L_NEWLIST);
    load_abs(RAX, A_HEAP);
    lea(R8, RAX, 8);
    mov_ri(R9, A_HEAPBASE + A_HEAPSIZE);
    alu_rr(A_CMP, R8, R9);
    jcc(CC_A, L_HEAPERR);
    mov_ri(RDX, 0);
    store(RAX, 0, RDX);
    lea(RDI, RAX, 8);
    store_abs(A_HEAP, RDI);
    ret();

    bind(L_APPEND);
    load_abs(RAX, A_HEAP);
    load(RDX, RDI, 0);
    mov_rr(R8, RDX);
    alu_ri(A_ADD, R8, 1);
    mov_rr(R10, R8);
    shl_ri(R10, 3);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_ADD, R10, 8);
    jcc(CC_B, L_HEAPERR);
    alu_rr(A_ADD, R10, RAX);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_ADD, R10, 7);
    jcc(CC_B, L_HEAPERR);
    alu_ri(A_AND, R10, -8);
    mov_ri(R9, A_HEAPBASE + A_HEAPSIZE);
    alu_rr(A_CMP, R10, R9);
    jcc(CC_A, L_HEAPERR);
    store(RAX, 0, R8);
    push_r(RAX);
    push_r(RDX);
    push_r(RSI);
    push_r(RDI);
    lea(RSI, RDI, 8);
    lea(RDI, RAX, 8);
    mov_rr(RCX, RDX);
    shl_ri(RCX, 3);
    rep_movsb();
    pop_r(RDI);
    pop_r(RSI);
    pop_r(RDX);
    pop_r(RAX);
    lea(RDI, RAX, 8);
    mov_rr(RCX, RDX);
    shl_ri(RCX, 3);
    alu_rr(A_ADD, RDI, RCX);
    store(RDI, 0, RSI);
    alu_ri(A_ADD, RDI, 8);
    store_abs(A_HEAP, RDI);
    ret();

    int get_ok = newlabel();
    bind(L_GET);
    load(RCX, RDI, 0);
    test_rr(RSI);
    jcc(CC_L, L_BOUNDS);
    alu_rr(A_CMP, RSI, RCX);
    jcc(CC_AE, L_BOUNDS);
    lea(RCX, RDI, 8);
    shl_ri(RSI, 3);
    alu_rr(A_ADD, RCX, RSI);
    load(RAX, RCX, 0);
    jmp(get_ok);
    bind(L_BOUNDS);
    mov_ri(RSI, str_bounds + 8);
    mov_ri(RDX, sizeof(bounds_msg) - 1);
    jmp(L_RTERR);
    bind(get_ok);
    ret();
}

typedef struct { char *name; int type, depth, local, base; } Var;

static Var gvars[MAXVARS], lvars[MAXVARS];
static int ngv = 0, nlv = 0, gcells = 0, lcells = 0, infunc = 0;

static char *hn[MAXVARS];
static int hd[MAXVARS], nh = 0;

static void note_depth(const char *name, int d, int line) {
    if (d > MAXDEPTH) die(line, "Historie zu tief (maximal 64)", name);
    for (int i = 0; i < nh; i++)
        if (strcmp(hn[i], name) == 0) { if (d > hd[i]) hd[i] = d; return; }
    if (nh == MAXVARS) die(line, "Zu viele Variablen", NULL);
    hn[nh] = strdup(name);
    hd[nh++] = d;
}

static int depth_of(const char *name) {
    for (int i = 0; i < nh; i++)
        if (strcmp(hn[i], name) == 0) return hd[i];
    return 0;
}

static int parse_history_index(Token *t);

static void prescan(void) {
    for (int i = 0; i + 1 < ntokens; i++) {
        diagnostic_line = tokens[i].line;
        diagnostic_column = tokens[i].column;
        if (tokens[i].type == T_IDENT && tokens[i + 1].type == T_AT) {
            if (tokens[i + 2].type != T_NUMBER)
                die(tokens[i].line, "Nach '" SYM_AT "' muss eine Zahl stehen", NULL);
            note_depth(tokens[i].text, parse_history_index(&tokens[i + 2]), tokens[i].line);
        }
        if (tokens[i].type == T_UNDO && tokens[i + 1].type == T_IDENT)
            note_depth(tokens[i + 1].text, 1, tokens[i].line);
        if (tokens[i].type == T_IDENT &&
            (strcmp(tokens[i].text, KW_CHANGED) == 0 || strcmp(tokens[i].text, KW_CHANGED_DE) == 0) &&
            tokens[i + 1].type == T_LPAREN && tokens[i + 2].type == T_IDENT)
            note_depth(tokens[i + 2].text, 1, tokens[i].line);
    }
}

static Var *find_var(const char *name) {
    Var *t = infunc ? lvars : gvars;
    int n = infunc ? nlv : ngv;
    for (int i = 0; i < n; i++)
        if (strcmp(t[i].name, name) == 0) return &t[i];
    return NULL;
}

static Var *mkvar(const char *name, int type, int depth, int line) {
    Var *v;
    if (name) {
        if ((infunc ? nlv : ngv) == MAXVARS) die(line, "Zu viele Variablen", NULL);
        v = infunc ? &lvars[nlv++] : &gvars[ngv++];
        v->name = strdup(name);
    } else {
        v = calloc(1, sizeof(Var));
    }
    v->type = type;
    v->depth = depth;
    v->local = infunc;
    if (infunc) { v->base = -8 * (lcells + 1); lcells += depth + 1; }
    else { v->base = A_GLOB + 8 * gcells; gcells += depth + 1; }
    return v;
}

static int slot(Var *v, int j) { return v->local ? v->base - 8 * j : v->base + 8 * j; }

static void vload(int reg, Var *v, int j) {
    if (v->local) load(reg, RBP, slot(v, j));
    else load_abs(reg, slot(v, j));
}

static void vstore(Var *v, int j, int reg) {
    if (v->local) store(RBP, slot(v, j), reg);
    else store_abs(slot(v, j), reg);
}

static void valu(int ext, int reg, Var *v, int j) {
    if (v->local) op_rm(ext * 8 + 3, reg, RBP, slot(v, j));
    else op_ra(ext * 8 + 3, reg, slot(v, j));
}

static void vstore_hist(Var *v) {
    for (int j = v->depth; j >= 1; j--) {
        vload(RCX, v, j - 1);
        vstore(v, j, RCX);
    }
    vstore(v, 0, RAX);
}

typedef struct { char *name; int label, nparams, ptype[MAXPARAMS], rtype; } Func;
static Func funcs[MAXFUNCS];
static int nfuncs = 0;
static Func *curfunc = NULL;
static int lret = 0;

static Func *find_func(const char *name) {
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, name) == 0) return &funcs[i];
    return NULL;
}

static struct { int brk, cont; } loops[MAXLOOPS];
static int nloops = 0;

static int pos = 0;

static Token *cur(void) {
    diagnostic_line = tokens[pos].line;
    diagnostic_column = tokens[pos].column;
    return &tokens[pos];
}

static Token *expect(TokType t, const char *what) {
    if (cur()->type != t) die(cur()->line, "Erwartet wurde", what);
    return &tokens[pos++];
}

static void expect_eol(void) {
    if (cur()->type == T_NEWLINE) { pos++; return; }
    if (cur()->type == T_EOF) return;
    die(cur()->line, "Unerwartetes Zeichen am Zeilenende", cur()->text);
}

static void need_int(int t, int line) {
    if (t != TY_INT) die(line, "Hier wird eine Zahl erwartet, kein Text", NULL);
}

static long long parse_integer(Token *t, int negative) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(t->text, &end, 10);
    diagnostic_line = t->line;
    diagnostic_column = t->column;
    unsigned long long limit = (unsigned long long)LLONG_MAX + (negative ? 1ULL : 0ULL);
    if (errno == ERANGE || !end || *end || value > limit)
        die(t->line, "Zahl liegt ausserhalb des erlaubten Bereichs", t->text);
    if (!negative) return (long long)value;
    if (value == (unsigned long long)LLONG_MAX + 1ULL) return LLONG_MIN;
    return -(long long)value;
}

static int parse_history_index(Token *t) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(t->text, &end, 10);
    diagnostic_line = t->line;
    diagnostic_column = t->column;
    if (errno == ERANGE || !end || *end || value > MAXDEPTH)
        die(t->line, "Historienindex muss zwischen 0 und 64 liegen", t->text);
    return (int)value;
}

static void jump_cc(int lbl, int when_true) {
    if (cmp_end >= 0 && clen == cmp_end) {
        int cc = cmp_cc;
        trunc_to(cmp_start);
        jcc(when_true ? cc : (cc ^ 1), lbl);
    } else {
        test_rr(RAX);
        jcc(when_true ? CC_NE : CC_E, lbl);
    }
}

static char *unescape(const char *s, size_t n, size_t *outn, int line) {
    char *r = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 'n': r[k++] = '\n'; break;
                case 't': r[k++] = '\t'; break;
                case 'r': r[k++] = '\r'; break;
                case '0': r[k++] = '\0'; break;
                case '\\': case '"': r[k++] = s[i]; break;
                default: { char tmp[3] = { '\\', s[i], '\0' }; die(line, "Unbekannte Escape-Folge", tmp); }
            }
        } else {
            r[k++] = s[i];
        }
    }
    *outn = k;
    return r;
}

static int parse_expr(void);

static int parse_call(Token *nm) {
    pos += 2;
    if (strcmp(nm->text, KW_INPUT) == 0 || strcmp(nm->text, KW_ASK) == 0) {
        if (cur()->type != T_RPAREN) {
            int t = parse_expr();
            if (t != TY_TEXT) die(nm->line, "'" KW_ASK "' braucht einen Textprompt", NULL);
            call(L_PRTEXT);
            expect(T_RPAREN, "')'");
            call(L_INPUT);
            return TY_TEXT;
        }
        expect(T_RPAREN, "')'");
        call(L_INPUT);
        return TY_TEXT;
    }
    if (strcmp(nm->text, KW_CHANGED) == 0 || strcmp(nm->text, KW_CHANGED_DE) == 0) {
        Token *name = expect(T_IDENT, "Variablenname");
        Var *v = find_var(name->text);
        if (!v) die(name->line, "Unbekannte Variable", name->text);
        expect(T_RPAREN, "')'");
        vload(RDI, v, 0);
        vload(RSI, v, 1);
        if (v->type == TY_TEXT) {
            call(L_STREQ);
            alu_ri(A_XOR, RAX, 1);
        } else {
            mov_rr(RAX, RDI);
            alu_rr(A_CMP, RAX, RSI);
            setcc(CC_NE);
        }
        return TY_INT;
    }
    if (strcmp(nm->text, KW_LIST) == 0) {
        expect(T_RPAREN, "')'");
        call(L_NEWLIST);
        return TY_LIST;
    }
    if (strcmp(nm->text, KW_LEN) == 0) {
        int t = parse_expr();
        if (t != TY_TEXT && t != TY_LIST) die(nm->line, "'" KW_LEN "' braucht Text oder eine Liste", NULL);
        expect(T_RPAREN, "')'");
        load(RAX, RAX, 0);
        return TY_INT;
    }
    if (strcmp(nm->text, KW_STR) == 0) {
        int t = parse_expr();
        expect(T_RPAREN, "')'");
        if (t == TY_INT) call(L_STRINT);
        else if (t != TY_TEXT) die(nm->line, "'" KW_STR "' braucht eine Zahl oder Text", NULL);
        return TY_TEXT;
    }
    if (strcmp(nm->text, KW_NUMBER) == 0) {
        int t = parse_expr();
        if (t != TY_TEXT) die(nm->line, "'" KW_NUMBER "' braucht einen Text", NULL);
        expect(T_RPAREN, "')'");
        call(L_PARSEINT);
        return TY_INT;
    }
    if (strcmp(nm->text, KW_APPEND) == 0) {
        int t = parse_expr();
        if (t != TY_LIST) die(nm->line, "'" KW_APPEND "' braucht zuerst eine Liste", NULL);
        push_r(RAX);
        expect(T_COMMA, "','");
        int t2 = parse_expr();
        if (t2 != TY_INT) die(nm->line, "'" KW_APPEND "' kann nur Zahlen anhaengen", NULL);
        expect(T_RPAREN, "')'");
        mov_rr(RSI, RAX);
        pop_r(RDI);
        call(L_APPEND);
        return TY_LIST;
    }
    if (strcmp(nm->text, KW_GET) == 0) {
        int t = parse_expr();
        if (t != TY_LIST) die(nm->line, "'" KW_GET "' braucht zuerst eine Liste", NULL);
        push_r(RAX);
        expect(T_COMMA, "','");
        int t2 = parse_expr();
        if (t2 != TY_INT) die(nm->line, "'" KW_GET "' braucht einen Zahlenindex", NULL);
        expect(T_RPAREN, "')'");
        mov_rr(RSI, RAX);
        pop_r(RDI);
        call(L_GET);
        return TY_INT;
    }
    Func *f = find_func(nm->text);
    if (!f) die(nm->line, "Unbekannte Funktion", nm->text);
    int n = 0;
    if (cur()->type != T_RPAREN) {
        for (;;) {
            int line = cur()->line;
            int t = parse_expr();
            if (n >= f->nparams) die(line, "Zu viele Argumente fuer", f->name);
            if (t != f->ptype[n]) die(line, "Falscher Typ bei Argument fuer", f->name);
            push_r(RAX);
            n++;
            if (cur()->type == T_COMMA) { pos++; continue; }
            break;
        }
    }
    expect(T_RPAREN, "')'");
    if (n != f->nparams) die(nm->line, "Falsche Anzahl Argumente fuer", f->name);
    call(f->label);
    if (n) alu_ri(A_ADD, RSP, 8 * n);
    return f->rtype;
}

static int parse_primary(void) {
    Token *t = cur();
    if (t->type == T_NUMBER) {
        pos++;
        long long v = parse_integer(t, 0);
        int s = clen;
        mov_ri(RAX, v);
        ls = (Simple){ s, clen, 1, v, NULL, 0 };
        return TY_INT;
    }
    if (t->type == T_STRING) {
        pos++;
        size_t n;
        char *u = unescape(t->text, strlen(t->text), &n, t->line);
        unsigned addr = add_string(u, n);
        free(u);
        mov_ri(RAX, addr);
        return TY_TEXT;
    }
    if (t->type == T_TRUE || t->type == T_FALSE) {
        pos++;
        mov_ri(RAX, t->type == T_TRUE ? 1 : 0);
        return TY_INT;
    }
    if (t->type == T_IDENT || t->type == T_LISTKW) {
        if (tokens[pos + 1].type == T_LPAREN) return parse_call(t);
        if (t->type == T_LISTKW) die(t->line, "Nach '" KW_LIST "' wird '(' erwartet", NULL);
        Var *v = find_var(t->text);
        if (!v) die(t->line, "Unbekannte Variable", t->text);
        pos++;
        int j = 0;
        if (cur()->type == T_AT) {
            pos++;
            Token *n = expect(T_NUMBER, "Zahl nach '" SYM_AT "'");
            j = parse_history_index(n);
        }
        int s = clen;
        vload(RAX, v, j);
        ls = (Simple){ s, clen, 2, 0, v, j };
        return v->type;
    }
    if (t->type == T_LPAREN) {
        pos++;
        int ty = parse_expr();
        expect(T_RPAREN, "')'");
        return ty;
    }
    die(t->line, "Zahl, Text, Variable oder '(' erwartet", t->type == T_EOF ? "Dateiende" : t->text);
    return 0;
}

static int parse_unary(void) {
    if (cur()->type == T_MINUS) {
        int line = cur()->line;
        pos++;
        if (cur()->type == T_NUMBER) {
            long long v = parse_integer(cur(), 1);
            pos++;
            int s = clen;
            mov_ri(RAX, v);
            ls = (Simple){ s, clen, 1, v, NULL, 0 };
            return TY_INT;
        }
        int t = parse_unary();
        need_int(t, line);
        neg_r(RAX);
        return TY_INT;
    }
    return parse_primary();
}

static int simple_valid(int p1) { return ls.kind && ls.start == p1 && ls.end == clen; }

static void alu_simple(int ext, Simple *s) {
    if (s->kind == 1) {
        if (s->imm >= INT32_MIN && s->imm <= INT32_MAX) {
            alu_ri(ext, RAX, (int)s->imm);
        } else {
            mov_ri(RCX, s->imm);
            alu_rr(ext, RAX, RCX);
        }
    } else {
        valu(ext, RAX, s->v, s->j);
    }
}

static void simple_to_rcx(Simple *s) {
    if (s->kind == 1) mov_ri(RCX, s->imm);
    else vload(RCX, s->v, s->j);
}

static int parse_term(void) {
    int t = parse_unary();
    for (;;) {
        int k = cur()->type;
        int line = cur()->line;
        if (k != T_STAR && k != T_SLASH && k != T_MOD) break;
        pos++;
        int p0 = clen;
        push_r(RAX);
        int p1 = clen;
        int t2 = parse_unary();
        if (t != TY_INT || t2 != TY_INT) die(line, "Rechnen geht nur mit Zahlen", NULL);
        int simple = simple_valid(p1);
        Simple s = ls;
        if (simple) trunc_to(p0);
        if (k == T_STAR) {
            if (simple) {
                if (s.kind == 1) { mov_ri(RCX, s.imm); imul_rr(RAX, RCX); }
                else if (s.v && ((Var *)s.v)->local) op_rm(0x0FAF, RAX, RBP, slot(s.v, s.j));
                else op_ra(0x0FAF, RAX, slot(s.v, s.j));
            } else {
                mov_rr(RCX, RAX);
                pop_r(RAX);
                imul_rr(RAX, RCX);
            }
        } else {
            if (simple) simple_to_rcx(&s);
            else { mov_rr(RCX, RAX); pop_r(RAX); }
            if (!(simple && s.kind == 1 && s.imm != 0)) {
                test_rr(RCX);
                jcc(CC_E, L_DIVZ);
            }
            int div_safe = newlabel();
            mov_ri(R8, -1);
            alu_rr(A_CMP, RCX, R8);
            jcc(CC_NE, div_safe);
            mov_ri(R8, LLONG_MIN);
            alu_rr(A_CMP, RAX, R8);
            jcc(CC_E, L_OVERFLOW);
            bind(div_safe);
            cqo();
            idiv_r(RCX);
            if (k == T_MOD) mov_rr(RAX, RDX);
        }
        t = TY_INT;
    }
    return t;
}

static void concat_gen(int t, int t2) {
    if (t == TY_TEXT) {
        if (t2 == TY_INT) call(L_STRINT);
        mov_rr(RSI, RAX);
        pop_r(RDI);
        call(L_CONCAT);
    } else {
        push_r(RAX);
        load(RAX, RSP, 8);
        call(L_STRINT);
        mov_rr(RDI, RAX);
        pop_r(RSI);
        alu_ri(A_ADD, RSP, 8);
        call(L_CONCAT);
    }
}

static int parse_sum(void) {
    int t = parse_term();
    while (cur()->type == T_PLUS || cur()->type == T_MINUS) {
        int k = cur()->type;
        int line = cur()->line;
        pos++;
        int p0 = clen;
        push_r(RAX);
        int p1 = clen;
        int t2 = parse_term();
        if (k == T_PLUS && (t == TY_TEXT || t2 == TY_TEXT)) {
            concat_gen(t, t2);
            t = TY_TEXT;
            continue;
        }
        if (t != TY_INT || t2 != TY_INT) die(line, "Rechnen geht nur mit Zahlen", NULL);
        int ext = k == T_PLUS ? A_ADD : A_SUB;
        if (simple_valid(p1)) {
            Simple s = ls;
            trunc_to(p0);
            alu_simple(ext, &s);
        } else {
            mov_rr(RCX, RAX);
            pop_r(RAX);
            alu_rr(ext, RAX, RCX);
        }
        t = TY_INT;
    }
    return t;
}

static int parse_cmp(void) {
    int t = parse_sum();
    int k = cur()->type;
    int cc;
    switch (k) {
        case T_EQ: cc = CC_E;  break;
        case T_NE: cc = CC_NE; break;
        case T_LT: cc = CC_L;  break;
        case T_GT: cc = CC_G;  break;
        case T_LE: cc = CC_LE; break;
        case T_GE: cc = CC_GE; break;
        default: return t;
    }
    int line = cur()->line;
    pos++;
    int p0 = clen;
    push_r(RAX);
    int p1 = clen;
    int t2 = parse_sum();
    if (t != t2) die(line, "Zahl und Text lassen sich nicht vergleichen", NULL);
    if (t == TY_TEXT) {
        if (k != T_EQ && k != T_NE) die(line, "Text kann nur mit '" SYM_EQ "' und '" SYM_NE "' verglichen werden", NULL);
        mov_rr(RSI, RAX);
        pop_r(RDI);
        call(L_STREQ);
        if (k == T_NE) alu_ri(A_XOR, RAX, 1);
        return TY_INT;
    }
    if (simple_valid(p1)) {
        Simple s = ls;
        trunc_to(p0);
        alu_simple(A_CMP, &s);
    } else {
        mov_rr(RCX, RAX);
        pop_r(RAX);
        alu_rr(A_CMP, RAX, RCX);
    }
    cmp_start = clen;
    cmp_cc = cc;
    setcc(cc);
    cmp_end = clen;
    return TY_INT;
}

static int parse_not(void) {
    if (cur()->type == T_NOT) {
        int line = cur()->line;
        pos++;
        int t = parse_not();
        need_int(t, line);
        test_rr(RAX);
        cmp_start = clen;
        cmp_cc = CC_E;
        setcc(CC_E);
        cmp_end = clen;
        return TY_INT;
    }
    return parse_cmp();
}

static int parse_and(void) {
    int line = cur()->line;
    int t = parse_not();
    if (cur()->type != T_AND) return t;
    need_int(t, line);
    int lf = newlabel(), le = newlabel();
    jump_cc(lf, 0);
    while (cur()->type == T_AND) {
        pos++;
        line = cur()->line;
        need_int(parse_not(), line);
        jump_cc(lf, 0);
    }
    mov_ri(RAX, 1);
    jmp(le);
    bind(lf);
    mov_ri(RAX, 0);
    bind(le);
    return TY_INT;
}

static int parse_expr(void) {
    int line = cur()->line;
    int t = parse_and();
    if (cur()->type != T_OR) return t;
    need_int(t, line);
    int lt = newlabel(), le = newlabel();
    jump_cc(lt, 1);
    while (cur()->type == T_OR) {
        pos++;
        line = cur()->line;
        need_int(parse_and(), line);
        jump_cc(lt, 1);
    }
    mov_ri(RAX, 0);
    jmp(le);
    bind(lt);
    mov_ri(RAX, 1);
    bind(le);
    return TY_INT;
}

static void parse_block(int top);

static void parse_cond(int lfalse) {
    int line = cur()->line;
    need_int(parse_expr(), line);
    jump_cc(lfalse, 0);
}

static void parse_if(void) {
    int lend = newlabel();
    for (;;) {
        int lnext = newlabel();
        parse_cond(lnext);
        expect_eol();
        parse_block(0);
        if (cur()->type == T_ELSE) {
            jmp(lend);
            bind(lnext);
            pos++;
            if (cur()->type == T_IF) { pos++; continue; }
            expect_eol();
            parse_block(0);
            expect(T_END, "'" KW_END "'");
            break;
        }
        bind(lnext);
        expect(T_END, "'" KW_END "'");
        break;
    }
    bind(lend);
    expect_eol();
}

static int parse_type(void) {
    if (cur()->type == T_INTKW) { pos++; return TY_INT; }
    if (cur()->type == T_TEXTKW) { pos++; return TY_TEXT; }
    if (cur()->type == T_LISTKW) { pos++; return TY_LIST; }
    die(cur()->line, "Erwartet wurde", "'" KW_INT "', '" KW_TEXT "' oder '" KW_LIST "'");
    return 0;
}

static void parse_func(void) {
    Token *kw = cur();
    if (infunc) die(kw->line, "Funktionen koennen nicht verschachtelt werden", NULL);
    pos++;
    Token *nm = expect(T_IDENT, "Funktionsname");
    if (find_func(nm->text) || strcmp(nm->text, KW_LEN) == 0 || strcmp(nm->text, KW_STR) == 0 ||
        strcmp(nm->text, KW_INPUT) == 0 || strcmp(nm->text, KW_NUMBER) == 0 ||
        strcmp(nm->text, KW_APPEND) == 0 || strcmp(nm->text, KW_GET) == 0 ||
        strcmp(nm->text, KW_ASK) == 0 || strcmp(nm->text, KW_CHANGED) == 0 ||
        strcmp(nm->text, KW_CHANGED_DE) == 0)
        die(nm->line, "Funktion gibt es schon", nm->text);
    if (nfuncs == MAXFUNCS) die(nm->line, "Zu viele Funktionen", NULL);
    Func *f = &funcs[nfuncs++];
    f->name = strdup(nm->text);
    f->label = newlabel();
    f->rtype = TY_INT;
    f->nparams = 0;
    Token *pnames[MAXPARAMS];
    expect(T_LPAREN, "'('");
    if (cur()->type != T_RPAREN) {
        for (;;) {
            Token *p = expect(T_IDENT, "Parametername");
            if (f->nparams == MAXPARAMS) die(p->line, "Zu viele Parameter", NULL);
            int ty = TY_INT;
            if (cur()->type == T_AS) { pos++; ty = parse_type(); }
            pnames[f->nparams] = p;
            f->ptype[f->nparams++] = ty;
            if (cur()->type == T_COMMA) { pos++; continue; }
            break;
        }
    }
    expect(T_RPAREN, "')'");
    if (cur()->type == T_AS) { pos++; f->rtype = parse_type(); }
    expect_eol();

    int skip = newlabel();
    jmp(skip);
    bind(f->label);
    infunc = 1;
    curfunc = f;
    nlv = 0;
    lcells = 0;
    lret = newlabel();
    push_r(RBP);
    mov_rr(RBP, RSP);
    eb(0xB9);
    int patch = clen;
    e32(0);
    int lz = newlabel(), ll = newlabel();
    test_rr(RCX);
    jcc(CC_E, lz);
    bind(ll);
    eb(0x6A); eb(0);
    alu_ri(A_SUB, RCX, 1);
    jcc(CC_NE, ll);
    bind(lz);

    for (int i = 0; i < f->nparams; i++) {
        if (find_var(pnames[i]->text)) die(pnames[i]->line, "Parameter doppelt", pnames[i]->text);
        int disp = 16 + 8 * (f->nparams - 1 - i);
        int d = depth_of(pnames[i]->text);
        if (d > 0) {
            Var *v = mkvar(pnames[i]->text, f->ptype[i], d, pnames[i]->line);
            load(RAX, RBP, disp);
            vstore(v, 0, RAX);
        } else {
            if (nlv == MAXVARS) die(pnames[i]->line, "Zu viele Variablen", NULL);
            Var *v = &lvars[nlv++];
            v->name = strdup(pnames[i]->text);
            v->type = f->ptype[i];
            v->depth = 0;
            v->local = 1;
            v->base = disp;
        }
    }

    parse_block(0);
    expect(T_END, "'" KW_END "'");
    if (f->rtype == TY_INT) mov_ri(RAX, 0);
    else if (f->rtype == TY_TEXT) mov_ri(RAX, str_empty);
    else call(L_NEWLIST);
    bind(lret);
    leave();
    ret();
    for (int i = 0; i < 4; i++) code[patch + i] = (unsigned char)(((unsigned)lcells >> (8 * i)) & 255);
    bind(skip);
    infunc = 0;
    curfunc = NULL;
    expect_eol();
}

static void parse_statement(void) {
    Token *t = cur();

    if (t->type == T_PRINT) {
        pos++;
        if (cur()->type != T_NEWLINE && cur()->type != T_EOF) {
            for (;;) {
                int ty = parse_expr();
                if (ty == TY_LIST) die(t->line, "Listen koennen nicht direkt ausgegeben werden", NULL);
                call(ty == TY_TEXT ? L_PRTEXT : L_PRINT);
                if (cur()->type == T_COMMA) { pos++; continue; }
                break;
            }
        }
        mov_ri(RAX, str_nl);
        call(L_PRTEXT);
        expect_eol();
    }
    else if (t->type == T_IF) {
        pos++;
        parse_if();
    }
    else if (t->type == T_LOOP) {
        pos++;
        int top = newlabel(), end = newlabel();
        bind(top);
        parse_cond(end);
        expect_eol();
        if (nloops == MAXLOOPS) die(t->line, "Schleifen zu tief verschachtelt", NULL);
        loops[nloops].brk = end;
        loops[nloops++].cont = top;
        parse_block(0);
        nloops--;
        expect(T_END, "'" KW_END "'");
        jmp(top);
        bind(end);
        expect_eol();
    }
    else if (t->type == T_REPEAT) {
        pos++;
        int line = cur()->line;
        need_int(parse_expr(), line);
        Var *lim = mkvar(NULL, TY_INT, 0, t->line);
        vstore(lim, 0, RAX);
        Var *ctr;
        if (cur()->type == T_AS) {
            pos++;
            Token *name = expect(T_IDENT, "Variablenname nach '" KW_AS "'");
            ctr = find_var(name->text);
            if (!ctr) ctr = mkvar(name->text, TY_INT, depth_of(name->text), name->line);
            else if (ctr->type != TY_INT) die(name->line, "Zaehler muss eine Zahl sein", name->text);
        } else {
            ctr = mkvar(NULL, TY_INT, 0, t->line);
        }
        expect_eol();
        mov_ri(RAX, 1);
        vstore_hist(ctr);
        int top = newlabel(), nxt = newlabel(), end = newlabel();
        bind(top);
        vload(RAX, ctr, 0);
        valu(A_CMP, RAX, lim, 0);
        jcc(CC_G, end);
        if (nloops == MAXLOOPS) die(t->line, "Schleifen zu tief verschachtelt", NULL);
        loops[nloops].brk = end;
        loops[nloops++].cont = nxt;
        parse_block(0);
        nloops--;
        expect(T_END, "'" KW_END "'");
        bind(nxt);
        vload(RAX, ctr, 0);
        alu_ri(A_ADD, RAX, 1);
        vstore_hist(ctr);
        jmp(top);
        bind(end);
        expect_eol();
    }
    else if (t->type == T_STOP || t->type == T_NEXT) {
        if (!nloops) die(t->line, "Nur innerhalb einer Schleife erlaubt", t->text);
        pos++;
        jmp(t->type == T_STOP ? loops[nloops - 1].brk : loops[nloops - 1].cont);
        expect_eol();
    }
    else if (t->type == T_FUNC) {
        parse_func();
    }
    else if (t->type == T_GIVE) {
        if (!infunc) die(t->line, "'" KW_GIVE "' nur innerhalb einer Funktion", NULL);
        pos++;
        int line = cur()->line;
        int ty = parse_expr();
        if (ty != curfunc->rtype) die(line, "Falscher Typ bei '" KW_GIVE "'", curfunc->name);
        jmp(lret);
        expect_eol();
    }
    else if (t->type == T_UNDO) {
        pos++;
        Token *name = expect(T_IDENT, "Variablenname");
        Var *v = find_var(name->text);
        if (!v) die(name->line, "Unbekannte Variable", name->text);
        for (int j = 0; j < v->depth; j++) {
            vload(RCX, v, j + 1);
            vstore(v, j, RCX);
        }
        mov_ri(RCX, 0);
        vstore(v, v->depth, RCX);
        expect_eol();
    }
    else {
        int startpos = pos;
        int ty = parse_expr();
        if (cur()->type == T_ARROW) {
            pos++;
            Token *name = expect(T_IDENT, "Variablenname");
            Var *v = find_var(name->text);
            if (!v) v = mkvar(name->text, ty, depth_of(name->text), name->line);
            else if (v->type != ty) die(name->line, "Variable hat einen anderen Typ", name->text);
            vstore_hist(v);
            expect_eol();
        } else if ((t->type == T_IDENT || t->type == T_LISTKW) &&
                   tokens[startpos + 1].type == T_LPAREN) {
            expect_eol();
        } else {
            die(cur()->line, "Erwartet wurde", "'" SYM_ARROW "' (zum Beispiel: 5 " SYM_ARROW " x)");
        }
    }
}

static void parse_block(int top) {
    for (;;) {
        TokType ty = cur()->type;
        if (ty == T_NEWLINE) { pos++; continue; }
        if (ty == T_EOF) {
            if (top) return;
            if (pos > 0 && tokens[pos - 1].type == T_NEWLINE) {
                diagnostic_line = tokens[pos - 1].line;
                diagnostic_column = tokens[pos - 1].column;
                die(diagnostic_line, "Dateiende erreicht, '" KW_END "' fehlt", NULL);
            }
            die(cur()->line, "Dateiende erreicht, '" KW_END "' fehlt", NULL);
        }
        if (ty == T_END || ty == T_ELSE) return;
        parse_statement();
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); fclose(f); exit(1); }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { perror(path); fclose(f); exit(1); }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fprintf(stderr, "Kein Speicher\n"); fclose(f); exit(1); }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n || ferror(f)) {
        fprintf(stderr, "Lesefehler: %s\n", path);
        fclose(f);
        free(buf);
        exit(1);
    }
    if (memchr(buf, '\0', (size_t)n)) {
        fprintf(stderr, "%s: Fehler: Quelldatei enthaelt ein Nullbyte\n", path);
        fclose(f);
        free(buf);
        exit(1);
    }
    buf[n] = '\0';
    if (fclose(f) != 0) { perror(path); free(buf); exit(1); }
    return buf;
}

static void write_bytes(FILE *f, const void *data, size_t size, const char *path) {
    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Schreibfehler: %s\n", path);
        fclose(f);
        exit(1);
    }
}

static void write_elf(const char *path) {
    for (int i = 0; i < nfix; i++) {
        int target = lpos[fixes[i].label];
        if (target < 0) { fprintf(stderr, "Interner Fehler: Sprungziel fehlt\n"); exit(1); }
        int rel = target - (fixes[i].pos + 4);
        for (int k = 0; k < 4; k++) code[fixes[i].pos + k] = (unsigned char)(((unsigned)rel >> (8 * k)) & 255);
    }

    unsigned long long hdr = sizeof(Elf64_Ehdr) + 4 * sizeof(Elf64_Phdr);
    unsigned long long codeend = hdr + clen;
    unsigned long long off2 = (codeend + 4095) & ~4095ULL;

    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_entry = A_CODE + hdr;
    eh.e_phoff = sizeof eh;
    eh.e_ehsize = sizeof eh;
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 4;
    eh.e_shentsize = sizeof(Elf64_Shdr);

    Elf64_Phdr ph[4];
    memset(ph, 0, sizeof ph);
    ph[0] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_X, 0, A_CODE, A_CODE, codeend, codeend, 4096 };
    ph[1] = (Elf64_Phdr){ PT_LOAD, PF_R, off2, A_STR, A_STR, (unsigned long long)slen, (unsigned long long)slen, 4096 };
    ph[2] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_W, off2, A_DATA, A_DATA, 0,
                          (unsigned long long)(A_GLOB - A_DATA) + 8ULL * gcells, 4096 };
    ph[3] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_W, off2, A_HEAPBASE, A_HEAPBASE, 0, A_HEAPSIZE, 4096 };

    FILE *o = fopen(path, "wb");
    if (!o) { perror(path); exit(1); }
    write_bytes(o, &eh, sizeof eh, path);
    write_bytes(o, ph, sizeof ph, path);
    write_bytes(o, code, (size_t)clen, path);
    for (unsigned long long i = codeend; i < off2; i++) {
        if (fputc(0, o) == EOF) {
            fprintf(stderr, "Schreibfehler: %s\n", path);
            fclose(o);
            exit(1);
        }
    }
    write_bytes(o, sdata, (size_t)slen, path);
    if (fclose(o) != 0) { perror(path); exit(1); }
    if (chmod(path, 0755) != 0) { perror(path); exit(1); }
}

int main(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int run = 0;

    int options = 1;
    for (int i = 1; i < argc; i++) {
        if (options && strcmp(argv[i], "--") == 0) {
            options = 0;
        } else if (options && strcmp(argv[i], "--help") == 0) {
            printf("Benutzung: %s [--run] [-o ausgabe] datei.apx\n", argv[0]);
            return 0;
        } else if (options && strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fehler: Nach '-o' fehlt der Ausgabepfad\n");
                return 1;
            }
            if (output) {
                fprintf(stderr, "Fehler: '-o' darf nur einmal angegeben werden\n");
                return 1;
            }
            output = argv[++i];
        } else if (options && strcmp(argv[i], "--run") == 0) {
            run = 1;
        } else if (options && argv[i][0] == '-') {
            fprintf(stderr, "Unbekannte Option: %s\n", argv[i]);
            return 1;
        } else if (input) {
            fprintf(stderr, "Fehler: Es darf nur eine Quelldatei angegeben werden\n");
            return 1;
        } else {
            input = argv[i];
        }
    }
    if (!input) {
        fprintf(stderr, "Benutzung: %s [--run] [-o ausgabe] datei.apx\n", argv[0]);
        return 1;
    }
    srcname = input;

    char *outname;
    if (output) {
        outname = strdup(output);
    } else {
        outname = strdup(input);
        char *dot = strrchr(outname, '.');
        char *slash = strrchr(outname, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';
        else { free(outname); outname = fmt("%s.out", input); }
    }
    if (!outname) { fprintf(stderr, "Kein Speicher\n"); return 1; }

    char *src = read_file(input);
    source_text = src;
    lex(src);
    prescan();

    gen_runtime();
    str_empty = add_string("", 0);
    str_nl = add_string("\n", 1);

    bind(L_MAIN);
    mov_ri(RAX, A_HEAPBASE);
    store_abs(A_HEAP, RAX);

    parse_block(1);
    if (cur()->type != T_EOF) die(cur()->line, "Unerwartetes Schluesselwort", cur()->text);

    call(L_FLUSH);
    mov_ri(RAX, 60);
    mov_ri(RDI, 0);
    syscall_();

    write_elf(outname);

    if (run) {
        char *path = strchr(outname, '/') ? strdup(outname) : fmt("./%s", outname);
        if (!path) { fprintf(stderr, "Kein Speicher\n"); free(outname); return 1; }
        char *args[] = { path, NULL };
        execv(path, args);
        perror(path);
        free(path);
        free(outname);
        return 1;
    }
    printf("Fertig: %s%s\n", outname[0] == '/' ? "" : "./", outname);
    free(outname);
    return 0;
}
