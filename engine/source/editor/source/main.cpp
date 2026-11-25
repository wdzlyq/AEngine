#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>

#include "runtime/engine.h"

#include "editor/include/editor.h"

// https://gcc.gnu.org/onlinedocs/cpp/Stringizing.html
#define PICCOLO_XSTR(s) PICCOLO_STR(s)
#define PICCOLO_STR(s) #s

int main(int argc, char** argv)
{
    std::filesystem::path executable_path(argv[0]);
    std::filesystem::path config_file_path = executable_path.parent_path() / "PiccoloEditor.ini";

    // Parse flags
    // --headless [max_ticks]  Run without a display window; engine exits after max_ticks frames.
    bool headless    = false;
    int  max_ticks   = -1;  // -1 = run until shutdown by agent command

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if (arg == "--headless")
        {
            headless = true;
            if (i + 1 < argc)
            {
                try { max_ticks = std::stoi(argv[i + 1]); ++i; }
                catch (...) { /* next arg is not a number, leave as -1 */ }
            }
        }
        else if (arg == "--config" && i + 1 < argc)
        {
            config_file_path = argv[++i];
        }
    }

    Piccolo::g_is_headless = headless;

    Piccolo::PiccoloEngine* engine = new Piccolo::PiccoloEngine();

    engine->startEngine(config_file_path.generic_string());
    engine->initialize();

    if (headless)
    {
        // Headless mode: run the engine loop without editor UI.
        engine->runHeadless(max_ticks);
    }
    else
    {
        Piccolo::PiccoloEditor* editor = new Piccolo::PiccoloEditor();
        editor->initialize(engine);

        editor->run();

        editor->clear();
        delete editor;
    }

    engine->clear();
    engine->shutdownEngine();
    delete engine;

    return 0;
}
