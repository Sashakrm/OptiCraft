#include "core/Game.h"
#include "utils/Logger.h"
#include "utils/Config.h"


int main() {
    Game game(
        Config::default_window_width,
        Config::default_window_height,
        Config::default_window_title
    );

    if (!game.initialize()) {
        LOG_ERROR("Failed to initialize game");
        return 1;
    }

    game.run();

    LOG_INFO("OptiCraft exited");
    return 0;
}