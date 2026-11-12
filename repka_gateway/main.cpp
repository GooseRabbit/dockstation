#include "application/linux/application_bootstrap.hpp"

int main(int argc, char* argv[]) {
    const auto result =
        dockstation::application::linux_app::run_application(
            argc, const_cast<const char* const*>(argv));
    return static_cast<int>(result.exit_code);
}
