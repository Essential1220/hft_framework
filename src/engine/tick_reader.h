#pragma once

#include "common/types.h"

#include <string>
#include <vector>

namespace hft {

// Read .htick binary files produced by TickRecorder
bool read_htick_file(const std::string& filepath, std::vector<TickData>& out);

// Merge multiple tick vectors by time order (update_time + update_millisec)
void merge_ticks_by_time(std::vector<std::vector<TickData>>& sources,
                         std::vector<TickData>& merged);

} // namespace hft
