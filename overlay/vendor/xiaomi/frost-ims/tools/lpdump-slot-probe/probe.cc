#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <android-base/logging.h>

// Test a full native library in this private process, without invoking the
// installed Binder service. An expected old CHECK cannot create a tombstone.
static void controlled_abort(int) {
    static const char message[] = "C40_CONTROLLED_ABORT\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(86);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    android::base::InitLogging(argv, &android::base::StderrLogger);
    signal(SIGABRT, controlled_abort);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << dlerror() << "\n";
        return 87;
    }
    using Main = int (*)(int, char**, std::ostream&, std::ostream&);
    auto entry = reinterpret_cast<Main>(dlsym(library,
        "_Z10LpdumpMainiPPcRNSt3__113basic_ostreamIcNS1_11char_traitsIcEEEES6_"));
    if (!entry) {
        std::cerr << dlerror() << "\n";
        return 88;
    }
    std::vector<std::string> arguments = {"lpdump"};
    for (int i = 2; i < argc; i++) arguments.emplace_back(argv[i]);
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    std::ostringstream output, error;
    int result = entry(static_cast<int>(arguments.size()), pointers.data(), output, error);
    std::cout << output.str();
    std::cerr << error.str();
    return result;
}
