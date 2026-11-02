#pragma once

#include "search/source_adapter.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace media::search {

class SourceAdapterRegistry {
public:
    bool register_adapter(std::unique_ptr<ISourceAdapter> adapter, std::string* error = nullptr);

    ISourceAdapter* adapter_for(const std::filesystem::path& file) const;
    std::vector<ISourceAdapter*> adapters_for(const std::filesystem::path& file) const;
    std::vector<const ISourceAdapter*> adapters() const;

private:
    std::vector<std::unique_ptr<ISourceAdapter>> adapters_;
};

SourceAdapterRegistry& source_adapter_registry();

} // namespace media::search
