#include "chem/channel/updater.h"

#include <spdlog/fmt/fmt.h>

using namespace chem;
using namespace chem::channel;

Updater::Updater() : logger(spdlog::get("CHEM")) {}

void Updater::Start() {}
