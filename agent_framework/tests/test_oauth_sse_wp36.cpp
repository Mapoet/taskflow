#include <agent/agent_client/agent_client.hpp>
#include <agent/agent_transport/sse_connection.hpp>

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <vector>

using namespace agent_framework;

namespace {
class RecordingHttpClient final : public HTTPClient {
public:
    json post(const std::string&, const json&,
              const std::map<std::string, std::string>&) override { return json::object(); }
    json get(const std::string&,
             const std::map<std::string, std::string>&) override { return json::object(); }
    void get_sse(const std::string&, const std::map<std::string, std::string>& headers,
                 const std::function<void(std::string_view)>&, int,
                 const std::atomic<bool>*) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            observed.push_back(headers);
        }
        cv.notify_all();
    }
    void wait_for(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return observed.size() >= count; });
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::map<std::string, std::string>> observed;
};
}

int main() {
    RecordingHttpClient http;
    SSEConnection connection("http://example.invalid/events", "task", &http);
    connection.subscribe({{"Authorization", "Bearer old"}}, {}, {});
    http.wait_for(1);
    connection.reconnect("41", {{"Authorization", "Bearer new"}});
    http.wait_for(2);
    connection.close();
    std::lock_guard<std::mutex> lock(http.mutex);
    assert(http.observed[0].at("Authorization") == "Bearer old");
    assert(http.observed[1].at("Authorization") == "Bearer new");
    assert(http.observed[1].at("Last-Event-ID") == "41");
}
