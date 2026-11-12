#include <vector>

#include "application/linux/application_bootstrap.hpp"

int main(int argc, char* argv[]) {
    std::vector<const char*> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(argv[index]);
    }

    const auto result =
        dockstation::application::linux_app::run_application(
            argc, arguments.data());
    return static_cast<int>(result.exit_code);
}
