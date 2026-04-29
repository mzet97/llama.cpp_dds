// smoke_test.cpp
// Teste minimo: cria V4Bridge, init, start, stop. Sem inferencia real.
// Apenas confirma que a biblioteca linka e o bridge inicializa entidades DDS.
//
// Compilar:
//   clang++ -std=c++17 \
//     -I/Users/zeitune/.local/include -I. \
//     smoke_test.cpp libv4_bridge.a \
//     -L/Users/zeitune/.local/lib -lddsc \
//     -o smoke_test
//
// Rodar:
//   DYLD_LIBRARY_PATH=/Users/zeitune/.local/lib ./smoke_test

#include "dds_v4_bridge.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace llama_dds_v4;

    BridgeConfig cfg;
    cfg.domain_id      = 200;
    cfg.agent_id       = "agent-smoke-001";
    cfg.hostname       = "localhost";
    cfg.model          = "qwen3.5-0.8b";
    cfg.specialization = "TEXT";
    cfg.slots_total    = 1;
    cfg.heartbeat_interval = std::chrono::milliseconds(500);
    cfg.metrics_interval   = std::chrono::milliseconds(1000);

    V4Bridge bridge(cfg);
    bridge.set_task_handler([](const TaskInfo & t) {
        std::cout << "task recebida: " << t.task_id << "\n";
    });

    if (!bridge.init()) {
        std::cerr << "init falhou\n";
        return 1;
    }
    std::cout << "init ok\n";

    if (!bridge.start()) {
        std::cerr << "start falhou\n";
        return 2;
    }
    std::cout << "start ok\n";

    // Pequeno tempo para o heartbeat publicar pelo menos uma vez
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    bridge.publish_token("dummy-task", 0, "ola", true,
                         FinishReason::COMPLETION);
    std::cout << "publish_token ok\n";

    bridge.stop();
    std::cout << "stop ok\n";

    return 0;
}
