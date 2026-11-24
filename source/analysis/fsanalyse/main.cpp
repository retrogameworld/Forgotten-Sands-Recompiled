// fsanalyse: finds switch jump tables in a XEX and writes them in XenonRecomp's TOML format.
//
// Stock XenonAnalyse matches fixed instruction sequences from Sonic Unleashed's compiler. Forgotten Sands
// was built with a compiler that reorders these sequences and pads them with nops, so instead of matching
// a sequence this tool works backwards from every bctr:
//   cmplwi crN, rIdx, N  ->  bgt crN, default  ->  ...  ->  mtctr rX  ->  bctr
// It rebuilds how rX is computed from rIdx (lis/addi constants, shifts, table loads, adds) as a small
// expression tree, then evaluates that tree for every index 0..N to get the case labels.
//
// Usage: fsanalyse <default.xex> <out switch_tables.toml>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>

constexpr int kWindow = 48;  // how many instructions to look back from a bctr

struct Expr
{
    enum Kind { Const, Index, Shl, Add, Load } kind;
    uint32_t value = 0;   // Const: value, Shl: shift amount, Load: size in bytes
    std::unique_ptr<Expr> a, b;
};
using ExprPtr = std::unique_ptr<Expr>;

static ExprPtr MakeConst(uint32_t v) { auto e = std::make_unique<Expr>(); e->kind = Expr::Const; e->value = v; return e; }
static ExprPtr MakeNode(Expr::Kind k, ExprPtr a, ExprPtr b, uint32_t v = 0)
{
    auto e = std::make_unique<Expr>(); e->kind = k; e->a = std::move(a); e->b = std::move(b); e->value = v; return e;
}

static bool HasIndex(const Expr* e) { return e && (e->kind == Expr::Index || HasIndex(e->a.get()) || HasIndex(e->b.get())); }
static bool HasLoad(const Expr* e) { return e && (e->kind == Expr::Load || HasLoad(e->a.get()) || HasLoad(e->b.get())); }

// Instructions in the window, decoded once. pos 0 is the oldest, the last one is the bctr.
struct Window
{
    uint32_t start = 0;
    std::vector<ppc_insn> insns;
    const ppc_insn& at(int i) const { return insns[i]; }
    uint32_t addr(int i) const { return start + i * 4; }
};

// Returns the GPR an instruction writes, or -1.
static int WrittenGpr(const ppc_insn& insn)
{
    if (insn.opcode == nullptr) return -1;
    const char* n = insn.opcode->name;
    auto starts = [&](const char* p) { return strncmp(n, p, strlen(p)) == 0; };
    if (starts("st") || starts("cmp") || starts("b") || starts("mt") || starts("tw") || starts("td") ||
        starts("dcb") || starts("icb") || starts("sync") || starts("isync") || starts("eieio") || starts("nop") ||
        starts("f") || starts("lf") || starts("v") || starts("lv") || starts("mff") || starts("db") || starts("crx") ||
        starts("cr") || starts("mcrf"))
        return -1;
    return (int)insn.operands[0];
}

struct Resolver
{
    const Window& w;
    int idxReg;
    int cmpPos;
    bool ok = true;

    ExprPtr Resolve(int reg, int before, int depth = 0)
    {
        if (depth > 16) { ok = false; return MakeConst(0); }
        for (int i = before - 1; i >= 0; i--)
        {
            const auto& insn = w.at(i);
            if (WrittenGpr(insn) != reg) continue;

            // The switch index: the compared register, as it was when compared.
            if (reg == idxReg && i < cmpPos)
                return MakeNode(Expr::Index, nullptr, nullptr);

            const uint32_t* op = insn.operands;
            switch (insn.opcode->id)
            {
            case PPC_INST_LIS:
                return MakeConst(op[1] << 16);
            case PPC_INST_LI:
                return MakeConst((uint32_t)(int32_t)(int16_t)op[1]);
            case PPC_INST_ADDI:
                if (op[1] == 0) return MakeConst((uint32_t)(int32_t)(int16_t)op[2]);
                return MakeNode(Expr::Add, Resolve(op[1], i, depth + 1), MakeConst((uint32_t)(int32_t)(int16_t)op[2]));
            case PPC_INST_ADDIS:
                return MakeNode(Expr::Add, Resolve(op[1], i, depth + 1), MakeConst(op[2] << 16));
            case PPC_INST_ORI:
            {
                auto a = Resolve(op[1], i, depth + 1);
                if (a->kind != Expr::Const) { ok = false; return a; }
                a->value |= op[2];
                return a;
            }
            case PPC_INST_MR:
                return Resolve(op[1], i, depth + 1);
            case PPC_INST_RLWINM:
            {
                uint32_t sh = op[2], mb = op[3], me = op[4];
                if (sh == 0) return Resolve(op[1], i, depth + 1);              // clrlwi: index is already small
                if (mb == 0 && me == 31 - sh) return MakeNode(Expr::Shl, Resolve(op[1], i, depth + 1), nullptr, sh);
                ok = false;
                return MakeConst(0);
            }
            case PPC_INST_ADD:
                return MakeNode(Expr::Add, Resolve(op[1], i, depth + 1), Resolve(op[2], i, depth + 1));
            case PPC_INST_LWZX:
                return MakeNode(Expr::Load, Resolve(op[1], i, depth + 1), Resolve(op[2], i, depth + 1), 4);
            case PPC_INST_LHZX:
                return MakeNode(Expr::Load, Resolve(op[1], i, depth + 1), Resolve(op[2], i, depth + 1), 2);
            case PPC_INST_LBZX:
                return MakeNode(Expr::Load, Resolve(op[1], i, depth + 1), Resolve(op[2], i, depth + 1), 1);
            default:
                ok = false;
                return MakeConst(0);
            }
        }
        if (reg == idxReg) return MakeNode(Expr::Index, nullptr, nullptr);
        ok = false;
        return MakeConst(0);
    }
};

static bool Eval(const Image& image, const Expr* e, uint32_t index, uint32_t& out)
{
    uint32_t a = 0, b = 0;
    switch (e->kind)
    {
    case Expr::Const: out = e->value; return true;
    case Expr::Index: out = index; return true;
    case Expr::Shl: if (!Eval(image, e->a.get(), index, a)) return false; out = a << e->value; return true;
    case Expr::Add:
        if (!Eval(image, e->a.get(), index, a) || !Eval(image, e->b.get(), index, b)) return false;
        out = a + b; return true;
    case Expr::Load:
    {
        if (!Eval(image, e->a.get(), index, a) || !Eval(image, e->b.get(), index, b)) return false;
        auto* p = (const uint8_t*)image.Find(a + b);
        if (p == nullptr) return false;
        if (e->value == 4) out = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        else if (e->value == 2) out = (p[0] << 8) | p[1];
        else out = p[0];
        return true;
    }
    }
    return false;
}

// Function extents from .pdata, plus a guess for code that .pdata does not cover.
// (Same approach as Torchlight's find_jump_tables.py.)
struct Functions
{
    const Image& image;
    std::vector<std::pair<uint32_t, uint32_t>> funcs;  // [start, end)
    std::vector<uint32_t> callTargets;                 // every bl target, sorted
    uint32_t textStart = 0, textEnd = 0;

    explicit Functions(const Image& img) : image(img)
    {
        for (const auto& s : img.sections)
        {
            if (s.name == ".text") { textStart = (uint32_t)s.base; textEnd = (uint32_t)(s.base + s.size); }
            if (s.name != ".pdata") continue;
            for (size_t i = 0; i + 8 <= s.size; i += 8)
            {
                auto rd = [&](size_t o) { const uint8_t* p = s.data + o; return uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); };
                uint32_t begin = rd(i), packed = rd(i + 4);
                if (begin == 0) continue;
                funcs.emplace_back(begin, begin + ((packed >> 8) & 0x3FFFFF) * 4);
            }
        }
        std::sort(funcs.begin(), funcs.end());

        for (const auto& s : img.sections)
        {
            if (!(s.flags & SectionFlags_Code)) continue;
            for (size_t i = 0; i < s.size; i += 4)
            {
                const uint8_t* p = s.data + i;
                uint32_t x = uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
                if ((x >> 26) != 18 || (x & 3) != 1) continue;  // bl (relative, link)
                int32_t off = x & 0x03FFFFFC;
                if (off & 0x02000000) off -= 0x04000000;
                callTargets.push_back(uint32_t(s.base + i + off));
            }
        }
        std::sort(callTargets.begin(), callTargets.end());
        callTargets.erase(std::unique(callTargets.begin(), callTargets.end()), callTargets.end());
    }

    uint32_t W(uint32_t a) const { auto* p = (const uint8_t*)image.Find(a); return p ? uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) : 0; }

    // Index of the first function starting after a.
    size_t UpperBound(uint32_t a) const
    {
        return std::upper_bound(funcs.begin(), funcs.end(), std::make_pair(a, UINT32_MAX)) - funcs.begin();
    }

    const std::pair<uint32_t, uint32_t>* Of(uint32_t a) const
    {
        size_t i = UpperBound(a);
        if (i > 0 && funcs[i - 1].first <= a && a < funcs[i - 1].second) return &funcs[i - 1];
        return nullptr;
    }

    int64_t BranchTarget(uint32_t b) const
    {
        uint32_t x = W(b), op = x >> 26;
        if (x & 1) return -1;  // bl/bcl are calls
        if (op == 18) { int32_t off = x & 0x03FFFFFC; if (off & 0x02000000) off -= 0x04000000; return int64_t(b) + off; }
        if (op == 16) { int32_t off = x & 0xFFFC; if (off & 0x8000) off -= 0x10000; return int64_t(b) + off; }
        return -1;
    }

    bool BranchesCross(uint32_t lo, uint32_t mid, uint32_t hi) const
    {
        for (uint32_t b = lo; b < mid; b += 4) { auto t = BranchTarget(b); if (t >= mid && t < hi) return true; }
        for (uint32_t b = mid; b < hi; b += 4) { auto t = BranchTarget(b); if (t >= lo && t < mid) return true; }
        return false;
    }

    // Code outside .pdata: start after the last blr before a, merging earlier chunks while branches cross.
    std::pair<uint32_t, uint32_t> GapOf(uint32_t a) const
    {
        size_t i = UpperBound(a);
        uint32_t prevEnd = i > 0 ? funcs[i - 1].second : textStart;
        uint32_t nextStart = i < funcs.size() ? funcs[i].first : textEnd;
        constexpr uint32_t BLR = 0x4E800020;
        uint32_t start = a;
        while (start - 4 >= prevEnd && W(start - 4) != BLR) start -= 4;
        while (start > prevEnd)
        {
            uint32_t prev = start - 4;
            while (prev - 4 >= prevEnd && W(prev - 4) != BLR) prev -= 4;
            if (!BranchesCross(prev, start, nextStart)) break;
            start = prev;
        }
        while (start < a && (W(start) == 0 || W(start) == 0x60000000)) start += 4;
        // A bl target between the guess and a is a real function start (e.g. a tiny stub that tail-jumps
        // elsewhere sitting right before the switch function).
        auto it = std::upper_bound(callTargets.begin(), callTargets.end(), a);
        if (it != callTargets.begin() && *std::prev(it) > start) start = *std::prev(it);
        return { start, nextStart };
    }
};

int main(int argc, char** argv)
{
    if (argc < 3) { fmt::println("Usage: fsanalyse <default.xex> <out switch_tables.toml>"); return 1; }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    Functions functions(image);
    std::string gapFunctions;
    size_t gapCount = 0;

    uint32_t textStart = 0, textEnd = 0;
    size_t totalWords = 0;
    for (const auto& s : image.sections)
        if (s.flags & SectionFlags_Code) totalWords += s.size / 4;

    std::string out = "# Generated by fsanalyse (<project folder>\\src\\analysis\\fsanalyse)\n\n";
    size_t bctrs = 0, found = 0, notSwitch = 0, clobbered = 0, failed = 0, doneWords = 0;
    int lastPct = -1;
    std::vector<std::string> failures;

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code)) continue;
        auto* words = (const uint32_t*)section.data;
        size_t count = section.size / 4;

        for (size_t i = 0; i < count; i++, doneWords++)
        {
            int pct = (int)(doneWords * 100 / totalWords);
            if (pct / 5 != lastPct / 5) { lastPct = pct; fmt::println("progress {}%", pct / 5 * 5); }

            ppc_insn bctr;
            ppc::Disassemble(words + i, section.base + i * 4, bctr);
            if (bctr.opcode == nullptr || bctr.opcode->id != PPC_INST_BCTR || i < kWindow) continue;
            bctrs++;

            Window w;
            w.start = (uint32_t)(section.base + (i - kWindow) * 4);
            w.insns.resize(kWindow + 1);
            for (int j = 0; j <= kWindow; j++)
                ppc::Disassemble(words + i - kWindow + j, w.addr(j), w.insns[j]);
            const int bctrPos = kWindow;

            // Walk back to mtctr; stop at anything that leaves the block.
            int mtctrPos = -1, bgtPos = -1, cmpPos = -1;
            for (int j = bctrPos - 1; j >= 0; j--)
            {
                const auto& in = w.at(j);
                if (in.opcode == nullptr) break;
                auto id = in.opcode->id;
                if (id == PPC_INST_B || id == PPC_INST_BLR || id == PPC_INST_BCTR) break;
                if (mtctrPos < 0) { if (id == PPC_INST_MTCTR) mtctrPos = j; continue; }
                if (bgtPos < 0) { if (id == PPC_INST_BGT || id == PPC_INST_BLE || id == PPC_INST_BGTLR || id == PPC_INST_BLELR) bgtPos = j; continue; } // bgtlr: out of range returns
                if (id == PPC_INST_CMPLWI && in.operands[0] == w.at(bgtPos).operands[0]) { cmpPos = j; break; }
            }
            if (mtctrPos < 0 || bgtPos < 0 || cmpPos < 0)
            {
                // Inline jump table with the range check somewhere else (e.g. sub_82BF5570):
                //   lis rA,hi / rlwinm rB,rIdx,2,0,29 / addi rA,rA,lo / lwzx rX,rA,rB / mtctr rX / bctr / <table>
                // The table starts right after the bctr; its length = the run of words that are code
                // addresses inside the same function.
                const uint32_t bctrAddr = w.addr(bctrPos);
                const auto& mt = w.at(bctrPos - 1);
                const auto& ld = w.at(bctrPos - 2);
                if (mt.opcode && mt.opcode->id == PPC_INST_MTCTR && ld.opcode && ld.opcode->id == PPC_INST_LWZX && ld.operands[0] == mt.operands[0])
                {
                    int idx = -1;
                    uint32_t hi = 0, lo = 0;
                    bool haveHi = false, haveLo = false;
                    for (int j = bctrPos - 3; j >= bctrPos - 8; j--)
                    {
                        const auto& in = w.at(j);
                        if (!in.opcode) break;
                        if (in.opcode->id == PPC_INST_RLWINM && in.operands[0] == ld.operands[2] && in.operands[2] == 2 && in.operands[3] == 0 && in.operands[4] == 29 && idx < 0)
                            idx = (int)in.operands[1];
                        else if (in.opcode->id == PPC_INST_ADDI && in.operands[0] == ld.operands[1] && in.operands[1] == ld.operands[1] && !haveLo)
                            { lo = uint32_t(int32_t(int16_t(in.operands[2]))); haveLo = true; }
                        else if (in.opcode->id == PPC_INST_LIS && in.operands[0] == ld.operands[1] && !haveHi)
                            { hi = in.operands[1] << 16; haveHi = true; }
                    }
                    const auto* fn = functions.Of(bctrAddr);
                    if (idx >= 0 && haveHi && haveLo && hi + lo == bctrAddr + 4 && fn)
                    {
                        std::vector<uint32_t> labels;
                        for (uint32_t a = bctrAddr + 4; a < fn->second; a += 4)
                        {
                            uint32_t v = functions.W(a);
                            if (v < fn->first || v >= fn->second || (v & 3)) break;
                            labels.push_back(v);
                        }
                        if (labels.size() >= 2)
                        {
                            found++;
                            out += "[[switch]]\n";
                            out += fmt::format("base = 0x{:X}\n", bctrAddr);
                            out += fmt::format("r = {}\n", idx);
                            out += "default = 0x0\n";
                            out += "labels = [\n";
                            for (auto l : labels) out += fmt::format("    0x{:X},\n", l);
                            out += "]\n\n";
                            failures.push_back(fmt::format("0x{:X}: inline table without range check, {} entries (taken)", bctrAddr, labels.size()));
                            continue;
                        }
                    }
                }

                notSwitch++;
                // An indexed load right before the jump smells like a switch we failed to recognise.
                for (int j = bctrPos - 1; j >= bctrPos - 8; j--)
                {
                    auto* op = w.at(j).opcode;
                    if (op && (op->id == PPC_INST_LWZX || op->id == PPC_INST_LHZX || op->id == PPC_INST_LBZX))
                    {
                        failures.push_back(fmt::format("0x{:X}: indexed load before jump but no bounds check found (check by hand)", w.addr(bctrPos)));
                        break;
                    }
                }
                continue;
            }

            const auto& cmp = w.at(cmpPos);
            int idxReg = (int)cmp.operands[1];
            uint32_t caseCount = cmp.operands[2] + 1;
            const auto bgtId = w.at(bgtPos).opcode->id;
            uint32_t defaultLabel = bgtId == PPC_INST_BGT ? w.at(bgtPos).operands[1] : (bgtId == PPC_INST_BGTLR || bgtId == PPC_INST_BLELR) ? 0 : w.addr(bgtPos + 1);

            Resolver r{ w, idxReg, cmpPos };
            auto expr = r.Resolve((int)w.at(mtctrPos).operands[0], mtctrPos);
            uint32_t bctrAddr = w.addr(bctrPos);
            if (!r.ok || !HasIndex(expr.get()) || !HasLoad(expr.get()))
            {
                failed++;
                failures.push_back(fmt::format("0x{:X}: could not follow how the jump target is computed", bctrAddr));
                continue;
            }

            std::vector<uint32_t> labels(caseCount);
            bool good = true;
            for (uint32_t k = 0; k < caseCount && good; k++)
            {
                good = Eval(image, expr.get(), k, labels[k]);
                // A case label must be code close to the switch (same function).
                if (good && (labels[k] < section.base || labels[k] >= section.base + section.size ||
                             labels[k] + 0x40000 < bctrAddr || labels[k] > bctrAddr + 0x40000 || (labels[k] & 3)))
                    good = false;
            }
            if (!good)
            {
                failed++;
                failures.push_back(fmt::format("0x{:X}: case labels look wrong", bctrAddr));
                continue;
            }

            // The recompiler switches on the index register's value at the bctr, so it must still hold the index.
            for (int j = cmpPos + 1; j < bctrPos; j++)
                if (WrittenGpr(w.at(j)) == idxReg) { good = false; break; }
            if (!good)
            {
                clobbered++;
                failures.push_back(fmt::format("0x{:X}: index register r{} is overwritten before the jump", bctrAddr, idxReg));
                continue;
            }

            // Labels must sit inside the switch's function. Functions missing from .pdata get their extent
            // written to the config, or XenonRecomp splits the case blocks into separate tiny functions.
            uint32_t maxLabel = bctrAddr;
            for (auto l : labels) maxLabel = (std::max)(maxLabel, l);
            if (auto* fn = functions.Of(bctrAddr))
            {
                for (auto l : labels) if (l < fn->first || l >= fn->second) good = false;
                if (!good)
                {
                    failed++;
                    failures.push_back(fmt::format("0x{:X}: case labels leave its .pdata function 0x{:X}", bctrAddr, fn->first));
                    continue;
                }
            }
            else
            {
                auto gap = functions.GapOf(bctrAddr);
                for (auto l : labels) if (l < gap.first || l >= gap.second) good = false;
                if (!good)
                {
                    failed++;
                    failures.push_back(fmt::format("0x{:X}: case labels leave the guessed function 0x{:X}", bctrAddr, gap.first));
                    continue;
                }
                // The default case often sits right after the last case block; keep it inside too.
                if (defaultLabel > maxLabel && defaultLabel < gap.second && defaultLabel < maxLabel + 0x40) maxLabel = defaultLabel;
                uint32_t end = maxLabel + 4;
                while (end < gap.second && functions.W(end - 4) != 0x4E800020) end += 4;
                gapFunctions += fmt::format("    {{ address = 0x{:X}, size = 0x{:X} }},\n", gap.first, end - gap.first);
                gapCount++;
            }

            found++;
            out += "[[switch]]\n";
            out += fmt::format("base = 0x{:X}\n", bctrAddr);
            out += fmt::format("r = {}\n", idxReg);
            out += fmt::format("default = 0x{:X}\n", defaultLabel);
            out += "labels = [\n";
            for (auto l : labels) out += fmt::format("    0x{:X},\n", l);
            out += "]\n\n";
        }
    }

    FILE* f = fopen(argv[2], "w");
    if (f == nullptr) { fmt::println("Cannot write {}", argv[2]); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    // XenonRecomp config fragment: functions missing from .pdata that hold switch tables.
    std::string fnPath = std::string(argv[2]);
    fnPath = fnPath.substr(0, fnPath.rfind('.')) + "_functions.txt";
    if (FILE* ff = fopen(fnPath.c_str(), "w"))
    {
        std::string text = "functions = [\n" + gapFunctions + "]\n";
        fwrite(text.data(), 1, text.size(), ff);
        fclose(ff);
    }

    fmt::println("bctr total: {}", bctrs);
    fmt::println("switches in functions missing from .pdata: {} (written to {})", gapCount, fnPath);
    fmt::println("not a switch (function pointer / vtable jumps): {}", notSwitch);
    fmt::println("switch tables found: {}", found);
    fmt::println("index register overwritten: {}", clobbered);
    fmt::println("could not decode: {}", failed);
    for (auto& s : failures) fmt::println("  {}", s);
    return 0;
}
