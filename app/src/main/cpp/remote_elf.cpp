#include "remote_elf.hpp"
#include "mem.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <vector>

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

namespace {

struct SymTables {
    const Elf64_Shdr *dynsym = nullptr;
    const Elf64_Shdr *dynstr = nullptr;
    const Elf64_Shdr *symtab = nullptr;
    const Elf64_Shdr *strtab = nullptr;
};

static bool read_ehdr(pid_t pid, uintptr_t base, Elf64_Ehdr &eh) {
    return mem_read(pid, base, eh) && eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E';
}

static bool read_ehdr_file(FILE *f, Elf64_Ehdr &eh) {
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    return std::fread(&eh, 1, sizeof(eh), f) == sizeof(eh) && eh.e_ident[0] == 0x7f;
}

static bool calc_bias_from_phdr(pid_t pid, uintptr_t base, const Elf64_Ehdr &eh, uintptr_t &bias) {
    if (eh.e_phnum == 0 || eh.e_phnum > 128 || !eh.e_phoff) return false;
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (!mem_read(pid, base + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf64_Phdr)))
        return false;
    uint64_t min_v = UINT64_MAX;
    for (const auto &p : ph) {
        if (p.p_type != PT_LOAD) continue;
        min_v = std::min(min_v, (uint64_t)p.p_vaddr);
    }
    if (min_v == UINT64_MAX) return false;
    bias = base - (uintptr_t)min_v;
    return true;
}

static bool calc_bias_from_phdr_file(FILE *f, const Elf64_Ehdr &eh, uintptr_t map_base,
                                     uintptr_t &bias) {
    if (eh.e_phnum == 0 || eh.e_phnum > 128 || !eh.e_phoff) return false;
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (std::fseek(f, (long)eh.e_phoff, SEEK_SET) != 0) return false;
    if (std::fread(ph.data(), sizeof(Elf64_Phdr), ph.size(), f) != ph.size()) return false;
    uint64_t min_v = UINT64_MAX;
    for (const auto &p : ph) {
        if (p.p_type != PT_LOAD) continue;
        min_v = std::min(min_v, (uint64_t)p.p_vaddr);
    }
    if (min_v == UINT64_MAX) return false;
    bias = map_base - (uintptr_t)min_v;
    return true;
}

static bool lookup_in_tables(const char *sym_name, const char *str_data, size_t str_size,
                             const Elf64_Sym *syms, size_t sym_count, uintptr_t load_bias,
                             uintptr_t &addr_out) {
    for (size_t i = 0; i < sym_count; ++i) {
        const Elf64_Sym &s = syms[i];
        if (s.st_name >= str_size || !s.st_value) continue;
        const int type = ELF64_ST_TYPE(s.st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        if (std::strcmp(str_data + s.st_name, sym_name) != 0) continue;
        addr_out = load_bias + (uintptr_t)s.st_value;
        return true;
    }
    return false;
}

static bool sym_from_sections_remote(pid_t pid, uintptr_t base, const Elf64_Ehdr &eh,
                                     uintptr_t load_bias, const char *sym, uintptr_t &addr) {
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (!mem_read(pid, base + eh.e_shoff, sh.data(), sh.size() * sizeof(Elf64_Shdr)))
        return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;

    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 512 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (!mem_read(pid, base + strsh.sh_offset, shstr.data(), shstr.size())) return false;

    SymTables t{};
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) t.dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) t.dynstr = &s;
        if (std::strcmp(nm, ".symtab") == 0) t.symtab = &s;
        if (std::strcmp(nm, ".strtab") == 0) t.strtab = &s;
    }

    auto try_table = [&](const Elf64_Shdr *symsh, const Elf64_Shdr *strshp) -> bool {
        if (!symsh || !strshp || symsh->sh_entsize != sizeof(Elf64_Sym)) return false;
        if (strshp->sh_size == 0 || strshp->sh_size > 16 * 1024 * 1024) return false;
        if (symsh->sh_size == 0 || symsh->sh_size > 16 * 1024 * 1024) return false;
        std::vector<char> strtab(strshp->sh_size);
        if (!mem_read(pid, base + strshp->sh_offset, strtab.data(), strtab.size())) return false;
        const size_t n = symsh->sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(n);
        if (!mem_read(pid, base + symsh->sh_offset, syms.data(), symsh->sh_size)) return false;
        return lookup_in_tables(sym, strtab.data(), strtab.size(), syms.data(), n, load_bias,
                                addr);
    };

    if (try_table(t.dynsym, t.dynstr)) return true;
    if (try_table(t.symtab, t.strtab)) return true;
    return false;
}

static bool sym_from_sections_file(FILE *f, const Elf64_Ehdr &eh, uintptr_t load_bias,
                                   const char *sym, uintptr_t &addr) {
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (std::fseek(f, (long)eh.e_shoff, SEEK_SET) != 0) return false;
    if (std::fread(sh.data(), sizeof(Elf64_Shdr), sh.size(), f) != sh.size()) return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;

    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 512 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (std::fseek(f, (long)strsh.sh_offset, SEEK_SET) != 0) return false;
    if (std::fread(shstr.data(), 1, shstr.size(), f) != shstr.size()) return false;

    SymTables t{};
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) t.dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) t.dynstr = &s;
        if (std::strcmp(nm, ".symtab") == 0) t.symtab = &s;
        if (std::strcmp(nm, ".strtab") == 0) t.strtab = &s;
    }

    auto try_table = [&](const Elf64_Shdr *symsh, const Elf64_Shdr *strshp) -> bool {
        if (!symsh || !strshp || symsh->sh_entsize != sizeof(Elf64_Sym)) return false;
        std::vector<char> strtab(strshp->sh_size);
        if (std::fseek(f, (long)strshp->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(strtab.data(), 1, strtab.size(), f) != strtab.size()) return false;
        const size_t n = symsh->sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(n);
        if (std::fseek(f, (long)symsh->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(syms.data(), sizeof(Elf64_Sym), n, f) != n) return false;
        return lookup_in_tables(sym, strtab.data(), strtab.size(), syms.data(), n, load_bias,
                                addr);
    };

    if (try_table(t.dynsym, t.dynstr)) return true;
    if (try_table(t.symtab, t.strtab)) return true;
    return false;
}

} // namespace

bool elf_file_map_bias(const char *path, uintptr_t map_base, uintptr_t &load_bias_out) {
    load_bias_out = 0;
    if (!path || !path[0] || !map_base) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    Elf64_Ehdr eh{};
    if (!read_ehdr_file(f, eh)) {
        std::fclose(f);
        return false;
    }
    const bool ok = calc_bias_from_phdr_file(f, eh, map_base, load_bias_out);
    std::fclose(f);
    return ok;
}

bool elf_remote_bias(pid_t pid, uintptr_t map_base, ElfRemote &out) {
    out = {};
    out.base = map_base;
    Elf64_Ehdr eh{};
    if (!read_ehdr(pid, map_base, eh)) return false;
    return calc_bias_from_phdr(pid, map_base, eh, out.load_bias);
}

static uintptr_t relocate_dyn(uintptr_t raw, uintptr_t bias, uintptr_t base) {
    if (!raw) return 0;
    if (raw >= base && raw < base + 0x20000000ULL) return raw;
    if (raw < 0x20000000ULL) return bias + raw;
    return raw;
}

static bool dynsym_lookup(pid_t pid, uintptr_t base, const Elf64_Ehdr &eh, uintptr_t bias,
                          const char *sym, uintptr_t &addr) {
    if (!eh.e_phoff || eh.e_phnum == 0 || eh.e_phnum > 128) return false;
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (!mem_read(pid, base + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf64_Phdr))) return false;

    uintptr_t dynAddr = 0;
    size_t dynBytes = 0;
    for (const auto &p : ph) {
        if (p.p_type != PT_DYNAMIC || !p.p_filesz) continue;
        dynAddr = bias + (uintptr_t)p.p_vaddr;
        dynBytes = (size_t)p.p_filesz;
        break;
    }
    if (!dynAddr || dynBytes < sizeof(Elf64_Dyn)) return false;
    if (dynBytes > 64 * 1024) dynBytes = 64 * 1024;

    std::vector<Elf64_Dyn> dyn(dynBytes / sizeof(Elf64_Dyn));
    if (!mem_read(pid, dynAddr, dyn.data(), dyn.size() * sizeof(Elf64_Dyn))) return false;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    size_t strsz = 0;
    for (const auto &d : dyn) {
        if (d.d_tag == DT_NULL) break;
        if (d.d_tag == DT_SYMTAB) symtab = (uintptr_t)d.d_un.d_ptr;
        else if (d.d_tag == DT_STRTAB) strtab = (uintptr_t)d.d_un.d_ptr;
        else if (d.d_tag == DT_STRSZ) strsz = (size_t)d.d_un.d_val;
        else if (d.d_tag == DT_HASH) hash = (uintptr_t)d.d_un.d_ptr;
        else if (d.d_tag == DT_GNU_HASH) gnu = (uintptr_t)d.d_un.d_ptr;
    }
    symtab = relocate_dyn(symtab, bias, base);
    strtab = relocate_dyn(strtab, bias, base);
    hash = relocate_dyn(hash, bias, base);
    gnu = relocate_dyn(gnu, bias, base);
    if (!symtab || !strtab || strsz == 0 || strsz > 8 * 1024 * 1024) return false;

    size_t nsyms = 0;
    if (hash) {
        uint32_t hdr[2] = {};
        if (mem_read(pid, hash, hdr, sizeof(hdr)) && hdr[1] > 0 && hdr[1] < 400000)
            nsyms = hdr[1];
    }
    if (!nsyms && gnu) {
        uint32_t hdr[4] = {};
        if (!mem_read(pid, gnu, hdr, sizeof(hdr))) return false;
        const uint32_t nbuckets = hdr[0];
        const uint32_t symoff = hdr[1];
        const uint32_t bloom = hdr[2];
        if (!nbuckets || nbuckets > 1000000 || bloom > 1000000) return false;
        const uintptr_t bucketsAddr = gnu + 16 + (uintptr_t)bloom * sizeof(uint64_t);
        std::vector<uint32_t> buckets(nbuckets);
        if (!mem_read(pid, bucketsAddr, buckets.data(), buckets.size() * sizeof(uint32_t)))
            return false;
        uint32_t hi = symoff;
        for (uint32_t b : buckets)
            if (b >= symoff && b > hi) hi = b;
        size_t chainCount = (size_t)(hi - symoff) + 1;
        std::vector<uint32_t> chains(chainCount);
        const uintptr_t chainsAddr = bucketsAddr + nbuckets * sizeof(uint32_t);
        if (!mem_read(pid, chainsAddr, chains.data(), chains.size() * sizeof(uint32_t)))
            return false;
        bool grew = true;
        while (grew && chains.size() < 400000) {
            grew = false;
            for (uint32_t b : buckets) {
                if (b < symoff) continue;
                uint32_t i = b;
                for (int step = 0; step < 100000; ++step) {
                    const size_t ci = (size_t)(i - symoff);
                    if (ci >= chains.size()) {
                        const size_t extra = 8192;
                        const size_t old = chains.size();
                        chains.resize(old + extra);
                        if (!mem_read(pid, chainsAddr + old * sizeof(uint32_t), chains.data() + old,
                                      extra * sizeof(uint32_t)))
                            return false;
                        grew = true;
                    }
                    if (i > hi) hi = i;
                    if (chains[ci] & 1u) break;
                    ++i;
                }
            }
        }
        nsyms = (size_t)hi + 1;
    }
    if (!nsyms || nsyms > 400000) return false;

    std::vector<char> str(strsz);
    if (!mem_read(pid, strtab, str.data(), str.size())) return false;
    str.back() = '\0';

    constexpr size_t kChunk = 512;
    for (size_t baseIdx = 0; baseIdx < nsyms; baseIdx += kChunk) {
        const size_t n = std::min(kChunk, nsyms - baseIdx);
        std::vector<Elf64_Sym> syms(n);
        if (!mem_read(pid, symtab + baseIdx * sizeof(Elf64_Sym), syms.data(), n * sizeof(Elf64_Sym)))
            return false;
        for (const auto &s : syms) {
            if (!s.st_value || s.st_name >= str.size()) continue;
            const char *nm = str.data() + s.st_name;
            const size_t avail = str.size() - s.st_name;
            if (strnlen(nm, avail) == avail) continue;
            if (std::strcmp(nm, sym) != 0) continue;
            addr = bias + (uintptr_t)s.st_value;
            return true;
        }
    }
    return false;
}

bool elf_remote_sym(pid_t pid, uintptr_t map_base, const char *name, uintptr_t &addr_out) {
    addr_out = 0;
    Elf64_Ehdr eh{};
    if (!read_ehdr(pid, map_base, eh)) return false;
    uintptr_t bias = 0;
    if (!calc_bias_from_phdr(pid, map_base, eh, bias)) return false;
    if (dynsym_lookup(pid, map_base, eh, bias, name, addr_out)) return true;
    return sym_from_sections_remote(pid, map_base, eh, bias, name, addr_out);
}

bool elf_file_sym(const char *path, uintptr_t load_bias, const char *name, uintptr_t &addr_out) {
    addr_out = 0;
    if (!path || !path[0]) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    Elf64_Ehdr eh{};
    if (!read_ehdr_file(f, eh)) {
        std::fclose(f);
        return false;
    }
    const bool ok = sym_from_sections_file(f, eh, load_bias, name, addr_out);
    std::fclose(f);
    return ok;
}

bool elf_file_sym_contains(const char *path, uintptr_t load_bias, const char *needle,
                           uintptr_t &addr_out, char *matched, size_t matchedCap) {
    addr_out = 0;
    if (matched && matchedCap) matched[0] = '\0';
    if (!path || !needle || !needle[0]) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    Elf64_Ehdr eh{};
    if (!read_ehdr_file(f, eh)) {
        std::fclose(f);
        return false;
    }
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) {
        std::fclose(f);
        return false;
    }
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (std::fseek(f, (long)eh.e_shoff, SEEK_SET) != 0 ||
        std::fread(sh.data(), sizeof(Elf64_Shdr), sh.size(), f) != sh.size()) {
        std::fclose(f);
        return false;
    }
    if (eh.e_shstrndx >= eh.e_shnum) {
        std::fclose(f);
        return false;
    }
    const Elf64_Shdr &shstr = sh[eh.e_shstrndx];
    std::vector<char> shstrtab(shstr.sh_size);
    if (std::fseek(f, (long)shstr.sh_offset, SEEK_SET) != 0 ||
        std::fread(shstrtab.data(), 1, shstrtab.size(), f) != shstrtab.size()) {
        std::fclose(f);
        return false;
    }

    auto scan_table = [&](const Elf64_Shdr *symsh, const Elf64_Shdr *strshp) -> bool {
        if (!symsh || !strshp || symsh->sh_entsize != sizeof(Elf64_Sym)) return false;
        std::vector<char> strtab(strshp->sh_size);
        if (std::fseek(f, (long)strshp->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(strtab.data(), 1, strtab.size(), f) != strtab.size()) return false;
        const size_t n = symsh->sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(n);
        if (std::fseek(f, (long)symsh->sh_offset, SEEK_SET) != 0) return false;
        if (std::fread(syms.data(), sizeof(Elf64_Sym), n, f) != n) return false;
        for (const auto &s : syms) {
            if (s.st_name >= strtab.size() || !s.st_value) continue;
            const char *nm = strtab.data() + s.st_name;
            if (!std::strstr(nm, needle)) continue;
            if (!std::strstr(nm, "TypeInfo") && !std::strstr(nm, "_c")) continue;
            addr_out = load_bias + (uintptr_t)s.st_value;
            if (matched && matchedCap)
                std::snprintf(matched, matchedCap, "%s", nm);
            return true;
        }
        return false;
    };

    const Elf64_Shdr *dynsym = nullptr, *dynstr = nullptr, *symtab = nullptr, *strtab = nullptr;
    for (const auto &s : sh) {
        if (s.sh_name >= shstrtab.size()) continue;
        const char *nm = shstrtab.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) dynstr = &s;
        if (std::strcmp(nm, ".symtab") == 0) symtab = &s;
        if (std::strcmp(nm, ".strtab") == 0) strtab = &s;
    }

    bool ok = scan_table(dynsym, dynstr);
    if (!ok) ok = scan_table(symtab, strtab);
    std::fclose(f);
    return ok;
}
