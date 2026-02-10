// #include <iostream>
// #include <print>

#include "ChatApp.h"

#include "Base.h"
#include "GUI.h"

#include "Server.h"

enum RunType { SERVER, CLIENT };

int main(int argc, char* argv[]) {
        RunType run_type = CLIENT;

        if (argc > 1) {
                std::string type = argv[1];

                if (type == "server") run_type = SERVER;
        }

        switch (run_type) {
        case SERVER: {
                Server server;
                server.Init();
                server.Run();
                server.Shutdown();
        }
                return 0;
        case CLIENT: {
                GUI();
        }
                return 0;
        }
}