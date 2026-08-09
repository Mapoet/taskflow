#include <agent/encoder/encoder.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/memory/memory_assembly.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/vectorstore/vectorstore.hpp>
#include <node/knowledge_base_node.hpp>
#include <workflow/nodeflow.hpp>

#include <future>
#include <stdexcept>

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

    workflow::GraphBuilder builder;
    auto [source, task] = agent_framework::node::KnowledgeBaseSourceNode::create(
        builder, "rag", store, encoders);
    (void)task;
    agent_framework::node::KnowledgeBaseSourceNode::set_query(source, query, 1, "text");
    const auto results = std::any_cast<std::vector<RetrievalResult>>(source->values.at("results"));
    const auto citations = std::any_cast<std::vector<Citation>>(source->values.at("citations"));
    const auto context = std::any_cast<std::string>(source->values.at("context"));
    require(results.size() == 1 && results.front().doc_id == "gnss-ro", "retrieval selected wrong document");
    require(citations.size() == 1 && citations.front().doc_id == "gnss-ro", "citation mapping mismatch");
    require(context.find("[source:gnss-ro]") != std::string::npos, "stable source id missing from context");

    MemoryAssemblyInput assembly_input;
    assembly_input.retrieval.push_back(
        {MemorySlotKind::Retrieval, "gnss-ro", 50, 0, context,
         json{{"citation_id", "gnss-ro"}}});
    MemoryAssemblyPolicy policy;
    policy.hard_limit_bytes = 2048;
    const auto assembled = assemble_memory(assembly_input, policy);

    auto client = std::make_shared<LLMClient>();
    client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    client->register_adapter("probe", std::make_shared<CitationProbeAdapter>());
    client->set_default_adapter("probe");
    LLMInput input;
    input.system_prompt = "Answer only from retrieved evidence and preserve source IDs.";
    input.user_prompt = query;
    input.context = assembled.text;
    const auto output = client->invoke(input).get();
    require(output.is_final, "mock LLM output was not final");
    require(output.final_answer.find("[source:gnss-ro]") != std::string::npos, "final answer lost citation");
    require(output.final_answer.find("ocean-colour") == std::string::npos, "final answer cited unretrieved source");
}
