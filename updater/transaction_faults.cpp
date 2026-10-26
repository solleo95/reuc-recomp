#include "release.h"
namespace uc::update::detail {
// Tests link their own checkpoint implementation; the shipping helper has no fault controls.
Status checkpoint(std::string_view) { return std::monostate{}; }
}
