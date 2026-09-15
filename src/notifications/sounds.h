#pragma once

namespace novafix::notifications::sounds {

// Called after a successful presentation. The first call starts the delay;
// later calls play the startup sound once the game owns the foreground.
void OnSuccessfulPresent();
void PlayMenuTransition(bool opening);

} // namespace novafix::notifications::sounds
