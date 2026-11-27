#ifndef TIRAMISU_PORTAL_DROP_H
#define TIRAMISU_PORTAL_DROP_H

#include <string>

namespace longan {

// Poll while the launcher is idle. ZIP extraction runs on a background thread.
void portal_drop_poll(const std::string& local_state, const std::string& drop_dir);

}  // namespace longan

#endif
