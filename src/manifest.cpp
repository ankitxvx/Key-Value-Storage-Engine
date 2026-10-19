#include "detail/manifest.hpp"
#include <algorithm>
#include <cstring>
#include <set>

namespace kv::detail {

bool table_name(std::string_view name) {
    if (name.size() < 11 || name.substr(0, 6) != "table-" || name.substr(name.size() - 4) != ".sst") return false;
    return std::all_of(name.begin() + 6, name.end() - 4, [](char c) { return c >= '0' && c <= '9'; });
}
void write_manifest(const fs::path& directory, const Manifest& manifest, const Fault& fault) {
    std::string body;
    append_u64(body, manifest.checkpoint);
    append_u64(body, manifest.next_id);
    append_u32(body, static_cast<std::uint32_t>(manifest.names.size()));
    for (const auto& name : manifest.names) {
        append_u32(body, static_cast<std::uint32_t>(name.size()));
        body += name;
    }
    if (body.size() > max_record) fail("Manifest too large");
    std::string header("KVMF0001", 8);
    append_u32(header, static_cast<std::uint32_t>(body.size()));
    append_u32(header, crc32(body));
    {
        File file(directory / "MANIFEST.tmp", "wb");
        inject(fault, "manifest.write");
        file.write(header);
        file.write(body);
        inject(fault, "manifest.sync");
        file.sync();
    }
    inject(fault, "manifest.publish");
    publish(directory / "MANIFEST.tmp", directory / "MANIFEST");
}
Manifest read_manifest(const fs::path& directory) {
    File file(directory / "MANIFEST", "rb");
    char header[16];
    if (file.read(header, 16) != 16 || std::memcmp(header, "KVMF0001", 8)) fail("Invalid manifest header");
    const auto length = u32(header + 8);
    if (length < 20 || length > max_record) fail("Invalid manifest size");
    std::string body(length, '\0');
    if (file.read(body.data(), length) != length || crc32(body) != u32(header + 12)) fail("Manifest checksum mismatch");
    char extra;
    if (file.read(&extra, 1)) fail("Trailing manifest bytes");
    Manifest m{u64(body.data()), u64(body.data() + 8), {}};
    if (!m.next_id) fail("Invalid table generation");
    const auto count = u32(body.data() + 16);
    std::size_t pos = 20;
    std::set<std::string> unique;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (body.size() - pos < 4) fail("Truncated manifest");
        const auto n = u32(body.data() + pos);
        pos += 4;
        if (n > body.size() - pos) fail("Invalid manifest name");
        auto name = body.substr(pos, n);
        pos += n;
        if (!table_name(name) || !unique.insert(name).second) fail("Invalid manifest table");
        m.names.push_back(std::move(name));
    }
    if (pos != body.size()) fail("Malformed manifest");
    return m;
}
Manifest describe(const Tables& tables, std::uint64_t checkpoint, std::uint64_t next_id) {
    Manifest m{checkpoint, next_id, {}};
    for (const auto& table : tables) m.names.push_back(table->path.filename().string());
    return m;
}

} // namespace kv::detail
