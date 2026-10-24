// Addressable LED strip (WS2812/SK6812 over RMT) with a few effects driven
// from the application state (LightSettings + desk status).
#pragma once

namespace app {
class App;
}

namespace leds {

void start(app::App *app);

}  // namespace leds
