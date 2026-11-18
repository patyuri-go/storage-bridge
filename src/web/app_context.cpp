#include "web/app_context.hpp"

namespace sb::web {

AppContext& AppContext::instance() {
  static AppContext ctx;
  return ctx;
}

} // namespace sb::web