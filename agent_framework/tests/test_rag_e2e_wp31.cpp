#include <agent/encoder/encoder.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/vectorstore/vectorstore.hpp>

#include <future>
#include <stdexcept>
#include <taskflow/taskflow.hpp>

using namespace agent_framework;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

Document make_document(std::string id, std::string content) {
    Document document;
    document.doc_id = std::move(id);
    document.metadata.doc_id = document.doc_id;
    document.metadata.modality = "text";
    document.metadata.content = std::move(content);
    document.metadata.extra_metadata["tenant"] = "science";
    return document;
}

class CitationProbeAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(const LLMInput&,
                                 std::function<void(std::string_view)> = nullptr) override {
        throw std::logic_error("RAG E2E must pass through PromptRenderer");
    }
    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> = nullptr) override {
        const std::string wire = json(rendered.messages).dump() + rendered.rendered_text;
        require(wire.find("[source:gnss-ro]") != std::string::npos, "citation missing from rendered prompt");
        require(wire.find("unrelated ocean colour") == std::string::npos, "unretrieved document leaked into prompt");
        std::promise<LLMOutput> promise;
        LLMOutput output;
        output.is_final = true;
        output.final_answer = "Bending angle profiles constrain refractivity [source:gnss-ro].";
        promise.set_value(std::move(output));
        return promise.get_future();
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "rag-probe"; }
    bool supports_multimodal() const override { return false; }
};
}

int main() {
    auto store = std::make_shared<VectorStore>(
        std::make_unique<InMemoryVectorStoreBackend>(384));
    auto encoders = std::make_shared<EncoderManager>();
    auto encoder = std::make_shared<TextEncoder>();
    encoders->register_encoder("text", encoder);

    const std::string query = "How does GNSS radio occultation constrain refractivity?";
    store->insert(make_document("gnss-ro", query + " Bending angle profiles are assimilated."),
                  encoder->encode(query));
    store->insert(make_document("ocean-colour", "unrelated ocean colour calibration"),
                  encoder->encode("ocean colour chlorophyll calibration"));
    store->insert(make_document("weather-radar", "unrelated radar reflectivity"),
                  encoder->encode("weather radar reflectivity"));

    auto client = std::make_shared<LLMClient>();
    client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    client->register_adapter("probe", std::make_shared<CitationProbeAdapter>());
    client->set_default_adapter("probe");
    ExecutionRequest request;
    request.config.system_prompt = "Answer only from retrieved evidence and preserve source IDs.";
    request.config.max_iterations = 1;
    request.config.extra_config["RAG_TOP_K"] = 1;
    request.config.extra_config["RAG_MODALITY"] = "text";
    request.config.extra_config["RAG_METADATA_FILTER"] = json{{"tenant", "science"}};
    request.deps = {client, std::make_shared<ToolBus>(), nullptr, nullptr, store, encoders};
    request.session = std::make_shared<internal::AgentThreadState>();
    request.session->initial_user_prompt = query;
    request.context.session_id = "rag-production-e2e";
    request.options.persist_session = false;
    request.options.input_already_processed = true;
    std::string final_answer;
    request.options.react.sink.on_final_json = [&](const json& value) {
        final_answer = value.value("final_answer", "");
    };
    std::size_t assembled_events = 0;
    request.event_sink = [&](const ExecutionEvent& event) {
        if(event.type == ExecutionEventType::MemoryAssembled) ++assembled_events;
    };
    tf::Executor executor(2);
    GraphExecutor graph_executor;
    const auto output = graph_executor.execute_sync(executor, std::move(request));
    if(!output.success) throw std::runtime_error(
        "GraphExecutor RAG path failed: " + output.error.value_or("unknown"));
    require(assembled_events == 1, "RAG prompt did not pass through MemoryAssembly");
    require(final_answer.find("[source:gnss-ro]") != std::string::npos,
            "final answer lost citation");
    require(final_answer.find("ocean-colour") == std::string::npos,
            "final answer cited unretrieved source");
}
