// m0boy ROM [target]: drop a ROM, get a release and a source archive.
//
// One executable, no helper script: it finds the console from the file, looks
// for deepeel's coverage beside the ROM (or runs deepeel to make it), turns that
// coverage into what the generators read, runs m0recomp, builds the project for
// the target (the host PC by default) and writes
//   <out>/<name>-<target>-release.tar.gz   executable (or firmware) + README
//   <out>/<name>-<target>-source.tar.gz    generated C project + coverage used
// deepeel's map PNG is kept in <out>/ (and in the source archive).
#include <zlib.h>

#include "m0boy/m0recomp.h"
#ifdef M0BOY_HAVE_DEEPEEL
#include "deepeel.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void die(const std::string& m) { std::cerr << "m0boy: " << m << "\n"; std::exit(1); }
void say(const std::string& m) { std::cerr << "m0boy: " << m << std::endl; }

std::string quote(const std::string& s) { return s; }

// Runs argv (no shell). capture: stdout returned; otherwise stdout goes to /dev/null when quiet.
int run(const std::vector<std::string>& args, std::string* capture = nullptr, bool quiet = false) {
    std::string line;
    for (auto& a : args) line += (line.empty() ? "" : " ") + a;
    if (!capture) say(line);
    int pipefd[2] = {-1, -1};
    if (capture && pipe(pipefd) != 0) die("pipe failed");
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        if (capture) { dup2(pipefd[1], 1); close(pipefd[0]); close(pipefd[1]); }
        else if (quiet) { int n = open("/dev/null", O_WRONLY); dup2(n, 1); }
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    if (capture) {
        close(pipefd[1]);
        char buf[4096]; ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof buf)) > 0) capture->append(buf, (size_t)n);
        close(pipefd[0]);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
// Runs a library entry point (m0recomp_main, deepeel_main) in a forked child: the engines keep
// process-wide state and may call exit(), so each run is isolated; no executable is involved.
using EntryFn = int (*)(int, char**);
int run_fn(EntryFn fn, const std::string& label, std::vector<std::string> args, std::string* capture = nullptr) {
    args.insert(args.begin(), label);
    std::string line;
    for (auto& a : args) line += (line.empty() ? "" : " ") + a;
    if (!capture) say(line);
    int pipefd[2] = {-1, -1};
    if (capture && pipe(pipefd) != 0) die("pipe failed");
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        if (capture) { dup2(pipefd[1], 1); close(pipefd[0]); close(pipefd[1]); }
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        int rc = fn((int)args.size(), av.data());
        fflush(nullptr);
        _exit(rc);
    }
    if (capture) {
        close(pipefd[1]);
        char buf[4096]; ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof buf)) > 0) capture->append(buf, (size_t)n);
        close(pipefd[0]);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
int m0recomp_call(const std::vector<std::string>& a, std::string* capture = nullptr) {
    return run_fn(m0recomp_main, "m0recomp", a, capture);
}

void must(const std::vector<std::string>& args, bool quiet = false) {
    int rc = run(args, nullptr, quiet);
    if (rc) die("failed (" + std::to_string(rc) + "): " + args[0]);
}

fs::path self_dir() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(".") : p.parent_path();
}

bool executable(const fs::path& p) { return fs::is_regular_file(p) && access(p.c_str(), X_OK) == 0; }

fs::path find_tool(const char* name, const char* env) {
    if (const char* e = getenv(env)) if (executable(e)) return e;
    fs::path sib = self_dir() / name;
    if (executable(sib)) return sib;
    if (const char* path = getenv("PATH")) {
        std::stringstream ss(path); std::string d;
        while (std::getline(ss, d, ':')) if (!d.empty() && executable(fs::path(d) / name)) return fs::path(d) / name;
    }
    for (const char* rel : {"../memscan/build", "../deepeel/build"}) {
        fs::path p = self_dir() / "../.." / rel / name;   // <repo>/build/bin -> <repo>/..
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (executable(p)) return p;
    }
    return {};
}

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

std::string detect_system(const fs::path& rom, const std::string& forced) {
    if (!forced.empty()) return forced;
    std::string out;
    m0recomp_call({rom.string(), "--info"}, &out);
    std::stringstream ss(out); std::string line;
    while (std::getline(ss, line)) if (line.rfind("system=", 0) == 0) return line.substr(7);
    die("cannot tell which console " + rom.filename().string() + " is for");
}

// ---- deepeel coverage -> m0boy profile (sg1000/gg/sms/msx) ----------------------------------------------

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// ROM indices (bank * 0x4000 + offset & 0x3FFF) inside a padding ramp: $FF x16+ or $00 x33+.
std::set<uint64_t> ramp_bytes(const std::vector<uint8_t>& rom) {
    std::set<uint64_t> out;
    size_t i = 0, n = rom.size();
    while (i < n) {
        size_t j = i; uint8_t v = rom[i];
        while (j < n && rom[j] == v) j++;
        if ((v == 0xFF && j - i >= 16) || (v == 0x00 && j - i >= 33)) for (size_t k = i; k < j; k++) out.insert(k);
        i = j;
    }
    return out;
}

std::string bridge_profile(const std::string& sys, const fs::path& cov, const fs::path& rom, const fs::path& dst) {
    std::vector<uint8_t> image = read_file(rom);
    if (image.size() % 0x4000 == 512) image.erase(image.begin(), image.begin() + 512);
    std::set<uint64_t> ramps = ramp_bytes(image);
    const unsigned bank_shift = sys == "msx" ? 13 : 14;
    std::vector<std::string> body;
    std::vector<std::pair<uint64_t, uint32_t>> entries;
    size_t n = 0, dropped = 0;
    std::ifstream f(cov); std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line); std::string b, o, fl;
        if (!(in >> b >> o >> fl) || b[0] == '#') continue;
        unsigned bank = (unsigned)std::stoul(b, nullptr, 16), off = (unsigned)std::stoul(o, nullptr, 16);
        unsigned flags = (unsigned)std::stoul(fl, nullptr, 16);
        uint64_t hits = 1; in >> hits;
        if (!(flags & 1)) continue;
        if (ramps.count((uint64_t)bank << 14 | (off & 0x3FFF))) { dropped++; continue; }
        uint32_t loc = bank << 16 | off;
        char buf[48]; snprintf(buf, sizeof buf, "%06x %llu", loc, (unsigned long long)hits);
        body.push_back(buf);
        if (hits >= 8) entries.push_back({hits, loc});
        n++;
    }
    std::stable_sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a > b; });
    std::ofstream o(dst);
    o << "# m0boy." << sys << ".coverage.v2\n# bank-shift " << bank_shift << "\n# frames 600\n";
    std::set<uint32_t> seen; size_t seeds = 0;
    for (size_t i = 0; i < entries.size() && i < 5000; i++)
        if (seen.insert(entries[i].second).second) {
            char buf[32]; snprintf(buf, sizeof buf, "# entry %06x\n", entries[i].second);
            o << buf; seeds++;
        }
    for (auto& l : body) o << l << "\n";
    return std::to_string(n) + " executed pcs, " + std::to_string(seeds) + " entry seeds, " + std::to_string(dropped) + " padding-ramp heads dropped";
}

// ---- deepeel's map PNG: the embedded "deepeel_map" chunk (zTXt) ------------------------------------------

bool png_map(const fs::path& png, std::string& text) {
    std::vector<uint8_t> b = read_file(png);
    if (b.size() < 8) return false;
    size_t i = 8;
    while (i + 12 <= b.size()) {
        uint32_t n = (uint32_t)b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3];
        std::string type((const char*)&b[i + 4], 4);
        if (i + 12 + n > b.size()) break;
        if (type == "zTXt" && n > 13 && !strcmp((const char*)&b[i + 8], "deepeel_map")) {
            size_t k = 8 + strlen("deepeel_map") + 2;   // keyword, NUL, method
            uLongf cap = 1u << 24;
            std::string out(cap, 0);
            if (uncompress((Bytef*)out.data(), &cap, &b[i + k], (uLong)(n + 8 - k)) != Z_OK) return false;
            out.resize(cap); text = out; return true;
        }
        i += 12 + n;
    }
    return false;
}

// deepeel's text map (CPU-view addresses) -> physical-bank offsets (16 KiB, 0-3FFF), code classes only.
void physical_map(const std::string& raw, const fs::path& dst) {
    std::ofstream o(dst);
    o << "# physical-bank offset-start offset-end class\n";
    std::stringstream ss(raw); std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line); std::string b, l, h, k;
        if (!(in >> b >> l >> h >> k) || k.rfind("code-", 0) != 0) continue;
        unsigned bank = (unsigned)std::stoul(b, nullptr, 16);
        unsigned lo = (unsigned)std::stoul(l, nullptr, 16), hi = (unsigned)std::stoul(h, nullptr, 16);
        while (lo <= hi) {
            unsigned end = std::min(hi, lo | 0x3fffu);
            char buf[96]; snprintf(buf, sizeof buf, "%x %x %x %s\n", bank, lo & 0x3fff, end & 0x3fff, k.c_str());
            o << buf; lo = end + 1;
        }
    }
}

// ---- tar.gz -----------------------------------------------------------------------------------------------

struct Tar {
    gzFile gz;
    explicit Tar(const fs::path& p) { gz = gzopen(p.c_str(), "wb9"); if (!gz) die("cannot write " + p.string()); }
    void header(const std::string& name, uint64_t size, char type, unsigned mode) {
        char h[512] = {0};
        std::string pre, nm = name;
        if (nm.size() > 100) {   // ustar prefix split at a '/'
            size_t cut = nm.rfind('/', 155);
            while (cut != std::string::npos && nm.size() - cut - 1 > 100) cut = nm.rfind('/', cut - 1);
            if (cut == std::string::npos || cut == 0) return;
            pre = nm.substr(0, cut); nm = nm.substr(cut + 1);
        }
        memcpy(h, nm.data(), std::min<size_t>(nm.size(), 100));
        snprintf(h + 100, 8, "%07o", mode); snprintf(h + 108, 8, "%07o", 0); snprintf(h + 116, 8, "%07o", 0);
        snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
        snprintf(h + 136, 12, "%011llo", (unsigned long long)0);
        memset(h + 148, ' ', 8); h[156] = type; memcpy(h + 257, "ustar", 5); memcpy(h + 263, "00", 2);
        memcpy(h + 345, pre.data(), std::min<size_t>(pre.size(), 155));
        unsigned sum = 0; for (char c : h) sum += (unsigned char)c;
        snprintf(h + 148, 8, "%06o", sum); h[154] = 0; h[155] = ' ';
        gzwrite(gz, h, 512);
    }
    void add(const fs::path& src, const std::string& arc) {
        if (fs::is_directory(src)) {
            header(arc + "/", 0, '5', 0755);
            std::vector<fs::path> kids;
            for (auto& e : fs::directory_iterator(src)) kids.push_back(e.path());
            std::sort(kids.begin(), kids.end());
            for (auto& k : kids) {
                std::string nm = k.filename().string();
                if (nm == "build" || nm == ".pio") continue;   // build trees are not sources
                add(k, arc + "/" + nm);
            }
        } else if (fs::is_regular_file(src)) {
            std::vector<uint8_t> d = read_file(src);
            header(arc, d.size(), '0', executable(src) ? 0755 : 0644);
            if (!d.empty()) gzwrite(gz, d.data(), (unsigned)d.size());
            size_t pad = (512 - d.size() % 512) % 512;
            char z[512] = {0};
            if (pad) gzwrite(gz, z, (unsigned)pad);
        }
    }
    ~Tar() { char z[1024] = {0}; gzwrite(gz, z, 1024); gzclose(gz); }
};

void make_tar(const fs::path& out, const std::vector<std::pair<fs::path, std::string>>& items) {
    { Tar t(out); for (auto& [s, a] : items) t.add(s, a); }
    say("wrote " + out.string() + " (" + std::to_string(fs::file_size(out) / 1024) + " KiB)");
}

void usage() {
    std::cerr << "usage: m0boy ROM [target] [--out DIR] [--jobs N] [--no-deepeel] [--system S] [-- m0recomp options]\n"
                 "  target: pc (default: this machine), meta, pokitto, espboy, odroid, aka\n"
                 "  writes <ROM stem>.m0boy/<name>-<target>-release.tar.gz and -source.tar.gz\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string rom_arg, target, out_arg, forced;
    unsigned jobs = 0; bool no_deepeel = false;
    std::vector<std::string> extra;
    bool passthru = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (passthru) extra.push_back(a);
        else if (a == "--") passthru = true;
        else if (a == "--out" && i + 1 < argc) out_arg = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = (unsigned)atoi(argv[++i]);
        else if (a == "--system" && i + 1 < argc) forced = argv[++i];
        else if (a == "--no-deepeel") no_deepeel = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a.rfind("--", 0) == 0) extra.push_back(a);
        else if (rom_arg.empty()) rom_arg = a;
        else if (target.empty()) target = a;
        else extra.push_back(a);
    }
    if (rom_arg.empty()) { usage(); return 2; }
    fs::path rom = fs::absolute(rom_arg);
    if (!fs::is_regular_file(rom)) die(rom.string() + ": no such file");
    if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1);
    if (target.empty()) target = "pc";

    std::string system = detect_system(rom, forced);
    std::string name = rom.stem().string();
    fs::path out = out_arg.empty() ? rom.parent_path() / (name + ".m0boy") : fs::absolute(out_arg);
    fs::create_directories(out);
    say(rom.filename().string() + ": " + system + " -> " + target);

    // coverage: beside the ROM, else deepeel makes it
    auto locate = [&](const fs::path& dir, fs::path& cov, fs::path& png, fs::path& rt) {
        cov = dir / (name + ".cov");
        if (!fs::is_regular_file(cov)) { cov.clear(); return false; }
        png = dir / (name + ".map.png"); if (!fs::is_regular_file(png)) png.clear();
        rt = dir / (name + ".rtcode"); if (!fs::is_regular_file(rt)) rt.clear();
        return true;
    };
    fs::path cov, png, rt;
    fs::path ddir = rom.parent_path() / (name + ".deepeel");
    bool have = locate(ddir, cov, png, rt) || locate(rom.parent_path(), cov, png, rt);
    static const std::set<std::string> covered = {"gameboy", "nes", "pce", "megadrive", "sg1000", "gg", "sms", "msx"};
    if (have) say("coverage found: " + cov.string());
    else if (!no_deepeel && covered.count(system)) {
        std::vector<std::string> dargs = {rom.string(), "--out", ddir.string(), "--jobs", std::to_string(jobs)};
        int rc = -1;
#ifdef M0BOY_HAVE_DEEPEEL
        say("no coverage beside the ROM: running deepeel (built in) into " + ddir.string() + "/");
        rc = run_fn(deepeel_main, "deepeel", dargs);
#else
        fs::path deepeel = find_tool("deepeel", "DEEPEEL");
        if (!deepeel.empty()) {
            say("no coverage beside the ROM: running deepeel into " + ddir.string() + "/");
            dargs.insert(dargs.begin(), deepeel.string());
            rc = run(dargs);
        } else say("no coverage beside the ROM and no deepeel available (build with DEEPEEL_DIR or set DEEPEEL=/path): static walk only");
#endif
        if (rc > 0) die("deepeel failed (" + std::to_string(rc) + ")");
        if (rc == 0) have = locate(ddir, cov, png, rt);
    }

    fs::path project = out / "project";
    std::error_code ec;
    fs::remove_all(project, ec);
    std::string prefix = lower(name).substr(0, 24);
    for (auto& c : prefix) if (!isalnum((unsigned char)c)) c = '_';
    if (prefix.empty()) prefix = "game";
    std::vector<std::string> gen = {rom.string(), "--system", system, "--target", target,
                                    "--output-prefix", prefix, "-o", project.string(), "-j", std::to_string(jobs)};
    if (!png.empty()) { std::error_code e2; fs::copy_file(png, out / png.filename(), fs::copy_options::overwrite_existing, e2); }
    if (have && (system == "sg1000" || system == "gg" || system == "sms" || system == "msx")) {
        fs::path profile = out / (name + ".m0boy.cov");
        say(profile.string() + ": " + bridge_profile(system, cov, rom, profile));
        gen.insert(gen.end(), {"--coverage", profile.string()});
        std::string raw;
        if (!png.empty() && system != "msx" && png_map(png, raw)) {
            fs::path mp = out / (name + ".map");
            physical_map(raw, mp);
            gen.insert(gen.end(), {"--coverage-map", mp.string()});
        }
    } else if (system == "gameboy") {
        gen.push_back("--meta-static");
        if (!rt.empty()) gen.insert(gen.end(), {"--rtcode", rt.string()});
    } else if (system == "nes" || system == "pce") {
        gen.push_back("--meta-static");
    }
    gen.insert(gen.end(), extra.begin(), extra.end());
    if (int rc = m0recomp_call(gen)) die("m0recomp failed (" + std::to_string(rc) + ")");

    fs::path rel = out / (name + "-" + target + "-release");
    fs::remove_all(rel, ec);
    fs::create_directories(rel);
    bool shipped = false;
    if (target == "pc" && fs::is_regular_file(project / "CMakeLists.txt")) {
        fs::path b = project / "build";
        must({"cmake", "-S", project.string(), "-B", b.string(), "-DCMAKE_BUILD_TYPE=Release"}, true);
        must({"cmake", "--build", b.string(), "-j", std::to_string(jobs)}, true);
        for (auto& e : fs::directory_iterator(b))
            if (e.is_regular_file() && executable(e.path()) && (e.path().extension() == "" || e.path().extension() == ".exe")) {
                fs::copy_file(e.path(), rel / e.path().filename(), fs::copy_options::overwrite_existing); shipped = true;
            }
    } else if (target != "pc") {
        for (auto& e : fs::recursive_directory_iterator(project)) {
            std::string fn = e.path().filename().string(), full = e.path().string();
            if (e.is_regular_file() && (fn == "firmware.bin" || fn == "firmware.elf") && full.find("/src/") == std::string::npos) {
                fs::copy_file(e.path(), rel / fn, fs::copy_options::overwrite_existing); shipped = true;
            }
        }
    }
    if (!shipped) say("warning: no binary or firmware was produced for " + target + "; the project is in the source archive");
    if (!png.empty()) fs::copy_file(png, rel / png.filename(), fs::copy_options::overwrite_existing, ec);
    std::ofstream(rel / "README.txt") << name << " - " << system << " recompiled for " << target << " by m0boy\n"
        << "coverage: " << (have ? cov.filename().string() : std::string("none (static walk)")) << "\n"
        << "Controls and options: the in-game menu; states and saves go beside the executable.\n"
        << "Counts printed by the tools are observations of this build, not hardware measurements.\n";

    make_tar(out / (name + "-" + target + "-release.tar.gz"), {{rel, name + "-" + target + "-release"}});
    std::string root = name + "-" + target + "-source";
    std::vector<std::pair<fs::path, std::string>> items = {{project, root + "/project"}};
    if (have) items.push_back({cov, root + "/coverage/" + cov.filename().string()});
    if (!png.empty()) items.push_back({png, root + "/coverage/" + png.filename().string()});
    make_tar(out / (name + "-" + target + "-source.tar.gz"), items);
    say("done: " + out.string());
    return 0;
}
