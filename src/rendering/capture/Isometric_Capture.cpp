//
// Изометрический снимок мира — см. Isometric_Capture.h.
//
#include "rendering/capture/Isometric_Capture.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <new>

#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "rendering/Frustum.h"
#include "rendering/Renderer.h"
#include "utils/Config.h"
#include "utils/logging/Logger.h"
#include "world/chunks/Chunk_Manager.h"

namespace {
    constexpr float k_pi = 3.14159265358979323846f;
    // Камера стоит далеко вдоль луча взгляда: тогда сортировка прозрачных чанков «по расстоянию
    // до камеры» внутри Renderer::render_chunks совпадает с сортировкой по глубине вдоль взгляда.
    constexpr float k_camera_distance = 3000.0f;
    constexpr float k_max_pixels_per_block = 16.0f;
    // Доли шкалы прогресса.
    constexpr float k_loading_share = 0.85f;

    std::string make_output_path() {
        const std::time_t t = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char stamp[40];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_buf);
        return std::string("screenshots/iso_") + stamp + ".png";
    }
}

Isometric_Capture::Isometric_Capture(Chunk_Manager& chunks, Renderer& renderer)
    : m_chunks(chunks), m_renderer(renderer) {}

Isometric_Capture::~Isometric_Capture() {
    if (m_writer.joinable()) m_writer.join();
    destroy_framebuffer();
    close_region();
}

bool Isometric_Capture::start(const glm::vec3& center_block, int radius_chunks,
                              const glm::vec3& sky_color, float ambient) {
    if (m_state != State::Idle) return false;

    m_center = center_block;
    m_radius = std::max(1, radius_chunks);
    m_sky_color = sky_color;
    m_ambient = ambient;
    m_output_path = make_output_path();

    const int center_cx = static_cast<int>(std::floor(center_block.x / static_cast<float>(Config::chunk_size)));
    const int center_cz = static_cast<int>(std::floor(center_block.z / static_cast<float>(Config::chunk_size)));

    m_chunks.begin_capture_region(center_cx, center_cz, m_radius,
                                  Config::iso_capture_extra_depth_layers,
                                  Config::iso_capture_top_margin_blocks);
    if (m_chunks.get_capture_status().phase == Chunk_Manager::Capture_Phase::Inactive) {
        fail("cannot open capture region");
        return false;
    }
    m_region_open = true;
    m_state = State::Loading;
    LOG_INFO("Isometric capture started: radius " + std::to_string(m_radius) + " chunks");
    return true;
}

void Isometric_Capture::update() {
    switch (m_state) {
        case State::Loading: {
            m_chunks.pump_capture_region();
            const Chunk_Manager::Capture_Status status = m_chunks.get_capture_status();
            m_loading_progress = status.progress;
            m_loading_phase = static_cast<int>(status.phase);
            m_chunks_loaded = status.chunks_loaded;
            m_chunks_total = status.chunks_total;
            if (status.phase == Chunk_Manager::Capture_Phase::Ready) {
                if (prepare_render()) m_state = State::Rendering;
            }
            break;
        }
        case State::Rendering: {
            if (!render_tile(m_tiles_done)) {
                fail("rendering failed");
                break;
            }
            ++m_tiles_done;
            if (m_tiles_done >= m_tiles_x * m_tiles_y) finish_render();
            break;
        }
        default:
            break;
    }
}

void Isometric_Capture::cancel() {
    if (!is_busy()) return;
    destroy_framebuffer();
    close_region();
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_state = State::Cancelled;
    LOG_INFO("Isometric capture cancelled");
}

bool Isometric_Capture::is_finished() const {
    switch (m_state) {
        case State::Done:
        case State::Failed:
        case State::Cancelled:
            return true;
        case State::Writing:
            return m_write_finished.load();
        default:
            return false;
    }
}

bool Isometric_Capture::has_succeeded() const {
    if (m_state == State::Done) return true;
    return m_state == State::Writing && m_write_finished.load() && m_write_ok.load();
}

Isometric_Capture::Stage Isometric_Capture::get_stage() const {
    switch (m_state) {
        case State::Loading:
            switch (static_cast<Chunk_Manager::Capture_Phase>(m_loading_phase)) {
                case Chunk_Manager::Capture_Phase::Generating: return Stage::Generating;
                case Chunk_Manager::Capture_Phase::Lighting:   return Stage::Lighting;
                case Chunk_Manager::Capture_Phase::Meshing:
                case Chunk_Manager::Capture_Phase::Ready:      return Stage::Meshing;
                default:                                       return Stage::Planning;
            }
        case State::Rendering: return Stage::Rendering;
        case State::Writing:   return Stage::Saving;
        default:               return Stage::Finished;
    }
}

float Isometric_Capture::get_progress() const {
    switch (m_state) {
        case State::Loading:
            return m_loading_progress * k_loading_share;
        case State::Rendering: {
            const float tiles = static_cast<float>(std::max(1, m_tiles_x * m_tiles_y));
            return k_loading_share + (1.0f - k_loading_share) * (static_cast<float>(m_tiles_done) / tiles);
        }
        default:
            return 1.0f;
    }
}

void Isometric_Capture::close_region() {
    if (!m_region_open) return;
    m_region_open = false;
    m_chunks.end_capture_region();
}

void Isometric_Capture::fail(const std::string& message) {
    m_error = message;
    destroy_framebuffer();
    close_region();
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_state = State::Failed;
    LOG_ERROR("Isometric capture failed: " + message);
}

// ---------------------------------------------------------------------------------------------
//  Камера и размер картинки
// ---------------------------------------------------------------------------------------------
bool Isometric_Capture::prepare_render() {
    const std::vector<Renderable_Chunk> renderables = m_chunks.get_renderable_chunks(nullptr, nullptr, -1);
    if (renderables.empty()) {
        fail("nothing to render");
        return false;
    }

    if (std::getenv("OPTICRAFT_ISO_DEBUG")) {
        size_t sections = 0;
        for (const auto& r : renderables) if (r.meshes) for (const auto& s : r.meshes->sections) sections += s.has_geometry ? 1 : 0;
        LOG_INFO("iso chunks=" + std::to_string(renderables.size()) + " sections with geometry=" + std::to_string(sections));
    }

    // Целимся в центр чанка игрока: так изображение симметрично вокруг него.
    const glm::vec3 target(
        std::floor(m_center.x / static_cast<float>(Config::chunk_size)) * Config::chunk_size + Config::chunk_size * 0.5f,
        m_center.y,
        std::floor(m_center.z / static_cast<float>(Config::chunk_size)) * Config::chunk_size + Config::chunk_size * 0.5f);

    const float pitch = Config::iso_capture_pitch_degrees * k_pi / 180.0f;
    const float yaw = Config::iso_capture_yaw_degrees * k_pi / 180.0f;
    const glm::vec3 to_camera(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    m_view = glm::lookAt(target + to_camera * k_camera_distance, target, glm::vec3(0.0f, 1.0f, 0.0f));

    // Рамка кадра — по реальной геометрии (секциям мешей), а не по всему возможному объёму
    // мира: картинка получается плотно обрезанной и использует каждый пиксель.
    float min_x = std::numeric_limits<float>::max(), max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max(), max_y = std::numeric_limits<float>::lowest();
    float min_d = std::numeric_limits<float>::max(), max_d = std::numeric_limits<float>::lowest();
    for (const Renderable_Chunk& chunk : renderables) {
        const float x0 = static_cast<float>(chunk.position.x * Config::chunk_size);
        const float z0 = static_cast<float>(chunk.position.z * Config::chunk_size);
        const float x1 = x0 + static_cast<float>(Config::chunk_size);
        const float z1 = z0 + static_cast<float>(Config::chunk_size);
        for (const Chunk_Section_Range& section : chunk.meshes->sections) {
            if (!section.has_geometry) continue;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec4 world((corner & 1) ? x1 : x0,
                                      (corner & 2) ? section.max_y : section.min_y,
                                      (corner & 4) ? z1 : z0, 1.0f);
                const glm::vec4 v = m_view * world;
                min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
                min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
                min_d = std::min(min_d, -v.z); max_d = std::max(max_d, -v.z);
            }
        }
    }
    if (min_x > max_x) {
        fail("no visible geometry");
        return false;
    }

    constexpr float margin = 2.0f; // блоков по краям кадра
    min_x -= margin; max_x += margin;
    min_y -= margin; max_y += margin;
    const float extent_x = max_x - min_x;
    const float extent_y = max_y - min_y;

    m_scale = std::min(k_max_pixels_per_block,
                       static_cast<float>(Config::iso_capture_max_image_size) / std::max(extent_x, extent_y));
    m_image_w = std::max(1, static_cast<int>(std::ceil(extent_x * m_scale)));
    m_image_h = std::max(1, static_cast<int>(std::ceil(extent_y * m_scale)));
    m_min_x = min_x;
    m_max_y = max_y;
    m_near = std::max(0.1f, min_d - 5.0f);
    m_far = max_d + 5.0f;

    GLint max_renderbuffer = 4096;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    GLint max_viewport[2] = {4096, 4096};
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport);
    m_tile_size = std::min({Config::iso_capture_tile_size, static_cast<int>(max_renderbuffer),
                            static_cast<int>(max_viewport[0]), static_cast<int>(max_viewport[1]),
                            std::max(m_image_w, m_image_h)});
    m_tile_size = std::max(m_tile_size, 16);
    m_tiles_x = (m_image_w + m_tile_size - 1) / m_tile_size;
    m_tiles_y = (m_image_h + m_tile_size - 1) / m_tile_size;
    m_tiles_done = 0;

    try {
        m_pixels.assign(static_cast<size_t>(m_image_w) * static_cast<size_t>(m_image_h) * 3u, 0);
    } catch (const std::bad_alloc&) {
        fail("not enough memory for the image");
        return false;
    }

    if (!create_framebuffer()) {
        fail("cannot create framebuffer");
        return false;
    }

    LOG_INFO("Isometric capture: image " + std::to_string(m_image_w) + "x" + std::to_string(m_image_h) +
             ", " + std::to_string(m_tiles_x * m_tiles_y) + " tile(s) of " + std::to_string(m_tile_size) +
             " px, " + std::to_string(m_scale) + " px/block");
    return true;
}

// ---------------------------------------------------------------------------------------------
//  FBO и рендер тайлов
// ---------------------------------------------------------------------------------------------
bool Isometric_Capture::create_framebuffer() {
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);

    while (glGetError() != GL_NO_ERROR) {} // сбросить чужие ошибки

    glGenFramebuffers(1, &m_fbo);
    glGenRenderbuffers(1, &m_color_rb);
    glGenRenderbuffers(1, &m_depth_rb);

    glBindRenderbuffer(GL_RENDERBUFFER, m_color_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_tile_size, m_tile_size);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_tile_size, m_tile_size);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth_rb);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                          glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
    return complete;
}

void Isometric_Capture::destroy_framebuffer() {
    if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_color_rb) { glDeleteRenderbuffers(1, &m_color_rb); m_color_rb = 0; }
    if (m_depth_rb) { glDeleteRenderbuffers(1, &m_depth_rb); m_depth_rb = 0; }
}

bool Isometric_Capture::render_tile(int tile_index) {
    const int tile_x = tile_index % m_tiles_x;
    const int tile_y = tile_index / m_tiles_x;
    const int x0 = tile_x * m_tile_size;
    const int y0 = tile_y * m_tile_size;
    const int tile_w = std::min(m_tile_size, m_image_w - x0);
    const int tile_h = std::min(m_tile_size, m_image_h - y0);

    // Окно проекции этого тайла в видовых координатах (1 пиксель = 1/scale блока).
    const float left = m_min_x + static_cast<float>(x0) / m_scale;
    const float right = left + static_cast<float>(tile_w) / m_scale;
    const float top = m_max_y - static_cast<float>(y0) / m_scale;
    const float bottom = top - static_cast<float>(tile_h) / m_scale;
    const glm::mat4 projection = glm::ortho(left, right, bottom, top, m_near, m_far);

    // Ортографический фрустум отсекает всё, что не попадает в этот тайл — тайлы дешевле целого.
    const Frustum frustum(projection * m_view);
    const std::vector<Renderable_Chunk> renderables = m_chunks.get_renderable_chunks(&frustum, nullptr, -1);

    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    GLint previous_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, previous_viewport);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, tile_w, tile_h);
    m_renderer.clear_screen(m_sky_color.r, m_sky_color.g, m_sky_color.b);
    const bool occlusion_was_enabled = m_renderer.is_occlusion_enabled();
    m_renderer.set_occlusion_enabled(false);
    m_renderer.set_gpu_sync_residency(true);   // снимок обязан быть полным: все чанки на GPU до отрисовки тайла
    m_renderer.render_chunks(renderables, m_view, projection, m_ambient);
    m_renderer.set_gpu_sync_residency(false);
    m_renderer.set_occlusion_enabled(occlusion_was_enabled);

    std::vector<uint8_t> tile;
    try {
        tile.resize(static_cast<size_t>(tile_w) * static_cast<size_t>(tile_h) * 3u);
    } catch (const std::bad_alloc&) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
        glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        return false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, tile_w, tile_h, GL_RGB, GL_UNSIGNED_BYTE, tile.data());
    const bool ok = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    if (!ok) return false;

    // OpenGL читает снизу вверх, а PNG и наша картинка хранятся сверху вниз.
    const size_t row_bytes = static_cast<size_t>(tile_w) * 3u;
    for (int y = 0; y < tile_h; ++y) {
        const int image_row = y0 + (tile_h - 1 - y);
        uint8_t* destination = m_pixels.data() +
            (static_cast<size_t>(image_row) * static_cast<size_t>(m_image_w) + static_cast<size_t>(x0)) * 3u;
        std::copy_n(tile.data() + static_cast<size_t>(y) * row_bytes, row_bytes, destination);
    }
    return true;
}

void Isometric_Capture::finish_render() {
    destroy_framebuffer();
    // Область больше не нужна: возвращаем обычную загрузку мира (срез освободится сам).
    close_region();

    m_write_finished = false;
    m_write_ok = false;
    m_state = State::Writing;

    // PNG большой картинки кодируется секунды — делаем это в фоне, игра продолжается.
    std::vector<uint8_t> pixels = std::move(m_pixels);
    m_pixels = {};
    const int width = m_image_w;
    const int height = m_image_h;
    const std::string path = m_output_path;
    m_writer = std::thread([this, pixels = std::move(pixels), width, height, path]() {
        bool ok = false;
        try {
            std::error_code ec;
            const std::filesystem::path file(path);
            if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
            stbi_write_png_compression_level = 6;
            ok = stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3) != 0;
        } catch (...) {
            ok = false;
        }
        m_write_ok = ok;
        m_write_finished = true;
    });
    LOG_INFO("Isometric capture: writing " + path);
}
