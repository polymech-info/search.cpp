#include "search/source_adapter_registry.hpp"
#include "search/host.hpp"

#include <algorithm>
#include <mutex>

namespace media::search {

bool SourceAdapterRegistry::register_adapter(std::unique_ptr<ISourceAdapter> adapter,
                                             std::string* error)
{
    if (!adapter) {
        if (error) *error = "adapter is null";
        return false;
    }
    const std::string id = adapter->id() ? adapter->id() : "";
    if (id.empty()) {
        if (error) *error = "adapter id is empty";
        return false;
    }
    const auto dup = std::find_if(adapters_.begin(), adapters_.end(),
        [&](const auto& existing) {
            return existing && id == existing->id();
        });
    if (dup != adapters_.end()) {
        if (error) *error = "duplicate source adapter id: " + id;
        return false;
    }
    adapters_.push_back(std::move(adapter));
    return true;
}

ISourceAdapter* SourceAdapterRegistry::adapter_for(const std::filesystem::path& file) const
{
    for (const auto& adapter : adapters_) {
        if (adapter && adapter->can_extract(file))
            return adapter.get();
    }
    return nullptr;
}

std::vector<ISourceAdapter*> SourceAdapterRegistry::adapters_for(const std::filesystem::path& file) const
{
    std::vector<ISourceAdapter*> out;
    for (const auto& adapter : adapters_) {
        if (adapter && adapter->can_extract(file))
            out.push_back(adapter.get());
    }
    return out;
}

std::vector<const ISourceAdapter*> SourceAdapterRegistry::adapters() const
{
    std::vector<const ISourceAdapter*> out;
    out.reserve(adapters_.size());
    for (const auto& adapter : adapters_) {
        if (adapter)
            out.push_back(adapter.get());
    }
    return out;
}

SourceAdapterRegistry& source_adapter_registry()
{
    static SourceAdapterRegistry registry;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string error;
        (void)registry.register_adapter(std::make_unique<CodeSourceAdapter>(), &error);
        (void)registry.register_adapter(std::make_unique<PlainTextSourceAdapter>(), &error);
        host::register_adapters(registry);
    });
    return registry;
}

} // namespace media::search
