#include "m0boy/system.h"
#include "m0boy/sg1000.h"
#include "m0boy/m0plus_generator.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using namespace m0boy;
    try {
        require(systems().size() >= 12, "Source registry incomplete");
        require(targets().size() == 7, "Target registry incomplete");
        require(parse_target("pc") == TargetId::pc && parse_target("odroid") == TargetId::odroid &&
                parse_target("aka") == TargetId::aka, "New targets not parsed");
        /* Hardware facts the capability matrix feeds on: consoles with 32 KiB
         * of SRAM, the ESPboy's 80 KiB, hosts unlimited. */
        require(target_descriptor(TargetId::meta).ram_bytes == 32768 &&
                target_descriptor(TargetId::pokitto).ram_bytes == 32768 &&
                target_descriptor(TargetId::espboy).ram_bytes == 81920 &&
                target_descriptor(TargetId::desktop).host && target_descriptor(TargetId::pc).host,
                "Target facts incorrect");
        require(target_descriptor(TargetId::odroid).psram_bytes == 4194304 &&
                target_descriptor(TargetId::aka).psram_bytes == 8388608,
                "ESP32-class targets need their PSRAM recorded");
        /* The matrix: a system runs on a target when a generator is wired AND
         * the runtime fits the hardware.  Every 32 KiB console target fits the
         * wired systems; the PC Engine's 64 KiB of video RAM excludes them and
         * the ESPboy alike. */
        for (auto id : {SystemId::gameboy, SystemId::sg1000, SystemId::gamegear,
                        SystemId::mastersystem, SystemId::msx, SystemId::pico8}) {
            const auto& system = system_descriptor(id);
            require(system.supports(TargetId::meta) && system.supports(TargetId::pokitto),
                    "Wired console target lost");
            require(target_fits(system, target_descriptor(TargetId::meta)),
                    "Wired system no longer fits the META");
        }
        const auto& pce = system_descriptor(SystemId::pce);
        require(!target_fits(pce, target_descriptor(TargetId::meta)) &&
                !target_fits(pce, target_descriptor(TargetId::pokitto)) &&
                !target_fits(pce, target_descriptor(TargetId::espboy)) &&
                target_fits(pce, target_descriptor(TargetId::odroid)) &&
                target_fits(pce, target_descriptor(TargetId::aka)) &&
                target_fits(pce, target_descriptor(TargetId::pc)),
                "PC Engine feasibility incorrect");
        require(!target_misfit_reason(pce, target_descriptor(TargetId::meta)).empty() &&
                target_misfit_reason(pce, target_descriptor(TargetId::pc)).empty(),
                "Misfit explanation incorrect");
        const auto& nes = system_descriptor(SystemId::nes);
        require(nes.supports(TargetId::pc) && nes.supports(TargetId::desktop) && nes.supports(TargetId::meta) &&
                nes.supports(TargetId::pokitto) && target_fits(nes, target_descriptor(TargetId::meta)) &&
                target_fits(nes, target_descriptor(TargetId::pokitto)),
                "NES must be wired for the desktop harness, PC, META and Pokitto");
        const auto& vx = system_descriptor(SystemId::vectrex);
        require(vx.cpu == CpuId::m6809 && vx.work_ram_bytes == 1024 &&
                vx.supports(TargetId::pc) && vx.supports(TargetId::meta) &&
                target_fits(vx, target_descriptor(TargetId::meta)) && !vx.supports(TargetId::pokitto) &&
                detect_system("game.VEC") == SystemId::vectrex,
                "Vectrex must be wired for the PC and the META");
        const auto& md = system_descriptor(SystemId::megadrive);
        require(md.cpu == CpuId::m68k && md.supports(TargetId::desktop) && !md.supports(TargetId::meta) &&
                md.supports(TargetId::pc) && md.supports(TargetId::aka) && !md.supports(TargetId::pokitto) &&
                detect_system("game.MD") == SystemId::megadrive,
                "Mega Drive bounded M68k profile must be wired for desktop, PC and AKA, not the META");
        require(detect_system("game.sms") == SystemId::mastersystem && parse_system("sms") == SystemId::mastersystem,
                "Master System detection failed");
        require(detect_system("game.nes") == SystemId::nes && detect_system("game.PCE") == SystemId::pce,
                "NES / PC Engine detection failed");
        const auto& gg = system_descriptor(SystemId::gamegear);
        require(gg.cpu == CpuId::z80 && gg.work_ram_bytes == 8192 && parse_system("gg") == SystemId::gamegear,
                "Game Gear profile incorrect");
        require(detect_system("game.GG") == SystemId::gamegear, "GG detection failed");
        const auto& msx = system_descriptor(SystemId::msx);
        require(msx.cpu == CpuId::z80 && msx.reset_address == 0x4000 && parse_system("msx") == SystemId::msx &&
                    detect_system("game.MX1") == SystemId::msx && msx.supports(TargetId::meta) &&
                    msx.supports(TargetId::pokitto) && msx.supports(TargetId::pc) &&
                    !msx.supports(TargetId::espboy),
                "MSX profile incorrect");
        require(system_descriptor(SystemId::pico8).supports(TargetId::pc),
                "PICO-8 PC target missing");
        const auto& sg = system_descriptor(SystemId::sg1000);
        require(sg.cpu == CpuId::z80 && sg.reset_address == 0 &&
                sg.work_ram_bytes == 1024, "SG-1000 profile incorrect");
        require(cpu_descriptor(sg.cpu).decoder_available, "Z80 decoder missing");
        for (auto target : {TargetId::desktop, TargetId::meta, TargetId::pokitto}) {
            require(sg.supports(target), "SG-1000 target missing");
            require(system_descriptor(SystemId::gameboy).supports(target), "Lost GB target");
        }
        require(detect_system("game.SG") == SystemId::sg1000, "SG detection failed");
        require(detect_system("game.sgb") == SystemId::gameboy, "SGB is a GB cartridge");
        require(!detect_system("game.bin"), "Raw binary falsely identified");
        require(!parse_cpu("arm") && !parse_system("unknown"), "Unknown ids accepted");
        require(parse_cpu("m0plus") == CpuId::m0plus &&
                system_descriptor(SystemId::meta).cpu == CpuId::m0plus &&
                system_descriptor(SystemId::pokitto).cpu == CpuId::m0plus &&
                system_descriptor(SystemId::meta).supports(TargetId::pc) &&
                system_descriptor(SystemId::pokitto).supports(TargetId::pc) &&
                system_descriptor(SystemId::meta).supports(TargetId::aka) &&
                !system_descriptor(SystemId::pokitto).supports(TargetId::aka) &&
                !system_descriptor(SystemId::pokitto).supports(TargetId::meta) &&
                !detect_system("firmware.bin"), "M0+ source profiles incorrect");
        std::string error;
        {
            const auto root = std::filesystem::temp_directory_path() /
                ("m0boy_m0plus_project_test_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root);
            const auto firmware = root / "source.bin";
            {
                std::ofstream image(firmware, std::ios::binary);
                const unsigned char bytes[] = {0, 0x80, 0, 0x20, 9, 0x40, 0, 0, 0, 0xbf};
                image.write(reinterpret_cast<const char*>(bytes), sizeof bytes);
            }
            const auto project = root / "pc";
            require(m0plus::generate(firmware, SystemId::meta, project, error),
                    "Valid META firmware project rejected");
            require(std::filesystem::file_size(project / "firmware.bin") == 10 &&
                    std::filesystem::exists(project / "run.py"), "META PC project incomplete");
            require(!m0plus::generate(firmware, SystemId::pokitto, project, error),
                    "Project destination unexpectedly changed boards");
            std::filesystem::remove_all(root);
        }
        require(!sg1000::Cartridge::from_bytes({}, error) && !error.empty(), "Empty ROM accepted");
        require(!sg1000::Cartridge::from_bytes(std::vector<uint8_t>(0xc001), error),
                "Unsupported mapper accepted");
        auto cartridge = sg1000::Cartridge::from_bytes({0xc3, 0x34, 0x12}, error);
        require(cartridge && error.empty(), "Headerless SG cartridge rejected");
        require(cartridge->read_banked(0, 0) == 0xc3 &&
                cartridge->read_banked(0, 2) == 0x12 &&
                cartridge->read_banked(0, 3) == 0xff &&
                cartridge->read_banked(1, 0) == 0xff &&
                cartridge->read_banked(0, 0xc000) == 0xff, "Static reader mapping incorrect");
        auto largest = sg1000::Cartridge::from_bytes(std::vector<uint8_t>(0xc000, 0xa5), error);
        require(largest && largest->read_banked(0, 0xbfff) == 0xa5, "48 KiB boundary rejected");
        /* a 512-byte copier header is not part of the cartridge (one rule:
         * sg1000.h; the host harnesses repeat it in m0_host_util.h) */
        require(sg1000::copier_header_skip(0x4000) == 0 && sg1000::copier_header_skip(0x4000 + 512) == 512 &&
                    sg1000::copier_header_skip(0xc000 + 512) == 512, "copier header rule incorrect");
        {
            const auto path = std::filesystem::temp_directory_path() / "m0boy_copier_header_test.sg";
            {
                std::vector<uint8_t> bytes(512, 0x5a);
                bytes.resize(0x4000 + 512, 0xa5);
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            }
            auto with_header = sg1000::Cartridge::load(path, error);
            require(with_header && with_header->bytes().size() == 0x4000 &&
                        with_header->read_banked(0, 0) == 0xa5 && with_header->read_banked(0, 0x3fff) == 0xa5,
                    "SG cartridge with a copier header misread");
            auto gg = sg1000::inspect_gamegear(path, error);
            require(gg && gg->rom_bytes == 0x4000, "Game Gear inspection keeps the copier header");
            auto sms = sg1000::inspect_mastersystem(path, error);
            require(sms && sms->rom_bytes == 0x4000 && sms->system == SystemId::mastersystem,
                    "Master System inspection keeps the copier header");
            std::filesystem::remove(path);
        }
        {   /* MSX: "AB" header, INIT inside the cartridge; the page follows INIT */
            const auto path = std::filesystem::temp_directory_path() / "m0boy_msx_test.mx1";
            auto write = [&](std::vector<uint8_t> bytes) {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            };
            std::vector<uint8_t> rom(0x4000, 0xff);
            rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x40;
            write(rom);
            auto low = sg1000::inspect_msx(path, error);
            require(low && low->system == SystemId::msx && low->rom_bytes == 0x4000 && low->mapper == "unbanked 4000-BFFF",
                    "MSX 4000 cartridge misread");
            rom[3] = 0x80; /* INIT at 8000: a 16 KiB ROM sits in page 2 */
            write(rom);
            auto high = sg1000::inspect_msx(path, error);
            require(high && high->mapper == "unbanked 8000-BFFF", "MSX 8000 cartridge misread");
            rom[3] = 0xc0; /* INIT outside the cartridge */
            write(rom);
            require(!sg1000::inspect_msx(path, error) && !error.empty(), "MSX INIT outside the ROM accepted");
            rom[0] = 'X';
            write(rom);
            require(!sg1000::inspect_msx(path, error), "MSX cartridge without header accepted");
            write(std::vector<uint8_t>(0x8001, 0));
            require(!sg1000::inspect_msx(path, error), "MSX cartridge without header accepted");
            std::vector<uint8_t> big(0x40000, 0xff); /* 256 KiB, writes to 8000 and A000: Konami */
            big[0] = 'A'; big[1] = 'B'; big[2] = 0x31; big[3] = 0x40;
            for (size_t at = 0x100; at < 0x120; at += 8) { big[at] = 0x32; big[at + 1] = 0x00; big[at + 2] = 0x80; big[at + 3] = 0x32; big[at + 4] = 0x00; big[at + 5] = 0xa0; }
            write(big);
            auto mapped = sg1000::inspect_msx(path, error);
            require(mapped && mapped->mapper == "konami" && mapped->rom_bytes == 0x40000, "MSX Konami cartridge misread");
            big[0x101] = 0x00; big[0x102] = 0x70; big[0x104] = 0x00; big[0x105] = 0x70; big[0x109] = 0x00; big[0x10a] = 0x70; big[0x10c] = 0x00; big[0x10d] = 0x70;
            big[0x111] = 0x00; big[0x112] = 0x70; big[0x114] = 0x00; big[0x115] = 0x70; big[0x119] = 0x00; big[0x11a] = 0x70; big[0x11c] = 0x00; big[0x11d] = 0x70;
            write(big);
            mapped = sg1000::inspect_msx(path, error);
            require(mapped && mapped->mapper == "ascii16", "MSX ASCII16 cartridge misread");
            std::filesystem::remove(path);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
