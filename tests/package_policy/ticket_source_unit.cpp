#include <filesystem>
#include <iostream>
#include <fstream>
#include "core/file_sys/ticket_source.h"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::absolute(argv[1]);
    const auto package = root / "package";
    const auto own = root / "external-nand";
    const auto installed = root / "installed-nand";
    const auto package_nand = package / "user" / "nand";
    int failures = 0;
    int checks = 0, skipped = 0;
    const auto check = [&](const char* name, const auto& actual, const auto& expected) {
        ++checks;
        if (actual != expected) {
            std::cerr << name << ": got " << actual << ", expected " << expected << '\n';
            ++failures;
        }
    };
    check("configured external content", FileSys::SelectTicketSource(own, installed, package, false), own);
    check("borrowed installed content", FileSys::SelectTicketSource(package_nand, installed, package, true), installed);
    check("package content never supplies tickets", FileSys::SelectTicketSource(package_nand, installed, package, false), std::filesystem::path{});
    check("package fallback never supplies tickets", FileSys::SelectTicketSource(own, package_nand, package, true), std::filesystem::path{});
    check("normal configured NAND", FileSys::SelectTicketSource(own, {}, {}, false), own);
    check("normal fallback content", FileSys::SelectTicketSource(own, installed, {}, true), installed);
    const auto external_store = own / "system/tickets";
    const auto packaged_store = package_nand / "system/tickets";
    std::filesystem::create_directories(external_store);
    std::filesystem::create_directories(packaged_store);
    std::ofstream(packaged_store / "synthetic.tik") << "synthetic placeholder; no ticket or key";
    check("external ticket store", FileSys::CanReadInstalledTicketPath(external_store, package), true);
    check("external ticket file", FileSys::CanReadInstalledTicketPath(external_store / "own.tik", package), true);
    check("package ticket store", FileSys::CanReadInstalledTicketPath(packaged_store, package), false);
    check("package ticket file", FileSys::CanReadInstalledTicketPath(packaged_store / "synthetic.tik", package), false);
    check("non-export ticket path unchanged", FileSys::CanReadInstalledTicketPath(packaged_store, {}), true);
    std::error_code ec;
    const auto linked_store = own / "linked-system/tickets";
    std::filesystem::create_directories(linked_store.parent_path());
    std::filesystem::create_directory_symlink(packaged_store, linked_store, ec);
    if (ec) { ++skipped; std::cout << "SKIP nested store symlink: " << ec.message() << '\n'; }
    else {
        check("linked package ticket store", FileSys::CanReadInstalledTicketPath(linked_store, package), false);
        check("file through linked package store", FileSys::CanReadInstalledTicketPath(linked_store / "synthetic.tik", package), false);
    }
    ec.clear();
    const auto linked_ticket = external_store / "linked.tik";
    std::filesystem::create_symlink(packaged_store / "synthetic.tik", linked_ticket, ec);
    if (ec) { ++skipped; std::cout << "SKIP ticket file symlink: " << ec.message() << '\n'; }
    else check("linked package ticket file", FileSys::CanReadInstalledTicketPath(linked_ticket, package), false);
    std::cout << checks << " ticket-source checks, " << failures << " failures, " << skipped << " skips\n";
    return failures == 0 ? 0 : 1;
}
