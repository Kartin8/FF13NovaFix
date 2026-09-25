#pragma once

namespace novafix::game::ff13_aspect_ratio {

// Shared by the GUI draw hook and backdrop capture hooks on the same thread.
extern thread_local unsigned g_guiDrawDepth;

}
