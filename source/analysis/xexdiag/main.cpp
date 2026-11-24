// xexdiag: checks that a XEX loads, and shows which instruction sequences lead
// into indirect jumps (bctr). Used to see why XenonAnalyse finds no switch tables.
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <fmt/core.h>

int main(int argc, char** argv)
{
    if (argc < 2) { fmt::println("Usage: xexdiag <default.xex>"); return 1; }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    fmt::println("base=0x{:X} size=0x{:X} entry=0x{:X}", image.base, image.size, image.entry_point);

    // xexdiag <xex> helpers: find the compiler helper functions XenonRecomp's config needs.
    if (argc >= 3 && std::string(argv[2]) == "helpers")
    {
        struct Pattern { const char* name; std::vector<uint8_t> bytes; };
        const Pattern patterns[] = {
            { "restgprlr_14_address", { 0xE9, 0xC1, 0xFF, 0x68 } },
            { "savegprlr_14_address", { 0xF9, 0xC1, 0xFF, 0x68 } },
            { "restfpr_14_address", { 0xC9, 0xCC, 0xFF, 0x70 } },
            { "savefpr_14_address", { 0xD9, 0xCC, 0xFF, 0x70 } },
            { "restvmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE } },
            { "savevmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE } },
            { "restvmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB } },
            { "savevmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB } },
        };
        for (const auto& p : patterns)
        {
            std::string hits;
            for (const auto& s : image.sections)
            {
                if (!(s.flags & SectionFlags_Code)) continue;
                for (size_t i = 0; i + p.bytes.size() <= s.size; i += 4)
                    if (memcmp(s.data + i, p.bytes.data(), p.bytes.size()) == 0)
                        hits += fmt::format(" 0x{:X}", s.base + i);
            }
            fmt::println("{} ={}", p.name, hits.empty() ? " NOT FOUND" : hits);
        }
        return 0;
    }

    // xexdiag <xex> dump <out.bin>: the decrypted, decompressed image (base 0x82000000). Game data: keep it in builds\.
    if (argc >= 4 && std::string(argv[2]) == "dump")
    {
        FILE* f = fopen(argv[3], "wb");
        if (f == nullptr) { fmt::println("Cannot write {}", argv[3]); return 1; }
        fwrite(image.data.get(), 1, image.size, f);
        fclose(f);
        fmt::println("wrote 0x{:X} bytes", image.size);
        return 0;
    }

    // xexdiag <xex> dis <hex address> <count>: disassemble count instructions from that address.
    if (argc >= 5 && std::string(argv[2]) == "dis")
    {
        uint32_t addr = std::stoul(argv[3], nullptr, 16);
        uint32_t count = std::stoul(argv[4]);
        for (uint32_t a = addr; a < addr + count * 4; a += 4)
        {
            auto* word = (const uint32_t*)image.Find(a);
            if (word == nullptr) break;
            ppc_insn insn;
            ppc::Disassemble(word, a, insn);
            fmt::println("  {:08X}  {} {}", a, insn.opcode ? insn.opcode->name : "?", insn.op_str);
        }
        return 0;
    }

    // xexdiag <xex> seh: .pdata functions with an exception handler, grouped by the handler word at begin-8.
    if (argc >= 3 && std::string(argv[2]) == "seh")
    {
        std::map<uint32_t, size_t> handlers;
        for (const auto& s : image.sections)
        {
            if (s.name != ".pdata") continue;
            for (size_t i = 0; i + 8 <= s.size; i += 8)
            {
                auto rd = [&](const uint8_t* p) { return uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); };
                uint32_t begin = rd(s.data + i), flags = rd(s.data + i + 4);
                if (begin == 0 || (flags >> 31) == 0) continue;
                auto* h = (const uint8_t*)image.Find(begin - 8);
                if (h) handlers[rd(h)]++;
            }
        }
        for (auto& [h, n] : handlers) fmt::println("handler 0x{:08X}: {} functions", h, n);
        return 0;
    }

    // xexdiag <xex> callers <hex address>: list every bl/b that targets that address.
    if (argc >= 4 && std::string(argv[2]) == "callers")
    {
        uint32_t target = std::stoul(argv[3], nullptr, 16);
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code)) continue;
            auto* words = (const uint32_t*)s.data;
            for (size_t i = 0; i < s.size / 4; i++)
            {
                ppc_insn insn;
                ppc::Disassemble(words + i, s.base + i * 4, insn);
                if (insn.opcode && (insn.opcode->id == PPC_INST_BL || insn.opcode->id == PPC_INST_B) && insn.operands[0] == target)
                    fmt::println("0x{:X} {}", s.base + i * 4, insn.opcode->name);
            }
        }
        return 0;
    }

    // xexdiag <xex> <hex address>: disassemble 24 instructions before and 8 after that address.
    if (argc >= 3)
    {
        uint32_t addr = std::stoul(argv[2], nullptr, 16);
        for (uint32_t a = addr - 24 * 4; a <= addr + 8 * 4; a += 4)
        {
            auto* word = (const uint32_t*)image.Find(a);
            if (word == nullptr) continue;
            ppc_insn insn;
            ppc::Disassemble(word, a, insn);
            fmt::println("{} {:08X}  {} {}", a == addr ? ">" : " ", a, insn.opcode ? insn.opcode->name : "?", insn.op_str);
        }
        return 0;
    }

    std::map<std::string, size_t> sequences;
    size_t bctrCount = 0, codeWords = 0, invalid = 0;

    for (const auto& section : image.sections)
    {
        fmt::println("section {:8} base=0x{:08X} size=0x{:08X} flags={}", section.name, section.base, section.size, (uint32_t)section.flags);
        if (!(section.flags & SectionFlags_Code))
            continue;

        auto* words = (const uint32_t*)section.data;
        size_t count = section.size / 4;
        codeWords += count;
        for (size_t i = 0; i < count; i++)
        {
            ppc_insn insn;
            ppc::Disassemble(words + i, section.base + i * 4, insn);
            if (insn.opcode == nullptr) { invalid++; continue; }
            if (insn.opcode->id != PPC_INST_BCTR || i < 8)
                continue;

            bctrCount++;
            std::string seq;
            for (size_t j = i - 8; j <= i; j++)
            {
                ppc_insn prev;
                ppc::Disassemble(words + j, section.base + j * 4, prev);
                seq += prev.opcode ? prev.opcode->name : "?";
                seq += ' ';
            }
            sequences[seq]++;
        }
    }

    fmt::println("code words={} invalid={} ({:.1f}%) bctr={}", codeWords, invalid, codeWords ? 100.0 * invalid / codeWords : 0.0, bctrCount);

    std::multimap<size_t, std::string, std::greater<>> sorted;
    for (auto& [seq, n] : sequences) sorted.emplace(n, seq);
    size_t shown = 0;
    for (auto& [n, seq] : sorted)
    {
        fmt::println("{:6}  {}", n, seq);
        if (++shown == 25) break;
    }
    return 0;
}
