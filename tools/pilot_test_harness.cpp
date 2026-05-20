#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

typedef void (*AsyncCallback)(int success, const char* message, void* user_data);

typedef void (*fn_logos_core_init)(int, char**);
typedef void (*fn_logos_core_set_plugins_dir)(const char*);
typedef void (*fn_logos_core_start)();
typedef int  (*fn_logos_core_exec)();
typedef void (*fn_logos_core_cleanup)();
typedef int  (*fn_logos_core_load_plugin)(const char*);
typedef void (*fn_logos_core_call_plugin_method_async)(
    const char*, const char*, const char*, AsyncCallback, void*);
typedef void (*fn_logos_core_process_events)();

static bool g_done = false;

static void on_method_result(int success, const char* message, void* user_data) {
    const char* method = (const char*)user_data;
    if (success) {
        printf("\n=== RESULT [%s] ===\n%s\n==================\n", method, message);
    } else {
        printf("\n=== ERROR [%s] ===\n%s\n==================\n", method, message);
    }
    fflush(stdout);
    g_done = true;
}

static void on_load_result(int success, const char* message, void* user_data) {
    printf("Plugin load: %s — %s\n", success ? "OK" : "FAIL", message);
    fflush(stdout);
    g_done = true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: pilot_test <plugins_dir> <method> [params_json]\n");
        printf("Example: pilot_test /tmp/pilot-test/modules echo '[{\"name\":\"input\",\"value\":\"hello\",\"type\":\"string\"}]'\n");
        return 1;
    }

    const char* plugins_dir = argv[1];
    const char* method = argv[2];
    const char* params = argc > 3 ? argv[3] : "[]";

    void* lib = dlopen("liblogos_core.so", RTLD_NOW);
    if (!lib) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto core_init = (fn_logos_core_init)dlsym(lib, "logos_core_init");
    auto core_set_dir = (fn_logos_core_set_plugins_dir)dlsym(lib, "logos_core_set_plugins_dir");
    auto core_start = (fn_logos_core_start)dlsym(lib, "logos_core_start");
    auto core_exec = (fn_logos_core_exec)dlsym(lib, "logos_core_exec");
    auto core_cleanup = (fn_logos_core_cleanup)dlsym(lib, "logos_core_cleanup");
    auto load_plugin = (fn_logos_core_load_plugin)dlsym(lib, "logos_core_load_plugin");
    auto call_method = (fn_logos_core_call_plugin_method_async)dlsym(lib, "logos_core_call_plugin_method_async");
    auto process_events = (fn_logos_core_process_events)dlsym(lib, "logos_core_process_events");

    if (!core_init || !core_start || !call_method) {
        printf("Failed to load symbols from liblogos_core.so\n");
        dlclose(lib);
        return 1;
    }

    core_init(0, nullptr);
    core_set_dir(plugins_dir);
    core_start();

    // Load the pilot plugin
    g_done = false;
    printf("Loading pilot module...\n");
    int loaded = load_plugin("pilot");
    if (!loaded) {
        printf("Direct load failed, module may need subprocess host\n");
    }

    // Wait for plugin to be ready
    for (int i = 0; i < 50 && !g_done; i++) {
        process_events();
        usleep(100000);
    }

    // Call the method
    g_done = false;
    printf("Calling pilot.%s...\n", method);
    call_method("pilot", method, params, on_method_result, (void*)method);

    // Process events until result arrives
    for (int i = 0; i < 100 && !g_done; i++) {
        process_events();
        usleep(100000);
    }

    if (!g_done) {
        printf("Timed out waiting for result\n");
    }

    core_cleanup();
    dlclose(lib);
    return g_done ? 0 : 1;
}
