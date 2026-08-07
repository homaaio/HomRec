#include "hr_input_overlay_registry.h"

namespace {
std::vector<HrInputOverlaySource> g_sources;
}

namespace HrInputOverlayRegistry {

void Add(const HrInputOverlaySource &src) { g_sources.push_back(src); }
void Clear() { g_sources.clear(); }
const std::vector<HrInputOverlaySource> &All() { return g_sources; }

} // namespace HrInputOverlayRegistry
