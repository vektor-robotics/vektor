#pragma once

#include "vektor/run.hpp"

#include <string>
#include <vector>

namespace vektor {

void initialize_run_event_sources(RunManifest &manifest);
std::vector<RunEvent> collect_run_source_events(const RunManifest &manifest,
                                                const std::string &stopped_at);

} // namespace vektor
