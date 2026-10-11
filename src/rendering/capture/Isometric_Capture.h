//
// Изометрический снимок мира (клавиша F7) — как в Minecraft Indev.
//
// Снимается квадрат чанков вокруг игрока (Config::iso_capture_radius_chunks, по умолчанию
// 14 -> 29 x 29 чанков; 22 даёт 45 x 45, но это ~8+ ГБ памяти) ортографической камерой под углом настоящей изометрии. Работает в
// несколько шагов, по одному на кадр, чтобы окно не зависало:
//   1. Chunk_Manager грузит область, считает свет и строит меши (Loading);
//   2. сцена рисуется в FBO тайлами и склеивается в одну картинку на CPU (Rendering);
//   3. PNG пишется в фоновом потоке (Writing), игра при этом уже продолжается.
// Освещение и цвет неба — те же, что в игре в момент нажатия: день остаётся днём, ночь — ночью.
//
#ifndef OPTICRAFT_ISOMETRIC_CAPTURE_H
#define OPTICRAFT_ISOMETRIC_CAPTURE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <glm/glm.hpp>

class Chunk_Manager;
class Renderer;

class Isometric_Capture {
public:
    enum class Stage { Planning, Generating, Lighting, Meshing, Rendering, Saving, Finished };

    Isometric_Capture(Chunk_Manager& chunks, Renderer& renderer);
    ~Isometric_Capture();

    Isometric_Capture(const Isometric_Capture&) = delete;
    Isometric_Capture& operator=(const Isometric_Capture&) = delete;

    // center_block — позиция игрока (мировые координаты), radius — радиус области в чанках.
    // sky_color и ambient — время суток на момент снимка (фон кадра и яркость мира).
    // Возвращает false, если снимок начать нельзя.
    bool start(const glm::vec3& center_block, int radius_chunks,
               const glm::vec3& sky_color, float ambient);

    // Один шаг; звать каждый кадр, пока is_busy().
    void update();

    // Прервать загрузку/рендер. Если PNG уже пишется — дописывается до конца.
    void cancel();

    // true, пока нужен показ прогресса и мир стоит на паузе (загрузка и рендер).
    bool is_busy() const { return m_state == State::Loading || m_state == State::Rendering; }
    // true, когда всё закончено: файл записан, ошибка или отмена. Объект можно удалять.
    bool is_finished() const;
    bool has_succeeded() const;
    bool was_cancelled() const { return m_state == State::Cancelled; }

    Stage get_stage() const;
    float get_progress() const;
    size_t get_chunks_loaded() const { return m_chunks_loaded; }
    size_t get_chunks_total() const { return m_chunks_total; }
    int get_tiles_done() const { return m_tiles_done; }
    int get_tiles_total() const { return m_tiles_x * m_tiles_y; }
    int get_image_width() const { return m_image_w; }
    int get_image_height() const { return m_image_h; }
    const std::string& get_output_path() const { return m_output_path; }
    const std::string& get_error() const { return m_error; }

private:
    enum class State { Idle, Loading, Rendering, Writing, Done, Failed, Cancelled };

    Chunk_Manager& m_chunks;
    Renderer& m_renderer;

    State m_state = State::Idle;
    glm::vec3 m_center{0.0f};
    int m_radius = 0;
    glm::vec3 m_sky_color{0.0f};
    float m_ambient = 1.0f;
    bool m_region_open = false;

    size_t m_chunks_loaded = 0;
    size_t m_chunks_total = 0;
    float m_loading_progress = 0.0f;
    int m_loading_phase = 0; // Chunk_Manager::Capture_Phase

    // Камера снимка
    glm::mat4 m_view{1.0f};
    float m_min_x = 0.0f, m_max_y = 0.0f;   // верхний левый угол кадра в видовых координатах
    float m_near = 0.1f, m_far = 1000.0f;
    float m_scale = 1.0f;                   // пикселей на блок
    int m_image_w = 0, m_image_h = 0;
    int m_tile_size = 0;
    int m_tiles_x = 0, m_tiles_y = 0;
    int m_tiles_done = 0;
    std::vector<uint8_t> m_pixels;          // RGB, строки сверху вниз

    // FBO одного тайла
    unsigned int m_fbo = 0;
    unsigned int m_color_rb = 0;
    unsigned int m_depth_rb = 0;

    // Запись PNG
    std::string m_output_path;
    std::string m_error;
    std::thread m_writer;
    std::atomic<bool> m_write_finished{false};
    std::atomic<bool> m_write_ok{false};

    bool prepare_render();
    bool create_framebuffer();
    void destroy_framebuffer();
    bool render_tile(int tile_index);
    void finish_render();
    void close_region();
    void fail(const std::string& message);
};

#endif // OPTICRAFT_ISOMETRIC_CAPTURE_H
