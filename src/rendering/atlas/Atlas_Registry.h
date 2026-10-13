//
// Atlas_Registry — грузит ВСЕ *.atlas.json манифесты из указанной директории
// (по умолчанию assets/atlases/) в именованные Texture_Atlas. Это и есть
// механизм "сколько угодно атласов без переписывания кода": чтобы завести
// новый атлас (для UI, для 3D-иконок блоков в инвентаре, для чего угодно
// ещё) — просто кладёшь пару <name>.png + <name>.atlas.json в эту папку,
// пересборка не нужна, C++ трогать не надо.
//
// Формат манифеста (<name>.atlas.json):
//   {
//     "name": "blocks_main",              // имя, под которым атлас регистрируется — get_atlas() ищет по нему
//     "texture": "blocks_main.png",        // путь к PNG, относительно самого манифеста
//     "sprites": {
//       "grass_top":  { "x": 0,  "y": 0, "w": 16, "h": 16 },
//       "torch":      { "x": 32, "y": 0, "w": 16, "h": 24 }  // произвольный размер — НЕ обязан быть 16x16
//     }
//   }
//
// "name" в манифесте — то, под чем атлас будет доступен через get_atlas(name);
// если не задано, используется имя файла без расширений (".atlas.json").
//
#ifndef OPTICRAFT_ATLAS_REGISTRY_H
#define OPTICRAFT_ATLAS_REGISTRY_H

#include "rendering/atlas/Texture_Atlas.h"
#include <memory>
#include <string>
#include <unordered_map>

class Atlas_Registry {
private:
    std::unordered_map<std::string, std::unique_ptr<Texture_Atlas>> m_atlases;

    Atlas_Registry() = default;

public:
    static Atlas_Registry& get_instance();

    Atlas_Registry(const Atlas_Registry&) = delete;
    Atlas_Registry& operator=(const Atlas_Registry&) = delete;

    // Сканирует directory на файлы "*.atlas.json" и грузит каждый. Вызывать ОДИН раз при
    // старте (Game::initialize), ДО первого обращения к Block_Registry — та резолвит
    // текстуры блоков через уже загруженные атласы. Повторный вызов дозагружает новые
    // манифесты и перегружает уже известные (полезно для будущего hot-reload).
    // Возвращает число успешно загруженных атласов.
    int load_all(const std::string& directory);

    // nullptr, если атлас с таким именем не зарегистрирован (см. лог на LOG_ERROR).
    const Texture_Atlas* get_atlas(const std::string& name) const;

    // Освобождает GL-текстуры атласов. Звать до glfwTerminate(): иначе деструктор синглтона
    // удаляет текстуры уже после уничтожения GL-контекста (падение при выходе).
    void clear() { m_atlases.clear(); }
};

#endif //OPTICRAFT_ATLAS_REGISTRY_H
