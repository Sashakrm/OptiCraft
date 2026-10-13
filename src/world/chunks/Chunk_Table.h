//
// Chunk_Table.h — компактные open-addressing контейнеры для координат чанков.
//
// std::unordered_map/unordered_set — это chaining hash map: каждая запись живёт в
// отдельно аллоцированном в куче узле, и даже успешный lookup — это несколько обращений
// к памяти в СЛУЧАЙНЫХ местах кучи (массив бакетов -> узел -> следующий узел цепочки ->
// ...). При ~2000+ одновременно загруженных чанках (обычный load_radius, см. Config) и
// том, что get_chunk() вызывается на каждое чтение блока/света, на каждый видимый чанк
// каждый кадр при рендере (get_renderable_chunks), при каждой загрузке/выгрузке чанка —
// это основная "невидимая" стоимость Chunk_Manager, которую не видно в самих алгоритмах,
// но видно в cache-misses под профайлером.
//
// Ниже — open addressing (Robin Hood hashing, с backward-shift delete) в одном сплошном
// std::vector<Slot>: и успешный, и безуспешный поиск гарантированно завершается за
// ограниченное число шагов ВПЕРЁД по одному непрерывному куску памяти (несколько соседних
// кэш-линий, а не блуждание по куче), без единой лишней аллокации на чтение/запись.
// Robin Hood (а не обычный линейный пробинг) выбран из-за высокой текучести: чанки грузятся
// и выгружаются десятками в секунду при простой ходьбе игрока — обычный линейный пробинг
// с tombstone-удалением деградирует под такой нагрузкой (нужен периодический rehash только
// чтобы вычистить хвосты), а backward-shift delete держит таблицу "чистой" всё время.
//
#ifndef OPTICRAFT_CHUNK_TABLE_H
#define OPTICRAFT_CHUNK_TABLE_H

#include "world/chunks/Chunk.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <utility>
#include <glm/glm.hpp>

namespace Chunk_Coord_Pack {
    // 27 бит на X и на Z (мир по горизонтали ничем не ограничен — игрок теоретически может
    // уйти на тысячи чанков, — но ±2^26 чанков это больше двух миллиардов блоков в любую
    // сторону, недостижимо ни в одной реальной игровой сессии), 10 бит на Y (реальный
    // диапазон — Config::world_min_chunk_y..world_max_chunk_y, при текущих значениях всего
    // -8..32, так что ±512 — запас на два порядка больше нужного, даже если границы мира по
    // Y когда-нибудь расширят). 27+27+10 = 64 — используем ключ целиком; "пустой слот"
    // помечается ОТДЕЛЬНЫМ флагом (Slot::occupied), а не зарезервированным значением ключа,
    // так что даже крайний случай (все три поля на максимуме) остаётся обычным ключом без
    // коллизии со sentinel-значением.
    constexpr int k_xz_bits = 27;
    constexpr int k_y_bits = 10;
    constexpr uint64_t k_xz_mask = (uint64_t{1} << k_xz_bits) - 1;
    constexpr uint64_t k_y_mask = (uint64_t{1} << k_y_bits) - 1;
    constexpr int64_t k_xz_bias = int64_t{1} << (k_xz_bits - 1);
    constexpr int64_t k_y_bias = int64_t{1} << (k_y_bits - 1);

    inline uint64_t pack(int cx, int cy, int cz) {
        const uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(cx) + k_xz_bias) & k_xz_mask;
        const uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(cz) + k_xz_bias) & k_xz_mask;
        const uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(cy) + k_y_bias) & k_y_mask;
        return (ux << (k_y_bits + k_xz_bits)) | (uz << k_y_bits) | uy;
    }

    inline glm::ivec3 unpack(uint64_t key) {
        const int cy = static_cast<int>(key & k_y_mask) - static_cast<int>(k_y_bias);
        const int cz = static_cast<int>((key >> k_y_bits) & k_xz_mask) - static_cast<int>(k_xz_bias);
        const int cx = static_cast<int>((key >> (k_y_bits + k_xz_bits)) & k_xz_mask) - static_cast<int>(k_xz_bias);
        return {cx, cy, cz};
    }

    // Финализатор splitmix64: несколько xor-shift-умножений дают хорошее лавинное
    // перемешивание (каждый бит выхода зависит примерно от половины битов входа). Важно
    // именно для соседних по миру чанков — они получают близкие packed-ключи (координаты
    // лежат в соседних битовых полях), и БЕЗ полноценного финализатора соседи оседали бы в
    // соседних/пересекающихся слотах — ровно тот паттерн доступа, что чаще всего и
    // встречается (BFS освещения, обход по радиусу при рендере/стриминге чанков).
    inline uint64_t hash(uint64_t key) {
        uint64_t h = key;
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        return h;
    }
}

// Open-addressing множество координат чанков — замена
// unordered_set<glm::ivec3, Chunk_Key_Hash> для m_requested_chunks/m_pending_mesh_rebuilds/
// m_mesh_rebuild_requeue. Та же схема Robin Hood, что и в Chunk_Table ниже, но без нагрузки
// в виде значения — только ключ и флаг "занято".
class Chunk_Coord_Set {
public:
    Chunk_Coord_Set() { m_slots.resize(k_initial_capacity); m_mask = k_initial_capacity - 1; }

    bool contains(int cx, int cy, int cz) const {
        return find_slot(Chunk_Coord_Pack::pack(cx, cy, cz)) != k_npos;
    }

    // true, если позиции раньше не было (вставили новую запись); false, если уже была.
    bool insert(int cx, int cy, int cz) {
        maybe_grow();
        return insert_core(Chunk_Coord_Pack::pack(cx, cy, cz));
    }

    // true, если позиция была и её удалили.
    bool erase(int cx, int cy, int cz) { return erase_core(Chunk_Coord_Pack::pack(cx, cy, cz)); }

    void clear() {
        for (auto& slot : m_slots) slot.occupied = false;
        m_count = 0;
    }

    size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }

private:
    struct Slot {
        uint64_t key = 0;
        bool occupied = false;
    };
    // Степень двойки. Эти множества обычно на порядок меньше основной Chunk_Table (это
    // "в полёте"/"недавно тронутые" позиции, а не все загруженные чанки), так что стартуют
    // меньше и растут отдельно от неё.
    static constexpr size_t k_initial_capacity = 1024;
    static constexpr size_t k_npos = static_cast<size_t>(-1);

    std::vector<Slot> m_slots;
    size_t m_count = 0;
    size_t m_mask = 0;

    size_t home_index(uint64_t key) const { return Chunk_Coord_Pack::hash(key) & m_mask; }

    uint32_t probe_distance(size_t slot_index, uint64_t slot_key) const {
        return static_cast<uint32_t>((slot_index - home_index(slot_key)) & m_mask);
    }

    size_t find_slot(uint64_t key) const {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        while (true) {
            const Slot& slot = m_slots[pos];
            if (!slot.occupied) return k_npos;
            if (slot.key == key) return pos;
            // Инвариант Robin Hood: слоты вдоль пути пробинга идут с НЕУБЫВАЮЩИМ probe
            // distance для "правильно вставленной" последовательности. Если у текущего
            // слота дистанция меньше нашей — искомого ключа тут нет и дальше не будет
            // (он был бы вытеснен раньше), можно останавливаться, не доходя до пустого слота.
            if (probe_distance(pos, slot.key) < dist) return k_npos;
            pos = (pos + 1) & m_mask;
            ++dist;
        }
    }

    void maybe_grow() {
        if ((m_count + 1) * 8 > m_slots.size() * 7) { // load factor > 0.875
            rehash(m_slots.size() * 2);
        }
    }

    void rehash(size_t new_capacity) {
        std::vector<Slot> old_slots = std::move(m_slots);
        m_slots.assign(new_capacity, Slot{});
        m_mask = new_capacity - 1;
        m_count = 0;
        for (const auto& slot : old_slots) {
            if (slot.occupied) insert_core(slot.key);
        }
    }

    // Сам алгоритм вставки, без проверки роста (её делает insert(); rehash() тоже вызывает
    // это напрямую — ёмкость на момент rehash уже финальная, повторный рост не нужен).
    //
    // Почему "дошли до пустого слота" ВСЕГДА означает "key был новым", сколько бы свопов
    // ни случилось по пути: если бы key уже существовал в таблице на слоте S, то по
    // инварианту Robin Hood ВДОЛЬ ВСЕГО пути пробинга от home(key) до S у каждого
    // промежуточного слота probe_distance >= смещению текущего шага (иначе тот, первый
    // key, сам обязан был бы вытеснить его при своей исходной вставке и осесть раньше) —
    // значит наш проход НИКОГДА не мог бы свопнуть раньше, чем дойти до S и увидеть
    // slot.key == cur_key. То есть ветка "уже существовало" ниже всегда успевает сработать
    // первой, если key правда уже в таблице — а до пустого слота можно дойти только когда
    // key был новым, независимо от того, сколько раз мы по пути "вытесняли" чужие записи.
    bool insert_core(uint64_t key) {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        uint64_t cur_key = key;
        while (true) {
            Slot& slot = m_slots[pos];
            if (!slot.occupied) {
                slot.occupied = true;
                slot.key = cur_key;
                ++m_count;
                return true; // см. доказательство выше
            }
            if (slot.key == cur_key) {
                return false; // key уже был в таблице
            }
            const uint32_t existing_dist = probe_distance(pos, slot.key);
            if (existing_dist < dist) {
                std::swap(cur_key, slot.key);
                dist = existing_dist;
            }
            pos = (pos + 1) & m_mask;
            ++dist;
        }
    }

    bool erase_core(uint64_t key) {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        while (true) {
            if (!m_slots[pos].occupied) return false;
            if (m_slots[pos].key == key) break;
            if (probe_distance(pos, m_slots[pos].key) < dist) return false;
            pos = (pos + 1) & m_mask;
            ++dist;
        }

        // Backward-shift delete: подтягиваем каждый следующий слот на освободившееся место,
        // пока не упрёмся в пустой слот или слот, который и так уже "дома" (probe_distance
        // 0 — двигать его назад было бы неправильно, там ему самое место). Так таблица
        // остаётся без "дыр"/tombstone-ов всё время, без отдельного периодического rehash.
        size_t cur = pos;
        while (true) {
            const size_t next = (cur + 1) & m_mask;
            if (!m_slots[next].occupied || probe_distance(next, m_slots[next].key) == 0) {
                m_slots[cur].occupied = false;
                break;
            }
            m_slots[cur].key = m_slots[next].key;
            cur = next;
        }
        --m_count;
        return true;
    }
};

// Open-addressing отображение координат чанка -> владеющий указатель на Chunk. Замена
// unordered_map<glm::ivec3, unique_ptr<Chunk>, Chunk_Key_Hash> — см. общее описание вверху
// файла. Сам Chunk по-прежнему живёт на отдельной куче-аллокации через unique_ptr: переносим
// СЛОТ (ключ+владение), а не сам объект Chunk, поэтому любой ранее полученный `Chunk*`
// (например, закэшированный в Voxel_Cursor на время одного BFS) остаётся действительным даже
// если внутри Chunk_Table происходит rehash/сдвиг слотов — двигаются только сами unique_ptr,
// адрес объекта Chunk, на который они указывают, не меняется.
class Chunk_Table {
public:
    Chunk_Table() { m_slots.resize(k_initial_capacity); m_mask = k_initial_capacity - 1; }

    Chunk* find(int cx, int cy, int cz) {
        return const_cast<Chunk*>(static_cast<const Chunk_Table*>(this)->find(cx, cy, cz));
    }
    const Chunk* find(int cx, int cy, int cz) const {
        const size_t pos = find_slot(Chunk_Coord_Pack::pack(cx, cy, cz));
        return pos == k_npos ? nullptr : m_slots[pos].chunk.get();
    }

    bool contains(int cx, int cy, int cz) const { return find(cx, cy, cz) != nullptr; }

    // Вставляет новый чанк на эту позицию или заменяет существующий (как раньше
    // m_chunks[key] = ...). Возвращает сырой указатель на сохранённый чанк.
    Chunk* insert_or_assign(int cx, int cy, int cz, std::unique_ptr<Chunk> chunk) {
        maybe_grow();
        return insert_core(Chunk_Coord_Pack::pack(cx, cy, cz), std::move(chunk));
    }

    // true, если чанк был и его удалили (unique_ptr освобождает Chunk как обычно).
    bool erase(int cx, int cy, int cz) { return erase_core(Chunk_Coord_Pack::pack(cx, cy, cz)); }

    void clear() {
        for (auto& slot : m_slots) {
            slot.occupied = false;
            slot.chunk.reset();
        }
        m_count = 0;
    }

    size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }

    // Посещает все живые записи; fn(glm::ivec3, unique_ptr<Chunk>&) -> bool, false = остановиться
    // раньше (нужно rebuild_dirty_chunks для ограничения работы за кадр).
    template <typename F>
    void for_each_until(F&& fn) {
        for (auto& slot : m_slots) {
            if (!slot.occupied) continue;
            if (!fn(Chunk_Coord_Pack::unpack(slot.key), slot.chunk)) return;
        }
    }
    template <typename F>
    void for_each_until(F&& fn) const {
        for (const auto& slot : m_slots) {
            if (!slot.occupied) continue;
            if (!fn(Chunk_Coord_Pack::unpack(slot.key), slot.chunk)) return;
        }
    }
    // Удобный вариант без раннего выхода — fn(glm::ivec3, unique_ptr<Chunk>&), ничего не
    // возвращает.
    template <typename F>
    void for_each(F&& fn) {
        for_each_until([&](const glm::ivec3& pos, std::unique_ptr<Chunk>& chunk) {
            fn(pos, chunk);
            return true;
        });
    }
    template <typename F>
    void for_each(F&& fn) const {
        for_each_until([&](const glm::ivec3& pos, const std::unique_ptr<Chunk>& chunk) {
            fn(pos, chunk);
            return true;
        });
    }

private:
    struct Slot {
        uint64_t key = 0;
        bool occupied = false;
        std::unique_ptr<Chunk> chunk;
    };
    static constexpr size_t k_initial_capacity = 4096; // с запасом под load_radius по умолчанию
    // (см. Config::chunk_load_radius/vertical_load_radius_chunks — обычно грузится порядка
    // 2000 чанков) без единого rehash при старте; при большей дальности прорисовки таблица
    // просто удвоится ещё пару раз, это разовая стоимость на смену настройки, не на кадр.
    static constexpr size_t k_npos = static_cast<size_t>(-1);

    std::vector<Slot> m_slots;
    size_t m_count = 0;
    size_t m_mask = 0;

    size_t home_index(uint64_t key) const { return Chunk_Coord_Pack::hash(key) & m_mask; }

    uint32_t probe_distance(size_t slot_index, uint64_t slot_key) const {
        return static_cast<uint32_t>((slot_index - home_index(slot_key)) & m_mask);
    }

    size_t find_slot(uint64_t key) const {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        while (true) {
            const Slot& slot = m_slots[pos];
            if (!slot.occupied) return k_npos;
            if (slot.key == key) return pos;
            if (probe_distance(pos, slot.key) < dist) return k_npos;
            pos = (pos + 1) & m_mask;
            ++dist;
        }
    }

    void maybe_grow() {
        if ((m_count + 1) * 8 > m_slots.size() * 7) { // load factor > 0.875
            rehash(m_slots.size() * 2);
        }
    }

    void rehash(size_t new_capacity) {
        std::vector<Slot> old_slots = std::move(m_slots);
        m_slots.resize(new_capacity); // все Slot{} по умолчанию: occupied=false, chunk=nullptr
        m_mask = new_capacity - 1;
        m_count = 0;
        for (auto& slot : old_slots) {
            if (slot.occupied) insert_core(slot.key, std::move(slot.chunk));
        }
    }

    // См. Chunk_Coord_Set::insert_core — та же логика Robin Hood, здесь дополнительно
    // переносим (move) unique_ptr<Chunk> вместе с ключом при каждом вытеснении.
    Chunk* insert_core(uint64_t key, std::unique_ptr<Chunk> value) {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        uint64_t cur_key = key;
        std::unique_ptr<Chunk> cur_value = std::move(value);
        Chunk* result = nullptr;

        while (true) {
            Slot& slot = m_slots[pos];
            if (!slot.occupied) {
                slot.occupied = true;
                slot.key = cur_key;
                slot.chunk = std::move(cur_value);
                ++m_count;
                if (!result) result = slot.chunk.get();
                return result;
            }
            if (slot.key == cur_key) {
                // insert_or_assign: ключ уже существовал — подменяем значение. Возможно
                // только пока cur_key всё ещё оригинальный key (см. Chunk_Coord_Set).
                slot.chunk = std::move(cur_value);
                if (!result) result = slot.chunk.get();
                return result;
            }
            const uint32_t existing_dist = probe_distance(pos, slot.key);
            if (existing_dist < dist) {
                std::swap(cur_key, slot.key);
                std::swap(cur_value, slot.chunk);
                if (!result) result = slot.chunk.get(); // наш исходный ключ только что осел здесь
                dist = existing_dist;
            }
            pos = (pos + 1) & m_mask;
            ++dist;
        }
    }

    bool erase_core(uint64_t key) {
        size_t pos = home_index(key);
        uint32_t dist = 0;
        while (true) {
            if (!m_slots[pos].occupied) return false;
            if (m_slots[pos].key == key) break;
            if (probe_distance(pos, m_slots[pos].key) < dist) return false;
            pos = (pos + 1) & m_mask;
            ++dist;
        }

        size_t cur = pos;
        while (true) {
            const size_t next = (cur + 1) & m_mask;
            if (!m_slots[next].occupied || probe_distance(next, m_slots[next].key) == 0) {
                m_slots[cur].occupied = false;
                m_slots[cur].chunk.reset();
                break;
            }
            m_slots[cur].key = m_slots[next].key;
            m_slots[cur].chunk = std::move(m_slots[next].chunk);
            cur = next;
        }
        --m_count;
        return true;
    }
};

#endif //OPTICRAFT_CHUNK_TABLE_H
