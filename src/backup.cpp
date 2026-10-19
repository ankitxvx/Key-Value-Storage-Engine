#include "detail/engine.hpp"

namespace kv {

void Store::backup(const fs::path& destination) {
    // Capture a complete checkpoint while holding the mutex only for metadata.
    Tables captured;
    Manifest manifest;
    impl_->flush();
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->healthy();
        captured = impl_->tables;
        manifest = describe(captured, impl_->checkpoint, impl_->next_id);
    }
    const auto target = fs::absolute(destination);
    if (fs::exists(target)) throw std::invalid_argument("Backup destination already exists");
    // The staging directory is exclusively created and published only when complete.
    const fs::path staging = target.string() + ".backup-tmp";
    if (!fs::create_directory(staging)) throw std::invalid_argument("Backup staging directory already exists");
    for (const auto& table : captured) {
        inject(impl_->options.fault_injector, "backup.copy");
        File input(table->path, "rb");
        File output(staging / table->path.filename(), "wb");
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            const auto n = input.read(buffer.data(), buffer.size());
            if (!n) break;
            output.write(std::string_view(buffer.data(), n));
        }
        output.sync();
    }
    { File empty(staging / "WAL", "wb"); empty.sync(); }
    write_manifest(staging, manifest);
    sync_directory(staging);
    // Do not replace an existing directory, even if another caller created it.
    if (fs::exists(target)) throw std::invalid_argument("Backup destination appeared during copy");
    publish_directory(staging, target);
}
void Store::restore(const fs::path& backup, const fs::path& destination) {
    if (!fs::is_directory(backup) || !fs::is_regular_file(backup / "MANIFEST"))
        throw std::invalid_argument("Restore source is not a completed backup");
    Options options;
    options.compaction_table_trigger = 0;
    Store source(backup, options);
    source.backup(destination);
}

} // namespace kv
