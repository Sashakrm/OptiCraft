//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_EVENTS_H
#define OPTICRAFT_EVENTS_H

#include <functional>
#include <unordered_map>
#include <vector>
#include <string>

using Event_ID = std::string;
using Event_Callback = std::function<void(void*)>;

class Event_Bus {
private:
    std::unordered_map<Event_ID, std::vector<Event_Callback>> m_listeners;

    Event_Bus() = default;

public:
    static Event_Bus& get_instance() {
        static Event_Bus instance;
        return instance;
    }

    void subscribe(const Event_ID& event_name, Event_Callback callback) {
        m_listeners[event_name].push_back(callback);
    }

    void publish(const Event_ID& event_name, void* data = nullptr) {
        auto it = m_listeners.find(event_name);
        if (it != m_listeners.end()) {
            for (auto& callback : it->second) {
                callback(data);
            }
        }
    }

    void unsubscribe_all(const Event_ID& event_name) {
        m_listeners.erase(event_name);
    }
};

// Предопределённые события
namespace Events {
    constexpr Event_ID block_broken = "block_broken";
    constexpr Event_ID block_placed = "block_placed";
    constexpr Event_ID player_jump = "player_jump";
    constexpr Event_ID chunk_loaded = "chunk_loaded";
    constexpr Event_ID window_resized = "window_resized";
}

#endif //OPTICRAFT_EVENTS_H
