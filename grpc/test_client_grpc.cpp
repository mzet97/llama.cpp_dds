/**
 * Simple gRPC client for testing llama.cpp gRPC transport.
 * Sends chat completion requests via gRPC and receives responses.
 * Mirrors dds/test_client.cpp for fair comparison.
 */

#include "grpc_transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

// Shared UUID generator
#include "dds_types.h"

static std::string generate_uuid() {
    // Simple UUID for test client
    static std::atomic<int> counter{ 0 };
    return "grpc-test-" + std::to_string(counter.fetch_add(1));
}

int main(int argc, char * argv[]) {
    std::string address = "localhost:50051";
    if (argc > 1) {
        address = argv[1];
    }

    std::string prompt = "What is 2+2?";
    if (argc > 2) {
        prompt = argv[2];
    }

    std::cout << "=== gRPC Test Client ===" << std::endl;
    std::cout << "Server address: " << address << std::endl;
    std::cout << "Prompt: " << prompt << std::endl;

    std::atomic<bool> response_complete{ false };
    std::string       accumulated_response;

    llama_grpc::GRPCTransport transport(address);

    // Set up response callback
    transport.subscribe_responses([&](const llama_dds::ChatCompletionResponse & resp) {
        accumulated_response += resp.content;

        if (resp.is_final) {
            std::cout << "\n=== Response ===" << std::endl;
            std::cout << accumulated_response << std::endl;
            std::cout << "Prompt tokens:     " << resp.prompt_tokens << std::endl;
            std::cout << "Completion tokens: " << resp.completion_tokens << std::endl;
            if (resp.finish_reason.has_value()) {
                std::cout << "Finish reason:     " << resp.finish_reason.value() << std::endl;
            }
            response_complete.store(true);
        } else {
            std::cout << resp.content << std::flush;
        }
    });

    // Optional: subscribe to status
    transport.subscribe_status([&](const llama_dds::ServerStatus & status) {
        std::cout << "[Status] server=" << status.server_id
                  << " idle=" << status.slots_idle
                  << " processing=" << status.slots_processing
                  << " model=" << status.model_loaded
                  << " ready=" << (status.ready ? "yes" : "no")
                  << std::endl;
    });

    if (!transport.start_client()) {
        std::cerr << "Failed to start gRPC client" << std::endl;
        return 1;
    }

    // Build and send request
    llama_dds::ChatCompletionRequest request;
    request.request_id = generate_uuid();
    request.model      = "default";
    request.messages.push_back({ "user", prompt });
    request.temperature = 0.7f;
    request.max_tokens  = 256;
    request.stream      = true;

    auto t_start = std::chrono::high_resolution_clock::now();

    transport.send_request(request);

    // Wait for response with timeout
    int timeout_secs = 120;
    for (int i = 0; i < timeout_secs * 10; i++) {
        if (response_complete.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto t_end     = std::chrono::high_resolution_clock::now();
    auto elapsed   = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start);

    if (!response_complete.load()) {
        std::cerr << "Timeout: no response received within " << timeout_secs << "s" << std::endl;
        transport.stop_client();
        return 1;
    }

    std::cout << "\nTotal time: " << elapsed.count() << " ms" << std::endl;

    transport.stop_client();
    return 0;
}
