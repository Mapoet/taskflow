#include <agent/vectorstore/vectorstore.hpp>
#include <agent/encoder/encoder.hpp>
#include <node/knowledge_base_node.hpp>
#include <workflow/nodeflow.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>

using namespace agent_framework;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

Document document(std::string id, std::string content, std::string modality = "text") {
    Document value;
    value.doc_id = std::move(id);
    value.metadata.doc_id = value.doc_id;
    value.metadata.content = std::move(content);
    value.metadata.modality = std::move(modality);
    value.metadata.extra_metadata["tenant"] = id == "a" ? "tenant-a" : "tenant-b";
    value.metadata.extra_metadata["quality"] = id == "a" ? json(2) : json(1);
    return value;
}
}

int main() {
    VectorStore store(std::make_unique<InMemoryVectorStoreBackend>(3));
    store.insert(document("a", "alpha"), {1, 0, 0});
    store.insert(document("b", "beta"), {0, 1, 0});
    store.insert(document("c", "gamma", "image"), {1, 0, 0});
    const auto all = store.search({1, 0, 0}, 3);
    require(all.size() == 3 && all[0].doc_id == "a" && all[1].doc_id == "c", "search ordering contract failed");
    const auto text = store.search({1, 0, 0}, 3, "text");
    require(text.size() == 2 && text[0].doc_id == "a", "modality filter contract failed");
    const auto filtered = store.search({1, 0, 0}, 3, "text",
                                       {{"tenant", "tenant-a"}, {"quality", 2}});
    require(filtered.size() == 1 && filtered.front().doc_id == "a",
            "metadata filter contract failed");
    require(store.search({1, 0, 0}, 3, "text", {{"missing", true}}).empty(),
            "missing metadata key matched");
    bool invalid = false;
    try { store.insert(document("bad", "bad"), {1, 0}); } catch(const std::invalid_argument&) { invalid = true; }
    require(invalid, "invalid dimension accepted");
    invalid = false;
    try { store.insert(document("zero", "zero"), {0, 0, 0}); } catch(const std::invalid_argument&) { invalid = true; }
    require(invalid, "zero vector accepted");
    invalid = false;
    try { store.insert(document("nan", "nan"), {std::numeric_limits<float>::quiet_NaN(), 0, 0}); } catch(const std::invalid_argument&) { invalid = true; }
    require(invalid, "NaN vector accepted");
    require(store.update_document(document("b", "beta-updated"), {1, 0, 0}), "update failed");
    const auto updated = store.search({1, 0, 0}, 3, "text");
    require(updated.size() == 2 && updated[0].doc_id == "a" && updated[1].doc_id == "b", "update visibility contract failed");
    require(store.delete_document("b") && !store.delete_document("b"), "delete idempotence contract failed");
    require(store.search({1, 0, 0}, 3, "text").size() == 1, "deleted vector remained visible");

    const auto persisted = std::filesystem::temp_directory_path() / "agent-framework-vectorstore-wp31.json";
    require(store.save_index(persisted.string()), "save failed");
    VectorStore restored(std::make_unique<InMemoryVectorStoreBackend>());
    require(restored.load_index(persisted.string()), "load failed");
    const auto restored_text = restored.search({1, 0, 0}, 3, "text");
    require(restored_text.size() == 1 && restored_text[0].doc_id == "a", "restored data mismatch");
    std::filesystem::remove(persisted);

    auto retrieval_store = std::make_shared<VectorStore>(
        std::make_unique<InMemoryVectorStoreBackend>(384));
    auto encoders = std::make_shared<EncoderManager>();
    auto encoder = std::make_shared<TextEncoder>();
    encoders->register_encoder("text", encoder);
    retrieval_store->insert(document("rag", "GNSS radio occultation profile"),
                            encoder->encode("GNSS radio occultation profile"));
    workflow::GraphBuilder builder;
    auto [knowledge_source, task] = agent_framework::node::KnowledgeBaseSourceNode::create(
        builder, "knowledge", retrieval_store, encoders);
    (void)task;
    agent_framework::node::KnowledgeBaseSourceNode::set_query(
        knowledge_source, "GNSS radio occultation profile", 1);
    const auto results = std::any_cast<std::vector<RetrievalResult>>(knowledge_source->values.at("results"));
    const auto context = std::any_cast<std::string>(knowledge_source->values.at("context"));
    require(results.size() == 1 && results.front().doc_id == "rag", "knowledge retrieval mismatch");
    require(context.find("GNSS radio occultation") != std::string::npos, "knowledge context missing");
    std::cout << "test_vectorstore_wp31: ok\n";
}
