// ENG-3.C — the in-engine SM83 assembler. A table-driven, two-pass encoder for the standard Sharp
// LR35902 instruction set. Pass 1 walks the source assigning each label its byte offset (instruction
// lengths follow from operand SHAPE, never operand value); pass 2 encodes for real, with every label
// now known, resolving symbol references and signed-relative `jr` displacements.
//
// Correctness is checkable byte-for-byte: every encoding below matches the published SM83 opcode map,
// and the unit tests assert golden bytes per instruction category + the engine's own RNG presets.
#include "src/vm/gameboy/sm83_assembler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gbcpp::vm {

namespace {

// ── small text helpers ────────────────────────────────────────────────────────────────────────
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Remove all whitespace (operands are matched in a space-insensitive normalized form, so
// `sp + 5` and `[ hl ]` parse identically to `sp+5` / `[hl]`).
std::string despace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

[[noreturn]] void fail(std::size_t line, const std::string& msg) {
    std::ostringstream os;
    os << "SM83 assembler, line " << line << ": " << msg;
    throw std::runtime_error(os.str());
}

// ── operand classification ────────────────────────────────────────────────────────────────────
// The 3-bit r field of the standard register-operand tables: B C D E H L [HL] A.
int reg8(const std::string& t) {
    if (t == "b") return 0;
    if (t == "c") return 1;
    if (t == "d") return 2;
    if (t == "e") return 3;
    if (t == "h") return 4;
    if (t == "l") return 5;
    if (t == "[hl]") return 6;
    if (t == "a") return 7;
    return -1;
}

// The 2-bit register-pair field. `af` replaces `sp` only for push/pop (selected by allowAf).
int reg16(const std::string& t, bool allowAf) {
    if (t == "bc") return 0;
    if (t == "de") return 1;
    if (t == "hl") return 2;
    if (!allowAf && t == "sp") return 3;
    if (allowAf && t == "af") return 3;
    return -1;
}

// The 2-bit condition field: NZ Z NC C.
int cond(const std::string& t) {
    if (t == "nz") return 0;
    if (t == "z") return 1;
    if (t == "nc") return 2;
    if (t == "c") return 3;
    return -1;
}

bool isMem(const std::string& t) { return t.size() >= 2 && t.front() == '[' && t.back() == ']'; }
std::string memInner(const std::string& t) { return t.substr(1, t.size() - 2); }

// ── expression evaluation ─────────────────────────────────────────────────────────────────────
// A resolver maps a symbol name to a value, or nullopt if unknown. Pass 1 uses a lenient resolver
// (unknown → 0) so instruction SIZES — which never depend on a value — are computed even before
// labels are defined; pass 2 uses a strict resolver that errors on an unknown symbol.
using Resolver = std::optional<std::uint32_t> (*)(const std::string&, const SymbolTable&);

std::int64_t evalTerm(const std::string& term, std::size_t line, const SymbolTable& syms,
                      Resolver resolve, bool lenient) {
    if (term.empty()) fail(line, "empty expression term");
    if (term[0] == '$') {
        return static_cast<std::int64_t>(std::stoll(term.substr(1), nullptr, 16));
    }
    if (term[0] == '%') {
        return static_cast<std::int64_t>(std::stoll(term.substr(1), nullptr, 2));
    }
    if (std::isdigit(static_cast<unsigned char>(term[0]))) {
        return static_cast<std::int64_t>(std::stoll(term, nullptr, 10));
    }
    // A symbol (label or predefined constant).
    if (const std::optional<std::uint32_t> v = resolve(term, syms)) {
        return static_cast<std::int64_t>(*v);
    }
    if (lenient) return 0;
    fail(line, "unknown symbol '" + term + "'");
}

// Evaluate an operand expression: a term, optionally with one trailing +/- term (covers `label+2`,
// `$FF00-1`). Leading sign is honoured (`-2`).
std::int64_t evalExpr(const std::string& expr, std::size_t line, const SymbolTable& syms,
                      Resolver resolve, bool lenient) {
    std::string e = despace(expr);
    if (e.empty()) fail(line, "empty expression");
    bool neg = false;
    std::size_t i = 0;
    if (e[0] == '+' || e[0] == '-') {
        neg = e[0] == '-';
        i = 1;
    }
    // Find a top-level +/- separating two terms (search from index 1 so a leading sign isn't it).
    std::size_t split = std::string::npos;
    for (std::size_t j = std::max<std::size_t>(i, 1); j < e.size(); ++j) {
        if (e[j] == '+' || e[j] == '-') { split = j; break; }
    }
    if (split == std::string::npos) {
        const std::int64_t v = evalTerm(e.substr(i), line, syms, resolve, lenient);
        return neg ? -v : v;
    }
    const std::int64_t left = evalTerm(e.substr(i, split - i), line, syms, resolve, lenient);
    const std::int64_t right = evalTerm(e.substr(split + 1), line, syms, resolve, lenient);
    const std::int64_t l = neg ? -left : left;
    return e[split] == '+' ? l + right : l - right;
}

std::uint8_t imm8(std::int64_t v, std::size_t line) {
    if (v < -128 || v > 255) fail(line, "8-bit value out of range: " + std::to_string(v));
    return static_cast<std::uint8_t>(v & 0xFF);
}

std::uint8_t rel8(std::int64_t target, std::int64_t afterInstr, std::size_t line) {
    const std::int64_t d = target - afterInstr;
    if (d < -128 || d > 127) fail(line, "jr target out of range (" + std::to_string(d) + " bytes)");
    return static_cast<std::uint8_t>(d & 0xFF);
}

// ── one instruction → bytes ───────────────────────────────────────────────────────────────────
// addr is the byte offset of this instruction (for jr relative resolution). Emits into `out`.
void encode(const std::string& mnem, const std::vector<std::string>& ops, std::size_t addr,
            std::size_t line, const SymbolTable& syms, Resolver resolve, bool lenient,
            std::vector<std::uint8_t>& out) {
    auto eval = [&](const std::string& s) { return evalExpr(s, line, syms, resolve, lenient); };
    auto emit = [&](std::initializer_list<int> bytes) {
        for (int b : bytes) out.push_back(static_cast<std::uint8_t>(b & 0xFF));
    };
    auto need = [&](std::size_t n) {
        if (ops.size() != n) fail(line, mnem + ": expected " + std::to_string(n) + " operand(s)");
    };

    // ALU A-implicit ops: `op r` / `op a,r` / `op [hl]` / `op n` / `op a,n`.
    auto alu = [&](int baseReg, int immOp) {
        std::string rhs = ops.size() == 2 ? ops[1] : ops[0];
        if (ops.size() == 2 && ops[0] != "a") fail(line, mnem + ": first operand must be A");
        if (ops.size() != 1 && ops.size() != 2) fail(line, mnem + ": wrong operand count");
        if (const int r = reg8(rhs); r >= 0) {
            emit({baseReg | r});
        } else {
            emit({immOp, imm8(eval(rhs), line)});
        }
    };

    // ── 8-bit / 16-bit loads ──
    if (mnem == "ld") {
        need(2);
        const std::string& d = ops[0];
        const std::string& s = ops[1];
        // ld r,r' | ld r,[hl] | ld [hl],r | ld r,n | ld [hl],n  (via the unified r-table, [hl]=6)
        const int dr = reg8(d), sr = reg8(s);
        if (dr >= 0 && sr >= 0) {
            if (dr == 6 && sr == 6) fail(line, "ld [hl],[hl] is not an instruction (that opcode is halt)");
            emit({0x40 | (dr << 3) | sr});
            return;
        }
        if (dr >= 0 && sr < 0 && !isMem(s) && reg16(s, false) < 0) {
            emit({0x06 | (dr << 3), imm8(eval(s), line)});  // ld r,n
            return;
        }
        // ld a,[bc]/[de]/[hl+]/[hl-]/[nn] ; ld [bc]/[de]/[hl+]/[hl-]/[nn],a
        if (d == "a" && isMem(s)) {
            const std::string m = memInner(s);
            if (m == "bc") { emit({0x0A}); return; }
            if (m == "de") { emit({0x1A}); return; }
            if (m == "hl+" || m == "hli") { emit({0x2A}); return; }
            if (m == "hl-" || m == "hld") { emit({0x3A}); return; }
            if (m == "c" || m == "$ff00+c") { emit({0xF2}); return; }  // ld a,[$FF00+C]
            { const std::int64_t v = eval(m);
              emit({0xFA, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return; }
        }
        if (s == "a" && isMem(d)) {
            const std::string m = memInner(d);
            if (m == "bc") { emit({0x02}); return; }
            if (m == "de") { emit({0x12}); return; }
            if (m == "hl+" || m == "hli") { emit({0x22}); return; }
            if (m == "hl-" || m == "hld") { emit({0x32}); return; }
            if (m == "c" || m == "$ff00+c") { emit({0xE2}); return; }  // ld [$FF00+C],a
            { const std::int64_t v = eval(m);
              emit({0xEA, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return; }
        }
        if (d == "sp" && s == "hl") { emit({0xF9}); return; }               // ld sp,hl
        // ld rp,nn  (and the ld hl,sp+e special)
        if (const int rp = reg16(d, false); rp >= 0 && !isMem(s)) {
            if (d == "hl" && (s.rfind("sp+", 0) == 0 || s.rfind("sp-", 0) == 0)) {
                const std::int64_t e = eval(s.substr(2));                     // ld hl,sp+e
                emit({0xF8, imm8(e, line)}); return;
            }
            const std::int64_t v = eval(s);
            emit({0x01 | (rp << 4), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        // ld [nn],sp
        if (isMem(d) && s == "sp") {
            const std::int64_t v = eval(memInner(d));
            emit({0x08, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        fail(line, "ld: unsupported operands '" + d + "', '" + s + "'");
    }

    if (mnem == "ldh") {
        need(2);
        const std::string& d = ops[0];
        const std::string& s = ops[1];
        auto highByte = [&](const std::string& m) -> std::uint8_t {
            const std::int64_t v = eval(m);
            const std::int64_t lo = (v >= 0xFF00 && v <= 0xFFFF) ? (v - 0xFF00) : v;
            if (lo < 0 || lo > 0xFF) fail(line, "ldh address out of high page: " + m);
            return static_cast<std::uint8_t>(lo);
        };
        if (d == "a" && isMem(s)) {
            const std::string m = memInner(s);
            if (m == "c") { emit({0xF2}); return; }
            emit({0xF0, highByte(m)}); return;
        }
        if (s == "a" && isMem(d)) {
            const std::string m = memInner(d);
            if (m == "c") { emit({0xE2}); return; }
            emit({0xE0, highByte(m)}); return;
        }
        fail(line, "ldh: operands must be a,[n] or [n],a");
    }

    // ── 8-bit ALU ──
    if (mnem == "add") {
        // add hl,rp | add sp,e | add a,r | add r | add n
        if (ops.size() == 2 && ops[0] == "hl") {
            const int rp = reg16(ops[1], false);
            if (rp < 0) fail(line, "add hl,rp: bad register pair");
            emit({0x09 | (rp << 4)}); return;
        }
        if (ops.size() == 2 && ops[0] == "sp") { emit({0xE8, imm8(eval(ops[1]), line)}); return; }
        alu(0x80, 0xC6); return;
    }
    if (mnem == "adc") { alu(0x88, 0xCE); return; }
    if (mnem == "sub") { alu(0x90, 0xD6); return; }
    if (mnem == "sbc") { alu(0x98, 0xDE); return; }
    if (mnem == "and") { alu(0xA0, 0xE6); return; }
    if (mnem == "xor") { alu(0xA8, 0xEE); return; }
    if (mnem == "or")  { alu(0xB0, 0xF6); return; }
    if (mnem == "cp")  { alu(0xB8, 0xFE); return; }

    // ── inc / dec ──
    if (mnem == "inc" || mnem == "dec") {
        need(1);
        if (const int r = reg8(ops[0]); r >= 0) {
            emit({(mnem == "inc" ? 0x04 : 0x05) | (r << 3)}); return;
        }
        if (const int rp = reg16(ops[0], false); rp >= 0) {
            emit({(mnem == "inc" ? 0x03 : 0x0B) | (rp << 4)}); return;
        }
        fail(line, mnem + ": bad operand '" + ops[0] + "'");
    }

    // ── rotates (A) / misc single-byte ──
    static const std::array<std::pair<const char*, int>, 13> kSimple{{
        {"nop", 0x00}, {"halt", 0x76}, {"di", 0xF3}, {"ei", 0xFB},
        {"ccf", 0x3F}, {"scf", 0x37}, {"cpl", 0x2F}, {"daa", 0x27},
        {"rlca", 0x07}, {"rrca", 0x0F}, {"rla", 0x17}, {"rra", 0x1F},
        {"reti", 0xD9}}};
    for (const auto& [name, op] : kSimple) {
        if (mnem == name) { need(0); emit({op}); return; }
    }
    if (mnem == "stop") { need(0); emit({0x10, 0x00}); return; }

    // ── jumps / calls / returns ──
    if (mnem == "jp") {
        if (ops.size() == 1 && (ops[0] == "hl" || ops[0] == "[hl]")) { emit({0xE9}); return; }
        if (ops.size() == 1) {
            const std::int64_t v = eval(ops[0]);
            emit({0xC3, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        if (ops.size() == 2) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "jp: bad condition '" + ops[0] + "'");
            const std::int64_t v = eval(ops[1]);
            emit({0xC2 | (cc << 3), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        fail(line, "jp: wrong operand count");
    }
    if (mnem == "jr") {
        std::string targetExpr;
        int op0 = 0x18, cc = -1;
        if (ops.size() == 1) { targetExpr = ops[0]; }
        else if (ops.size() == 2) {
            cc = cond(ops[0]);
            if (cc < 0) fail(line, "jr: bad condition '" + ops[0] + "'");
            op0 = 0x20 | (cc << 3);
            targetExpr = ops[1];
        } else { fail(line, "jr: wrong operand count"); }
        const std::int64_t target = eval(targetExpr);
        emit({op0, rel8(target, static_cast<std::int64_t>(addr) + 2, line)});
        return;
    }
    if (mnem == "call") {
        if (ops.size() == 1) {
            const std::int64_t v = eval(ops[0]);
            emit({0xCD, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        if (ops.size() == 2) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "call: bad condition '" + ops[0] + "'");
            const std::int64_t v = eval(ops[1]);
            emit({0xC4 | (cc << 3), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        fail(line, "call: wrong operand count");
    }
    if (mnem == "ret") {
        if (ops.empty()) { emit({0xC9}); return; }
        if (ops.size() == 1) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "ret: bad condition '" + ops[0] + "'");
            emit({0xC0 | (cc << 3)}); return;
        }
        fail(line, "ret: wrong operand count");
    }
    if (mnem == "rst") {
        need(1);
        const std::int64_t v = eval(ops[0]);
        if (v < 0 || v > 0x38 || (v & 0x07) != 0) fail(line, "rst: vector must be one of $00..$38 step 8");
        emit({0xC7 | static_cast<int>(v)}); return;
    }

    // ── push / pop ──
    if (mnem == "push" || mnem == "pop") {
        need(1);
        const int rp = reg16(ops[0], /*allowAf=*/true);
        if (rp < 0) fail(line, mnem + ": bad register pair '" + ops[0] + "'");
        emit({(mnem == "push" ? 0xC5 : 0xC1) | (rp << 4)}); return;
    }

    // ── CB-prefixed: rotates/shifts and bit/res/set ──
    static const std::array<std::pair<const char*, int>, 8> kCbShift{{
        {"rlc", 0x00}, {"rrc", 0x08}, {"rl", 0x10}, {"rr", 0x18},
        {"sla", 0x20}, {"sra", 0x28}, {"swap", 0x30}, {"srl", 0x38}}};
    for (const auto& [name, base] : kCbShift) {
        if (mnem == name) {
            need(1);
            const int r = reg8(ops[0]);
            if (r < 0) fail(line, mnem + ": bad register '" + ops[0] + "'");
            emit({0xCB, base | r}); return;
        }
    }
    if (mnem == "bit" || mnem == "res" || mnem == "set") {
        need(2);
        const std::int64_t b = eval(ops[0]);
        if (b < 0 || b > 7) fail(line, mnem + ": bit index must be 0..7");
        const int r = reg8(ops[1]);
        if (r < 0) fail(line, mnem + ": bad register '" + ops[1] + "'");
        const int base = mnem == "bit" ? 0x40 : (mnem == "res" ? 0x80 : 0xC0);
        emit({0xCB, base | (static_cast<int>(b) << 3) | r}); return;
    }

    fail(line, "unknown mnemonic '" + mnem + "'");
}

// A parsed source line: any labels it defines + an optional instruction (mnemonic + operands).
struct Line {
    std::size_t              number;
    std::vector<std::string> labels;
    std::string              mnemonic;     // empty = label-only / blank line
    std::vector<std::string> operands;
};

std::vector<Line> parseLines(std::string_view source) {
    std::vector<Line> lines;
    std::size_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= source.size()) {
        const std::size_t nl = source.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? source.size() : nl;
        ++lineNo;
        std::string raw(source.substr(pos, end - pos));
        pos = end + 1;

        if (const std::size_t c = raw.find(';'); c != std::string::npos) raw.erase(c);  // strip comment
        std::string text = trim(raw);

        Line ln;
        ln.number = lineNo;
        // Leading labels: `name:` (possibly several, possibly sharing a line with an instruction).
        for (;;) {
            const std::size_t colon = text.find(':');
            if (colon == std::string::npos) break;
            std::string label = trim(text.substr(0, colon));
            // A colon that's part of an operand (none in our grammar) would break here; labels are
            // simple identifiers, so require the head to be label-shaped.
            if (label.empty() ||
                !(std::isalpha(static_cast<unsigned char>(label[0])) || label[0] == '_' || label[0] == '.')) {
                break;
            }
            ln.labels.push_back(lower(label));
            text = trim(text.substr(colon + 1));
        }
        if (!text.empty()) {
            // mnemonic = first whitespace-delimited token; the rest splits on commas.
            std::size_t sp = 0;
            while (sp < text.size() && !std::isspace(static_cast<unsigned char>(text[sp]))) ++sp;
            ln.mnemonic = lower(text.substr(0, sp));
            std::string rest = trim(text.substr(sp));
            std::size_t start = 0;
            while (start <= rest.size() && !rest.empty()) {
                const std::size_t comma = rest.find(',', start);
                const std::size_t e = comma == std::string::npos ? rest.size() : comma;
                ln.operands.push_back(lower(despace(rest.substr(start, e - start))));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (!ln.labels.empty() || !ln.mnemonic.empty()) lines.push_back(std::move(ln));
    }
    return lines;
}

std::optional<std::uint32_t> resolveStrict(const std::string& name, const SymbolTable& syms) {
    const auto it = syms.find(name);
    if (it == syms.end()) return std::nullopt;
    return it->second;
}
std::optional<std::uint32_t> resolveLenient(const std::string& name, const SymbolTable& syms) {
    const auto it = syms.find(name);
    if (it == syms.end()) return std::uint32_t{0};  // size doesn't depend on value
    return it->second;
}

}  // namespace

AssembledRoutine assembleSm83(std::string_view source, const SymbolTable& predefined) {
    const std::vector<Line> lines = parseLines(source);

    // Pass 1 — assign label offsets. Instruction length depends only on operand shape, so encoding
    // with a lenient (unknown → 0) resolver yields the correct size even before labels are known.
    SymbolTable syms = predefined;
    SymbolTable labels;
    {
        std::size_t addr = 0;
        std::vector<std::uint8_t> scratch;
        for (const Line& ln : lines) {
            for (const std::string& lbl : ln.labels) {
                if (labels.count(lbl) || predefined.count(lbl)) {
                    fail(ln.number, "duplicate label '" + lbl + "'");
                }
                labels[lbl] = static_cast<std::uint32_t>(addr);
                syms[lbl] = static_cast<std::uint32_t>(addr);
            }
            if (ln.mnemonic.empty()) continue;
            scratch.clear();
            encode(ln.mnemonic, ln.operands, addr, ln.number, syms, &resolveLenient,
                   /*lenient=*/true, scratch);
            addr += scratch.size();
        }
    }

    // Pass 2 — encode for real, every label now resolvable.
    AssembledRoutine result;
    result.labels = labels;
    std::size_t addr = 0;
    for (const Line& ln : lines) {
        if (ln.mnemonic.empty()) continue;
        const std::size_t before = result.bytes.size();
        encode(ln.mnemonic, ln.operands, addr, ln.number, syms, &resolveStrict,
               /*lenient=*/false, result.bytes);
        addr += result.bytes.size() - before;
    }
    return result;
}

}  // namespace gbcpp::vm
