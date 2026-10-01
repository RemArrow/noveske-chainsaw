// pack - encrypts the Noveske's runtime assets into the payload that is built into
// NoveskeChainsaw.dll (format: src/payload.h).
//
//   pack <Assets dir> <payload.bin> <payload_key.h>
//
// Packs every *.obj and *.png in the Assets folder, named by file stem ("noveske", "noveske_bolt",
// "Body_Diffuse", ...). Each asset is LZMS-compressed when that saves space, then AES-256-GCM
// encrypted with a key that is random for every build. The key goes to payload_key.h, split into
// two XOR halves, for the plugin to compile in.
//
// Before exiting it proves the result: every asset decrypts back byte-identical, a payload with one
// flipped byte is refused, and the payload contains no PNG signature or OBJ text.
#include "../../src/payload.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool Random(void* buf, ULONG n) {
    return BCryptGenRandom(nullptr, (PUCHAR)buf, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static std::vector<uint8_t> Lzms(const std::vector<uint8_t>& in) {
    COMPRESSOR_HANDLE c = nullptr;
    std::vector<uint8_t> out;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c)) return out;
    SIZE_T need = 0;
    Compress(c, in.data(), in.size(), nullptr, 0, &need);
    out.resize(need);
    if (!Compress(c, in.data(), in.size(), out.data(), out.size(), &need)) out.clear();
    else out.resize(need);
    CloseCompressor(c);
    return out;
}

static bool Contains(const std::vector<uint8_t>& hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("usage: pack <Assets dir> <payload.bin> <payload_key.h>\n");
        return 1;
    }
    struct Item { std::string name; std::vector<uint8_t> data; };
    std::vector<Item> items;
    for (auto& de : fs::directory_iterator(argv[1])) {
        auto ext = de.path().extension().string();
        if (ext == ".obj" || ext == ".png")
            items.push_back({de.path().stem().string(), ReadAll(de.path())});
    }
    if (items.empty()) { printf("pack: no assets in %s\n", argv[1]); return 1; }

    uint8_t key[32];
    if (!Random(key, sizeof key)) { printf("pack: no random key\n"); return 1; }

    std::vector<payload::Entry> entries(items.size());
    std::vector<uint8_t> data;
    const size_t dataStart = sizeof(payload::Header) + entries.size() * sizeof(payload::Entry);
    size_t raw = 0;
    for (size_t i = 0; i < items.size(); i++) {
        payload::Entry& e = entries[i];
        e.nameHash = payload::Hash(items[i].name);
        e.rawSize = (uint32_t)items[i].data.size();
        std::vector<uint8_t> stored = Lzms(items[i].data);
        if (!stored.empty() && stored.size() < items[i].data.size() * 95 / 100) e.flags = payload::kCompressed;
        else stored = items[i].data;
        e.storedSize = (uint32_t)stored.size();
        e.offset = (uint32_t)(dataStart + data.size());
        if (!Random(e.nonce, sizeof e.nonce)) return 1;
        size_t at = data.size();
        data.resize(at + stored.size());
        if (!payload::Gcm(true, key, e.nonce, e.tag, e.nameHash, stored.data(), data.data() + at, e.storedSize)) {
            printf("pack: encryption failed for %s\n", items[i].name.c_str());
            return 1;
        }
        raw += items[i].data.size();
        printf("  %-18s %9u -> %9u bytes%s\n", items[i].name.c_str(), e.rawSize, e.storedSize,
               e.flags & payload::kCompressed ? " (LZMS)" : "");
    }
    payload::Header h;
    memcpy(h.magic, payload::kMagic, 4);
    h.count = (uint32_t)entries.size();
    std::vector<uint8_t> blob((uint8_t*)&h, (uint8_t*)&h + sizeof h);
    blob.insert(blob.end(), (uint8_t*)entries.data(), (uint8_t*)(entries.data() + entries.size()));
    blob.insert(blob.end(), data.begin(), data.end());

    // Prove it before writing anything.
    for (auto& it : items) {
        std::vector<uint8_t> back;
        if (!payload::Extract(blob.data(), blob.size(), key, it.name, back) || back != it.data) {
            printf("pack: %s does not decrypt back to the original\n", it.name.c_str());
            return 1;
        }
    }
    std::vector<uint8_t> tampered = blob;
    tampered[entries[0].offset + entries[0].storedSize / 2] ^= 1;
    std::vector<uint8_t> junk;
    if (payload::Extract(tampered.data(), tampered.size(), key, items[0].name, junk)) {
        printf("pack: a modified payload was accepted\n");
        return 1;
    }
    for (std::string_view sig : {std::string_view("\x89PNG", 4), std::string_view("IHDR"), std::string_view("usemtl"),
                                 std::string_view("\nvt "), std::string_view("MI_Body")}) {
        if (Contains(blob, sig)) { printf("pack: plain asset bytes found in the payload\n"); return 1; }
    }

    fs::create_directories(fs::path(argv[2]).parent_path());
    std::ofstream(argv[2], std::ios::binary).write((const char*)blob.data(), blob.size());
    uint8_t mask[32];
    if (!Random(mask, sizeof mask)) return 1;
    std::ofstream k(argv[3]);
    k << "// Generated by tools/pack for this build only - never commit. key = A ^ B\n";
    for (int half = 0; half < 2; half++) {
        k << "static const volatile unsigned char kPayloadKey" << (half ? 'B' : 'A') << "[32] = {";
        for (int i = 0; i < 32; i++) k << (i ? "," : "") << (int)(half ? (key[i] ^ mask[i]) : mask[i]);
        k << "};\n";
    }
    SecureZeroMemory(key, sizeof key);
    printf("pack: %zu assets, %.1f MB -> %.1f MB payload, encrypted and verified\n", items.size(), raw / 1048576.0,
           blob.size() / 1048576.0);
    return 0;
}
