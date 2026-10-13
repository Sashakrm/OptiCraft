#include "rendering/atlas/Atlas_Registry.h"
#include "utils/logging/Logger.h"
#include <filesystem>

Atlas_Registry& Atlas_Registry::get_instance() {
    static Atlas_Registry instance;
    return instance;
}

int Atlas_Registry::load_all(const std::string& directory) {
    namespace fs = std::filesystem;

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        LOG_ERROR("Atlas_Registry: directory not found: " + directory);
        return 0;
    }

    int loaded_count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;

        const std::string filename = entry.path().filename().string();
        // Специально ищем суффикс ".atlas.json", а не просто ".json" — так в той же
        // папке спокойно может лежать что-то ещё (например README), не будучи спутанным
        // с манифестом атласа.
        const std::string suffix = ".atlas.json";
        if (filename.size() <= suffix.size() ||
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }

        auto atlas = std::make_unique<Texture_Atlas>();
        const std::string manifest_path = entry.path().string();
        if (!atlas->load_from_manifest(manifest_path)) {
            LOG_ERROR("Atlas_Registry: failed to load " + manifest_path + ", skipped");
            continue;
        }

        std::string name = atlas->get_name();
        if (name.empty()) {
            // Фолбэк: имя файла без ".atlas.json" — например "blocks_main.atlas.json" → "blocks_main".
            name = filename.substr(0, filename.size() - suffix.size());
        }

        if (m_atlases.count(name)) {
            LOG_WARN("Atlas_Registry: atlas \"" + name + "\" reloaded (was already registered)");
        }
        m_atlases[name] = std::move(atlas);
        ++loaded_count;
    }

    LOG_INFO("Atlas_Registry: loaded " + std::to_string(loaded_count) + " atlas(es) from " + directory);
    return loaded_count;
}

const Texture_Atlas* Atlas_Registry::get_atlas(const std::string& name) const {
    const auto it = m_atlases.find(name);
    if (it == m_atlases.end()) {
        LOG_ERROR("Atlas_Registry: no atlas registered as \"" + name + "\"");
        return nullptr;
    }
    return it->second.get();
}
