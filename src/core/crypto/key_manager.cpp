// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/crypto/partition_data_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/ticket_source.h"
#include "core/loader/loader.h"

namespace Core::Crypto {
namespace {

constexpr u64 CURRENT_CRYPTO_REVISION = 0x5;

constexpr std::array<std::pair<std::string_view, KeyIndex<S128KeyType>>, 30> s128_file_id{{
    {"eticket_rsa_kek", {S128KeyType::ETicketRSAKek, 0, 0}},
    {"eticket_rsa_kek_source", {S128KeyType::Source, u64(SourceKeyType::ETicketKek), 0}},
    {"eticket_rsa_kekek_source", {S128KeyType::Source, u64(SourceKeyType::ETicketKekek), 0}},
    {"rsa_kek_mask_0", {S128KeyType::RSAKek, u64(RSAKekType::Mask0), 0}},
    {"rsa_kek_seed_3", {S128KeyType::RSAKek, u64(RSAKekType::Seed3), 0}},
    {"rsa_oaep_kek_generation_source", {S128KeyType::Source, u64(SourceKeyType::RSAOaepKekGeneration), 0}},
    {"sd_card_kek_source", {S128KeyType::Source, u64(SourceKeyType::SDKek), 0}},
    {"aes_kek_generation_source", {S128KeyType::Source, u64(SourceKeyType::AESKekGeneration), 0}},
    {"aes_key_generation_source", {S128KeyType::Source, u64(SourceKeyType::AESKeyGeneration), 0}},
    {"package2_key_source", {S128KeyType::Source, u64(SourceKeyType::Package2), 0}},
    {"master_key_source", {S128KeyType::Source, u64(SourceKeyType::Master), 0}},
    {"header_kek_source", {S128KeyType::Source, u64(SourceKeyType::HeaderKek), 0}},
    {"key_area_key_application_source", {S128KeyType::Source, u64(SourceKeyType::KeyAreaKey), u64(KeyAreaKeyType::Application)}},
    {"key_area_key_ocean_source", {S128KeyType::Source, u64(SourceKeyType::KeyAreaKey), u64(KeyAreaKeyType::Ocean)}},
    {"key_area_key_system_source", {S128KeyType::Source, u64(SourceKeyType::KeyAreaKey), u64(KeyAreaKeyType::System)}},
    {"titlekek_source", {S128KeyType::Source, u64(SourceKeyType::Titlekek), 0}},
    {"keyblob_mac_key_source", {S128KeyType::Source, u64(SourceKeyType::KeyblobMAC), 0}},
    {"tsec_key", {S128KeyType::TSEC, 0, 0}},
    {"secure_boot_key", {S128KeyType::SecureBoot, 0, 0}},
    {"sd_seed", {S128KeyType::SDSeed, 0, 0}},
    {"bis_key_0_crypt", {S128KeyType::BIS, 0, u64(BISKeyType::Crypto)}},
    {"bis_key_0_tweak", {S128KeyType::BIS, 0, u64(BISKeyType::Tweak)}},
    {"bis_key_1_crypt", {S128KeyType::BIS, 1, u64(BISKeyType::Crypto)}},
    {"bis_key_1_tweak", {S128KeyType::BIS, 1, u64(BISKeyType::Tweak)}},
    {"bis_key_2_crypt", {S128KeyType::BIS, 2, u64(BISKeyType::Crypto)}},
    {"bis_key_2_tweak", {S128KeyType::BIS, 2, u64(BISKeyType::Tweak)}},
    {"bis_key_3_crypt", {S128KeyType::BIS, 3, u64(BISKeyType::Crypto)}},
    {"bis_key_3_tweak", {S128KeyType::BIS, 3, u64(BISKeyType::Tweak)}},
    {"header_kek", {S128KeyType::HeaderKek, 0, 0}},
    {"sd_card_kek", {S128KeyType::SDKek, 0, 0}},
}};

auto Find128ByName(std::string_view name) {
    return std::find_if(s128_file_id.begin(), s128_file_id.end(), [&name](const auto& pair) {
        return pair.first == name;
    });
}

constexpr std::array<std::pair<std::string_view, KeyIndex<S256KeyType>>, 6> s256_file_id{{
    {"header_key", {S256KeyType::Header, 0, 0}},
    {"sd_card_save_key_source", {S256KeyType::SDKeySource, u64(SDKeyType::Save), 0}},
    {"sd_card_nca_key_source", {S256KeyType::SDKeySource, u64(SDKeyType::NCA), 0}},
    {"header_key_source", {S256KeyType::HeaderSource, 0, 0}},
    {"sd_card_save_key", {S256KeyType::SDKey, u64(SDKeyType::Save), 0}},
    {"sd_card_nca_key", {S256KeyType::SDKey, u64(SDKeyType::NCA), 0}},
}};

auto Find256ByName(std::string_view name) {
    return std::find_if(s256_file_id.begin(), s256_file_id.end(), [&name](const auto& pair) { return pair.first == name; });
}

using KeyArray = std::array<std::pair<std::pair<S128KeyType, u64>, std::string_view>, 7>;
constexpr KeyArray KEYS_VARIABLE_LENGTH{{
    {{S128KeyType::Master, 0}, "master_key_"},
    {{S128KeyType::Package1, 0}, "package1_key_"},
    {{S128KeyType::Package2, 0}, "package2_key_"},
    {{S128KeyType::Titlekek, 0}, "titlekek_"},
    {{S128KeyType::Source, u64(SourceKeyType::Keyblob)}, "keyblob_key_source_"},
    {{S128KeyType::Keyblob, 0}, "keyblob_key_"},
    {{S128KeyType::KeyblobMAC, 0}, "keyblob_mac_key_"},
}};

template <std::size_t Size>
bool IsAllZeroArray(const std::array<u8, Size>& array) {
    return std::all_of(array.begin(), array.end(), [](const auto& elem) { return elem == 0; });
}
} // Anonymous namespace

u64 GetSignatureTypeDataSize(SignatureType type) {
    switch (type) {
    case SignatureType::RSA_4096_SHA1:
    case SignatureType::RSA_4096_SHA256:
        return 0x200;
    case SignatureType::RSA_2048_SHA1:
    case SignatureType::RSA_2048_SHA256:
        return 0x100;
    case SignatureType::ECDSA_SHA1:
    case SignatureType::ECDSA_SHA256:
        return 0x3C;
    }
    UNREACHABLE();
}

u64 GetSignatureTypePaddingSize(SignatureType type) {
    switch (type) {
    case SignatureType::RSA_4096_SHA1:
    case SignatureType::RSA_4096_SHA256:
    case SignatureType::RSA_2048_SHA1:
    case SignatureType::RSA_2048_SHA256:
        return 0x3C;
    case SignatureType::ECDSA_SHA1:
    case SignatureType::ECDSA_SHA256:
        return 0x40;
    }
    UNREACHABLE();
}

bool Ticket::IsValid() const {
    return !std::holds_alternative<std::monostate>(data);
}

SignatureType Ticket::GetSignatureType() const {
    if (const auto* ticket = std::get_if<RSA4096Ticket>(&data))
        return ticket->sig_type;
    if (const auto* ticket = std::get_if<RSA2048Ticket>(&data))
        return ticket->sig_type;
    if (const auto* ticket = std::get_if<ECDSATicket>(&data))
        return ticket->sig_type;
    UNREACHABLE();
}

TicketData& Ticket::GetData() {
    if (auto* ticket = std::get_if<RSA4096Ticket>(&data))
        return ticket->data;
    if (auto* ticket = std::get_if<RSA2048Ticket>(&data))
        return ticket->data;
    if (auto* ticket = std::get_if<ECDSATicket>(&data))
        return ticket->data;
    UNREACHABLE();
}

const TicketData& Ticket::GetData() const {
    if (const auto* ticket = std::get_if<RSA4096Ticket>(&data))
        return ticket->data;
    if (const auto* ticket = std::get_if<RSA2048Ticket>(&data))
        return ticket->data;
    if (const auto* ticket = std::get_if<ECDSATicket>(&data))
        return ticket->data;
    UNREACHABLE();
}

u64 Ticket::GetSize() const {
    const auto sig_type = GetSignatureType();

    return sizeof(SignatureType) + GetSignatureTypeDataSize(sig_type) + GetSignatureTypePaddingSize(sig_type) + sizeof(TicketData);
}

Ticket Ticket::SynthesizeCommon(Key128 title_key, const std::array<u8, 16>& rights_id) {
    RSA2048Ticket out{};
    out.sig_type = SignatureType::RSA_2048_SHA256;
    out.data.rights_id = rights_id;
    out.data.title_key_common = title_key;
    return Ticket{out};
}

Ticket Ticket::Read(const FileSys::VirtualFile& file) {
    // Attempt to read up to the largest ticket size, and make sure we read at least a signature
    // type.
    std::array<u8, sizeof(RSA4096Ticket)> raw_data{};
    auto read_size = file->Read(raw_data.data(), raw_data.size(), 0);
    if (read_size < sizeof(SignatureType)) {
        LOG_WARNING(Crypto, "Attempted to read ticket file with invalid size {}.", read_size);
        return Ticket{std::monostate()};
    }
    return Read(std::span{raw_data});
}

Ticket Ticket::Read(std::span<const u8> raw_data) {
    // Some tools read only 0x180 bytes of ticket data instead of 0x2C0, so
    // just make sure we have at least the bare minimum of data to work with.
    SignatureType sig_type;
    if (raw_data.size() < sizeof(SignatureType)) {
        LOG_WARNING(Crypto, "Attempted to parse ticket buffer with invalid size {}.", raw_data.size());
        return Ticket{std::monostate()};
    }
    std::memcpy(&sig_type, raw_data.data(), sizeof(sig_type));

    switch (sig_type) {
    case SignatureType::RSA_4096_SHA1:
    case SignatureType::RSA_4096_SHA256: {
        RSA4096Ticket ticket{};
        std::memcpy(&ticket, raw_data.data(), sizeof(ticket));
        return Ticket{ticket};
    }
    case SignatureType::RSA_2048_SHA1:
    case SignatureType::RSA_2048_SHA256: {
        RSA2048Ticket ticket{};
        std::memcpy(&ticket, raw_data.data(), sizeof(ticket));
        return Ticket{ticket};
    }
    case SignatureType::ECDSA_SHA1:
    case SignatureType::ECDSA_SHA256: {
        ECDSATicket ticket{};
        std::memcpy(&ticket, raw_data.data(), sizeof(ticket));
        return Ticket{ticket};
    }
    default:
        LOG_WARNING(Crypto, "Attempted to parse ticket buffer with invalid type {}.", sig_type);
        return Ticket{std::monostate()};
    }
}

Key128 GenerateKeyEncryptionKey(Key128 source, Key128 master, Key128 kek_seed, Key128 key_seed) {
    Key128 out{};
    AESCipher<Key128> cipher1(master, Mode::ECB);
    cipher1.Transcode(kek_seed.data(), kek_seed.size(), out.data(), Op::Decrypt);
    AESCipher<Key128> cipher2(out, Mode::ECB);
    cipher2.Transcode(source.data(), source.size(), out.data(), Op::Decrypt);
    if (key_seed != Key128{}) {
        AESCipher<Key128> cipher3(out, Mode::ECB);
        cipher3.Transcode(key_seed.data(), key_seed.size(), out.data(), Op::Decrypt);
    }
    return out;
}

Loader::ResultStatus DeriveSDKeys(std::array<Key256, 2>& sd_keys, KeyManager& keys) {
    if (!keys.HasKey(S128KeyType::Source, u64(SourceKeyType::SDKek))) {
        return Loader::ResultStatus::ErrorMissingSDKEKSource;
    }
    if (!keys.HasKey(S128KeyType::Source, u64(SourceKeyType::AESKekGeneration))) {
        return Loader::ResultStatus::ErrorMissingAESKEKGenerationSource;
    }
    if (!keys.HasKey(S128KeyType::Source, u64(SourceKeyType::AESKeyGeneration))) {
        return Loader::ResultStatus::ErrorMissingAESKeyGenerationSource;
    }

    const auto sd_kek_source = keys.GetKey(S128KeyType::Source, u64(SourceKeyType::SDKek));
    const auto aes_kek_gen = keys.GetKey(S128KeyType::Source, u64(SourceKeyType::AESKekGeneration));
    const auto aes_key_gen = keys.GetKey(S128KeyType::Source, u64(SourceKeyType::AESKeyGeneration));
    const auto master_00 = keys.GetKey(S128KeyType::Master);
    const auto sd_kek = GenerateKeyEncryptionKey(sd_kek_source, master_00, aes_kek_gen, aes_key_gen);
    keys.SetKey(S128KeyType::SDKek, sd_kek);

    if (!keys.HasKey(S128KeyType::SDSeed)) {
        return Loader::ResultStatus::ErrorMissingSDSeed;
    }
    const auto sd_seed = keys.GetKey(S128KeyType::SDSeed);

    if (!keys.HasKey(S256KeyType::SDKeySource, u64(SDKeyType::Save))) {
        return Loader::ResultStatus::ErrorMissingSDSaveKeySource;
    }
    if (!keys.HasKey(S256KeyType::SDKeySource, u64(SDKeyType::NCA))) {
        return Loader::ResultStatus::ErrorMissingSDNCAKeySource;
    }

    std::array<Key256, 2> sd_key_sources{
        keys.GetKey(S256KeyType::SDKeySource, u64(SDKeyType::Save)),
        keys.GetKey(S256KeyType::SDKeySource, u64(SDKeyType::NCA)),
    };

    // Combine sources and seed
    for (auto& source : sd_key_sources) {
        for (std::size_t i = 0; i < source.size(); ++i) {
            source[i] = static_cast<u8>(source[i] ^ sd_seed[i & 0xF]);
        }
    }

    AESCipher<Key128> cipher(sd_kek, Mode::ECB);
    // The transform manipulates sd_keys as part of the Transcode, so the return/output is
    // unnecessary. This does not alter sd_keys_sources.
    std::transform(sd_key_sources.begin(), sd_key_sources.end(), sd_keys.begin(), sd_key_sources.begin(), [&cipher](const Key256& source, Key256& out) {
        cipher.Transcode(source.data(), source.size(), out.data(), Op::Decrypt);
        return source; ///< Return unaltered source to satisfy output requirement.
    });

    keys.SetKey(S256KeyType::SDKey, sd_keys[0], u64(SDKeyType::Save));
    keys.SetKey(S256KeyType::SDKey, sd_keys[1], u64(SDKeyType::NCA));
    return Loader::ResultStatus::Success;
}

std::vector<Ticket> GetTicketblob(const Common::FS::IOFile& ticket_save) {
    if (!ticket_save.IsOpen()) {
        return {};
    }

    std::vector<u8> buffer(ticket_save.GetSize());
    if (ticket_save.Read(buffer) != buffer.size()) {
        return {};
    }

    std::vector<Ticket> out;
    for (std::size_t offset = 0; offset + 0x4 < buffer.size(); ++offset) {
        if (buffer[offset] == 0x4 && buffer[offset + 1] == 0x0 && buffer[offset + 2] == 0x1 &&
            buffer[offset + 3] == 0x0) {
            // NOTE: Assumes ticket blob will only contain RSA-2048 tickets.
            auto ticket = Ticket::Read(std::span{buffer.data() + offset, sizeof(RSA2048Ticket)});
            offset += sizeof(RSA2048Ticket);
            if (ticket.IsValid()) {
                out.push_back(ticket);
            }
        }
    }

    return out;
}

template <size_t size>
static std::array<u8, size> operator^(const std::array<u8, size>& lhs, const std::array<u8, size>& rhs) {
    std::array<u8, size> out;
    std::transform(lhs.begin(), lhs.end(), rhs.begin(), out.begin(), [](u8 lhs_elem, u8 rhs_elem) { return u8(lhs_elem ^ rhs_elem); });
    return out;
}

template <size_t target_size, size_t in_size>
static std::array<u8, target_size> MGF1(const std::array<u8, in_size>& seed) {
    // Avoids truncation overflow within the loop below.
    static_assert(target_size <= 0xFF);

    std::array<u8, in_size + 4> seed_exp{};
    std::memcpy(seed_exp.data(), seed.data(), in_size);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const EVP_MD* sha256 = EVP_sha256();

    std::vector<u8> out;
    size_t i = 0;
    while (out.size() < target_size) {
        size_t offset = out.size();
        out.resize(offset + 0x20);
        seed_exp[in_size + 3] = u8(i);

        u32 hash_len = 0;

        EVP_DigestInit_ex(ctx, sha256, nullptr);
        EVP_DigestUpdate(ctx, seed_exp.data(), seed_exp.size());
        EVP_DigestFinal_ex(ctx, out.data() + offset, &hash_len);

        ++i;
    }

    EVP_MD_CTX_free(ctx);

    std::array<u8, target_size> target;
    std::memcpy(target.data(), out.data(), target_size);
    return target;
}

template <size_t size>
static std::optional<u64> FindTicketOffset(const std::array<u8, size>& data) {
    u64 offset = 0;
    for (size_t i = 0x20; i < data.size() - 0x10; ++i) {
        if (data[i] == 0x1) {
            offset = i + 1;
            break;
        } else if (data[i] != 0x0) {
            return std::nullopt;
        }
    }

    return offset;
}

std::optional<Key128> KeyManager::ParseTicketTitleKey(const Ticket& ticket) {
    if (!ticket.IsValid()) {
        LOG_WARNING(Crypto, "Attempted to parse title key of invalid ticket.");
        return std::nullopt;
    }

    if (ticket.GetData().rights_id == Key128{}) {
        LOG_WARNING(Crypto, "Attempted to parse title key of ticket with no rights ID.");
        return std::nullopt;
    }

    const auto issuer = ticket.GetData().issuer;
    if (IsAllZeroArray(issuer)) {
        LOG_WARNING(Crypto, "Attempted to parse title key of ticket with invalid issuer.");
        return std::nullopt;
    }

    if (issuer[0] != 'R' || issuer[1] != 'o' || issuer[2] != 'o' || issuer[3] != 't') {
        LOG_WARNING(Crypto, "Parsing ticket with non-standard certificate authority.");
    }

    if (ticket.GetData().type == TitleKeyType::Common) {
        return ticket.GetData().title_key_common;
    }

    if (eticket_rsa_keypair == RSAKeyPair<2048>{}) {
        LOG_WARNING(
            Crypto,
            "Skipping personalized ticket title key parsing due to missing ETicket RSA key-pair.");
        return std::nullopt;
    }

    std::array<u8, 0x100> rsa_step;
    {
        // Private context for OpenSSL bignumbers
        // Inside block because I dont wanna pollute the space...
        const auto& title_key_block = ticket.GetData().title_key_block;
        BIGNUM* D = BN_bin2bn(eticket_rsa_keypair.decryption_key.data(), int(eticket_rsa_keypair.decryption_key.size()), NULL);
        BIGNUM* N = BN_bin2bn(eticket_rsa_keypair.modulus.data(), int(eticket_rsa_keypair.modulus.size()), NULL);
        BIGNUM* S = BN_bin2bn(title_key_block.data(), int(title_key_block.size()), NULL);
        BIGNUM* M = BN_new();
        // M = S ^ D mod N
        BN_mod_exp(M, S, D, N, NULL);
        BN_bn2bin(M, rsa_step.data());
        BN_free(D);
        BN_free(N);
        BN_free(S);
        BN_free(M);
    }

    u8 m_0 = rsa_step[0];
    std::array<u8, 0x20> m_1;
    std::array<u8, 0xDF> m_2;
    std::memcpy(m_1.data(), rsa_step.data() + 0x01, m_1.size());
    std::memcpy(m_2.data(), rsa_step.data() + 0x21, m_2.size());

    if (m_0 != 0) {
        return std::nullopt;
    }

    m_1 = m_1 ^ MGF1<0x20>(m_2);
    m_2 = m_2 ^ MGF1<0xDF>(m_1);

    const auto offset = FindTicketOffset(m_2);
    if (!offset) {
        return std::nullopt;
    }
    ASSERT(*offset > 0);

    Key128 key_temp{};
    std::memcpy(key_temp.data(), m_2.data() + *offset, key_temp.size());
    return key_temp;
}

KeyManager::KeyManager() {
    ReloadKeys();
}

void KeyManager::ReloadKeys() {
    // Initialize keys
    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
    if (!Common::FS::CreateDir(keys_dir))
        LOG_ERROR(Core, "Failed to create the keys directory.");
    if (Settings::values.use_dev_keys.GetValue()) {
        dev_mode = true;
        LoadFromFile(keys_dir / "dev.keys_autogenerated", false);
        LoadFromFile(keys_dir / "dev.keys", false);
    } else {
        dev_mode = false;
        LoadFromFile(keys_dir / "prod.keys_autogenerated", false);
        LoadFromFile(keys_dir / "prod.keys", false);
    }
    LoadFromFile(keys_dir / "title.keys_autogenerated", true);
    LoadFromFile(keys_dir / "title.keys", true);
    LoadFromFile(keys_dir / "console.keys_autogenerated", false);
    LoadFromFile(keys_dir / "console.keys", false);
}

static bool ValidCryptoRevisionString(std::string_view base, size_t begin, size_t length) {
    if (base.size() < begin + length)
        return false;
    return std::all_of(base.begin() + begin, base.begin() + begin + length, [](u8 c) { return std::isxdigit(c); });
}

void KeyManager::LoadFromFile(const std::filesystem::path& file_path, bool is_title_keys) {
    if (!Common::FS::Exists(file_path)) {
        return;
    }

    std::ifstream file;
    Common::FS::OpenFileStream(file, file_path, std::ios_base::in);

    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::vector<std::string> out;
            std::stringstream stream(line);
            std::string item;
            while (std::getline(stream, item, '=')) {
                out.push_back(std::move(item));
            }

            if (out.size() != 2) {
                continue;
            }

            out[0].erase(std::remove(out[0].begin(), out[0].end(), ' '), out[0].end());
            out[1].erase(std::remove(out[1].begin(), out[1].end(), ' '), out[1].end());

            if (out[0].compare(0, 1, "#") == 0) {
                continue;
            }

            if (is_title_keys) {
                auto rights_id_raw = Common::HexStringToArray<16>(out[0]);
                u128 rights_id{};
                std::memcpy(rights_id.data(), rights_id_raw.data(), rights_id_raw.size());
                Key128 key = Common::HexStringToArray<16>(out[1]);
                s128_keys[{S128KeyType::Titlekey, rights_id[1], rights_id[0]}] = key;
            } else {
                out[0] = Common::ToLower(out[0]);
                if (const auto iter128 = Find128ByName(out[0]); iter128 != s128_file_id.end()) {
                    const auto& index = iter128->second;
                    const Key128 key = Common::HexStringToArray<16>(out[1]);
                    s128_keys[{index.type, index.field1, index.field2}] = key;
                } else if (const auto iter256 = Find256ByName(out[0]); iter256 != s256_file_id.end()) {
                    const auto& index = iter256->second;
                    const Key256 key = Common::HexStringToArray<32>(out[1]);
                    s256_keys[{index.type, index.field1, index.field2}] = key;
                } else if (out[0].compare(0, 19, "eticket_rsa_keypair") == 0) {
                    const auto key_data = Common::HexStringToArray<528>(out[1]);
                    std::memcpy(eticket_rsa_keypair.decryption_key.data(), key_data.data(), eticket_rsa_keypair.decryption_key.size());
                    std::memcpy(eticket_rsa_keypair.modulus.data(), key_data.data() + 0x100, eticket_rsa_keypair.modulus.size());
                    std::memcpy(eticket_rsa_keypair.exponent.data(), key_data.data() + 0x200, eticket_rsa_keypair.exponent.size());
                } else {
                    for (const auto& kv : KEYS_VARIABLE_LENGTH) {
                        if (!ValidCryptoRevisionString(out[0], kv.second.size(), 2)) {
                            continue;
                        }
                        if (out[0].compare(0, kv.second.size(), kv.second) == 0) {
                            const auto index = std::strtoul(out[0].substr(kv.second.size(), 2).c_str(), nullptr, 16);
                            const auto sub = kv.first.second;
                            if (sub == 0) {
                                s128_keys[{kv.first.first, index, 0}] = Common::HexStringToArray<16>(out[1]);
                            } else {
                                s128_keys[{kv.first.first, kv.first.second, index}] = Common::HexStringToArray<16>(out[1]);
                            }
                            break;
                        }
                    }

                    constexpr std::array<const char*, 3> kak_names = {
                        "key_area_key_application_", "key_area_key_ocean_", "key_area_key_system_"
                    };
                    for (size_t j = 0; j < kak_names.size(); ++j) {
                        const auto& match = kak_names[j];
                        if (out[0].compare(0, std::strlen(match), match) == 0) {
                            const auto index = std::strtoul(out[0].substr(std::strlen(match), 2).c_str(), nullptr, 16);
                            s128_keys[{S128KeyType::KeyArea, index, j}] = Common::HexStringToArray<16>(out[1]);
                        }
                    }
                }
            }
        }
    }
}

bool KeyManager::AreKeysLoaded() const {
    return !s128_keys.empty() && !s256_keys.empty();
}

bool KeyManager::BaseDeriveNecessary() const {
    const auto check_key_existence = [this](auto key_type, u64 index1 = 0, u64 index2 = 0) {
        return !HasKey(key_type, index1, index2);
    };

    if (check_key_existence(S256KeyType::Header)) {
        return true;
    }

    for (size_t i = 0; i < CURRENT_CRYPTO_REVISION; ++i) {
        if (check_key_existence(S128KeyType::Master, i)
        || check_key_existence(S128KeyType::KeyArea, i, u64(KeyAreaKeyType::Application))
        || check_key_existence(S128KeyType::KeyArea, i, u64(KeyAreaKeyType::Ocean))
        || check_key_existence(S128KeyType::KeyArea, i, u64(KeyAreaKeyType::System))
        || check_key_existence(S128KeyType::Titlekek, i))
            return true;
    }

    return false;
}

bool KeyManager::HasKey(S128KeyType id, u64 field1, u64 field2) const {
    return s128_keys.find({id, field1, field2}) != s128_keys.end();
}

bool KeyManager::HasKey(S256KeyType id, u64 field1, u64 field2) const {
    return s256_keys.find({id, field1, field2}) != s256_keys.end();
}

Key128 KeyManager::GetKey(S128KeyType id, u64 field1, u64 field2) const {
    if (!HasKey(id, field1, field2)) {
        return {};
    }
    return s128_keys.at({id, field1, field2});
}

Key256 KeyManager::GetKey(S256KeyType id, u64 field1, u64 field2) const {
    if (!HasKey(id, field1, field2)) {
        return {};
    }
    return s256_keys.at({id, field1, field2});
}

Key256 KeyManager::GetBISKey(u8 partition_id) const {
    Key256 out{};

    for (const auto& bis_type : {BISKeyType::Crypto, BISKeyType::Tweak}) {
        if (HasKey(S128KeyType::BIS, partition_id, u64(bis_type))) {
            std::memcpy(
                out.data() + sizeof(Key128) * u64(bis_type),
                s128_keys.at({S128KeyType::BIS, partition_id, u64(bis_type)}).data(),
                sizeof(Key128));
        }
    }

    return out;
}

void KeyManager::SetKey(S128KeyType id, Key128 key, u64 field1, u64 field2) {
    // Held in memory only. suyu never writes key files: keys come from the files the
    // user installs, and anything computed from them (title keys from the user's own
    // tickets, SD keys from the user's sources) lives only for this session.
    if (s128_keys.find({id, field1, field2}) != s128_keys.end() || key == Key128{}) {
        return;
    }
    s128_keys[{id, field1, field2}] = key;
}

void KeyManager::SetKey(S256KeyType id, Key256 key, u64 field1, u64 field2) {
    if (s256_keys.find({id, field1, field2}) != s256_keys.end() || key == Key256{}) {
        return;
    }
    s256_keys[{id, field1, field2}] = key;
}

bool KeyManager::KeyFileExists(bool title) {
    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
    if (title)
        return Common::FS::Exists(keys_dir / "title.keys");
    if (Settings::values.use_dev_keys.GetValue())
        return Common::FS::Exists(keys_dir / "dev.keys");
    return Common::FS::Exists(keys_dir / "prod.keys");
}

void KeyManager::PopulateTickets() {
    if (!ticket_databases_loaded) {
        ticket_databases_loaded = true;

        std::vector<Ticket> tickets;
        const auto system_save_e1_path =
            Common::FS::GetSuyuPath(Common::FS::SuyuPath::NANDDir) / "system/save/80000000000000e1";
        if (Common::FS::Exists(system_save_e1_path)) {
            const Common::FS::IOFile save_e1{system_save_e1_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile};
            const auto blob1 = GetTicketblob(save_e1);
            tickets.insert(tickets.end(), blob1.begin(), blob1.end());
        }

        const auto system_save_e2_path =
            Common::FS::GetSuyuPath(Common::FS::SuyuPath::NANDDir) / "system/save/80000000000000e2";
        if (Common::FS::Exists(system_save_e2_path)) {
            const Common::FS::IOFile save_e2{system_save_e2_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile};
            const auto blob2 = GetTicketblob(save_e2);
            tickets.insert(tickets.end(), blob2.begin(), blob2.end());
        }

        for (const auto& ticket : tickets) {
            AddTicket(ticket);
        }
    }
}

void KeyManager::SynthesizeTickets() {
    for (const auto& key : s128_keys) {
        if (key.first.type == S128KeyType::Titlekey) {
            u128 rights_id{key.first.field1, key.first.field2};
            Key128 rights_id_2;
            std::memcpy(rights_id_2.data(), rights_id.data(), rights_id_2.size());
            const auto ticket = Ticket::SynthesizeCommon(key.second, rights_id_2);
            common_tickets.insert_or_assign(rights_id, ticket);
        }
    }
}

const std::map<u128, Ticket>& KeyManager::GetCommonTickets() const {
    return common_tickets;
}

const std::map<u128, Ticket>& KeyManager::GetPersonalizedTickets() const {
    return personal_tickets;
}

bool KeyManager::AddTicket(const Ticket& ticket) {
    if (!ticket.IsValid()) {
        LOG_WARNING(Crypto, "Attempted to add invalid ticket.");
        return false;
    }

    const auto& rid = ticket.GetData().rights_id;
    u128 rights_id;
    std::memcpy(rights_id.data(), rid.data(), rid.size());
    if (ticket.GetData().type == Core::Crypto::TitleKeyType::Common) {
        common_tickets[rights_id] = ticket;
    } else {
        personal_tickets[rights_id] = ticket;
    }

    if (HasKey(S128KeyType::Titlekey, rights_id[1], rights_id[0])) {
        LOG_DEBUG(Crypto,
            "Skipping parsing title key from ticket for known rights ID {:016X}{:016X}.",
            rights_id[1], rights_id[0]);
        return true;
    }

    const auto key = ParseTicketTitleKey(ticket);
    if (!key) {
        return false;
    }
    SetKey(S128KeyType::Titlekey, key.value(), rights_id[1], rights_id[0]);
    return true;
}

namespace {
// Ticket files are a few hundred bytes (an RSA-4096 ticket is 0x500); anything far larger is
// not one.
constexpr std::size_t kMaxTicketFileSize = 0x10000;

// Ticket::Read copies a whole ticket structure, so short input is zero-padded first.
Ticket ReadTicketBytes(std::span<const u8> bytes) {
    std::array<u8, sizeof(RSA4096Ticket)> padded{};
    std::memcpy(padded.data(), bytes.data(), std::min(bytes.size(), padded.size()));
    return Ticket::Read(std::span<const u8>{padded});
}

std::optional<std::vector<u8>> ReadSmallFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxTicketFileSize) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}
} // Anonymous namespace

std::filesystem::path TicketStoreDir(const std::filesystem::path& nand_dir) {
    // A Switch keeps imported tickets on its SYSTEM partition (the ES saves 80000000000000e1
    // and e2, which PopulateTickets reads), and suyu's NAND folder mirrors that partition as
    // <nand>/system. Plain .tik files beside those saves avoid writing the ES save format,
    // stay out of system/Contents/registered, which firmware installs replace, and sit in a
    // NAND area the package policy keeps out of exports and releases. Only tickets are kept
    // here, as the NSP shipped them: suyu writes no key files and never stores a title key
    // in plain form.
    return nand_dir / "system" / "tickets";
}

bool StoreInstalledTicket(const std::filesystem::path& nand_dir,
                          const FileSys::VirtualFile& ticket_file) {
    if (ticket_file == nullptr || nand_dir.empty()) {
        return false;
    }
    const auto size = ticket_file->GetSize();
    if (size < sizeof(SignatureType) || size > kMaxTicketFileSize) {
        LOG_WARNING(Crypto, "Not keeping ticket {}: unexpected size {}", ticket_file->GetName(),
                    size);
        return false;
    }
    const std::vector<u8> bytes = ticket_file->ReadAllBytes();
    if (bytes.size() != size) {
        return false;
    }
    const auto ticket = ReadTicketBytes(bytes);
    if (!ticket.IsValid() || ticket.GetData().rights_id == Key128{}) {
        LOG_WARNING(Crypto, "Not keeping {}: not a ticket with a rights ID",
                    ticket_file->GetName());
        return false;
    }

    const auto dir = TicketStoreDir(nand_dir);
    const auto path = dir / (Common::HexToString(ticket.GetData().rights_id, false) + ".tik");
    if (const auto existing = ReadSmallFile(path); existing && *existing == bytes) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR(Crypto, "Could not create the ticket store {}: {}",
                  Common::FS::PathToUTF8String(dir), ec.message());
        return false;
    }
    // Written beside the target and renamed over it, so a failed write never leaves a
    // truncated ticket behind.
    auto temp = path;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            LOG_ERROR(Crypto, "Could not write ticket {}", Common::FS::PathToUTF8String(path));
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        LOG_ERROR(Crypto, "Could not store ticket {}", Common::FS::PathToUTF8String(path));
        return false;
    }
    LOG_INFO(Crypto, "Kept the ticket for rights ID {} in {}",
             Common::HexToString(ticket.GetData().rights_id, false),
             Common::FS::PathToUTF8String(dir));
    return true;
}

std::size_t KeyManager::LoadInstalledTickets(const std::filesystem::path& nand_dir,
                                            const std::filesystem::path& package_dir) {
    if (nand_dir.empty()) {
        return 0;
    }
    const auto dir = TicketStoreDir(nand_dir).lexically_normal();
    if (!FileSys::CanReadInstalledTicketPath(dir, package_dir)) {
        LOG_WARNING(Crypto, "Ignoring ticket store inside export package: {}",
                    Common::FS::PathToUTF8String(dir));
        return 0;
    }
    if (std::find(loaded_ticket_stores.begin(), loaded_ticket_stores.end(), dir) !=
        loaded_ticket_stores.end()) {
        return 0;
    }
    loaded_ticket_stores.push_back(dir);

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    std::size_t added = 0;
    std::size_t with_title_key = 0;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& path = it->path();
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) ||
            Common::ToLower(Common::FS::PathToUTF8String(path.extension())) != ".tik") {
            continue;
        }
        if (!FileSys::CanReadInstalledTicketPath(path, package_dir)) {
            LOG_WARNING(Crypto, "Ignoring ticket inside export package: {}",
                        Common::FS::PathToUTF8String(path));
            continue;
        }
        const auto bytes = ReadSmallFile(path);
        const auto ticket = bytes ? ReadTicketBytes(*bytes) : Ticket{std::monostate()};
        if (!ticket.IsValid() || ticket.GetData().rights_id == Key128{}) {
            LOG_WARNING(Crypto, "Skipping {}: not a ticket", Common::FS::PathToUTF8String(path));
            continue;
        }
        ++added;
        if (AddTicket(ticket)) {
            ++with_title_key;
        }
    }
    installed_ticket_count += added;
    installed_title_key_count += with_title_key;
    LOG_INFO(Crypto, "Loaded {} installed tickets ({} with a title key) from {}", added,
             with_title_key, Common::FS::PathToUTF8String(dir));
    return added;
}

std::size_t KeyManager::GetInstalledTicketCount() const {
    return installed_ticket_count;
}

std::size_t KeyManager::GetInstalledTitleKeyCount() const {
    return installed_title_key_count;
}
} // namespace Core::Crypto
