#include "detail/codec.hpp"

namespace kv::detail {

void append_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
void append_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
std::uint32_t u32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
std::uint64_t u64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc = ~std::uint32_t{0};
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

std::string encode(const Record& r) {
    std::string body;
    append_u64(body, r.sequence);
    body.push_back(r.tombstone ? 1 : 0);
    append_u32(body, static_cast<std::uint32_t>(r.key.size()));
    append_u32(body, static_cast<std::uint32_t>(r.value.size()));
    body += r.key;
    body += r.value;
    std::string frame;
    append_u32(frame, static_cast<std::uint32_t>(body.size()));
    append_u32(frame, crc32(body));
    frame += body;
    return frame;
}

// Only incomplete trailing frames are recoverable. Complete corrupt frames fail
// closed, so corruption cannot silently discard acknowledged writes.
Record decode_record(std::string_view body) {
    if (body.size() < 17) fail("Short record");
    const auto key_size = u32(body.data() + 9);
    const auto value_size = u32(body.data() + 13);
    if (std::uint64_t{17} + key_size + value_size != body.size() ||
        (body[8] != 0 && body[8] != 1) || u64(body.data()) == 0 ||
        (body[8] == 1 && value_size != 0)) fail("Malformed record");
    return {u64(body.data()), body[8] == 1,
            std::string(body.substr(17, key_size)), std::string(body.substr(17 + key_size, value_size))};
}

std::string encode_batch(const std::vector<Record>& records) {
    if (records.size() == 1) return encode(records.front()); // v1 WAL compatibility
    std::string body;
    append_u64(body, records.back().sequence);
    body.push_back(2);
    append_u32(body, static_cast<std::uint32_t>(records.size()));
    append_u32(body, 0);
    for (const auto& r : records) body += encode(r);
    if (body.size() > max_record) throw std::length_error("Batch exceeds 64 MiB");
    std::string frame;
    append_u32(frame, static_cast<std::uint32_t>(body.size()));
    append_u32(frame, crc32(body));
    return frame + body;
}

bool read_record(File& file, Record& r, std::uint64_t& offset, bool wal,
                 std::vector<Record>* batch) {
    char header[8];
    const auto n = file.read(header, sizeof(header));
    if (n == 0) return false;
    if (n != sizeof(header)) {
        if (wal) return false;
        fail("Truncated SSTable record header");
    }
    const auto size = u32(header);
    if (size < 17 || size > max_record) fail("Invalid record length");
    std::string body(size, '\0');
    if (file.read(body.data(), size) != size) {
        if (wal) return false;
        fail("Truncated SSTable record");
    }
    if (crc32(body) != u32(header + 4)) fail("Record checksum mismatch");
    if (batch) batch->clear();
    if (wal && body[8] == 2 && batch) {
        const auto count = u32(body.data() + 9);
        if (!count || u32(body.data() + 13) != 0) fail("Invalid WAL batch");
        std::size_t pos = 17;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (body.size() - pos < 8) fail("Short batch frame");
            const auto length = u32(body.data() + pos);
            const auto checksum = u32(body.data() + pos + 4);
            pos += 8;
            if (length > body.size() - pos) fail("Invalid batch frame length");
            const auto inner = std::string_view(body).substr(pos, length);
            if (crc32(inner) != checksum) fail("Batch checksum mismatch");
            auto item = decode_record(inner);
            if (!batch->empty() && item.sequence != batch->back().sequence + 1)
                fail("Noncontiguous batch");
            batch->push_back(std::move(item));
            pos += length;
        }
        if (pos != body.size() || batch->back().sequence != u64(body.data())) fail("Malformed batch");
        r = batch->back();
    } else {
        r = decode_record(body);
        if (batch) batch->push_back(r);
    }
    offset += 8 + size;
    return true;
}

} // namespace kv::detail
