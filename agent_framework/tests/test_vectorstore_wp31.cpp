#include <agent/vectorstore/vectorstore.hpp>
#include <agent/encoder/encoder.hpp>
#include <node/knowledge_base_node.hpp>
#include <workflow/nodeflow.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

using namespace agent_framework;

namespace {
Document document(std::string id, std::string content, std::string modality = "text") {
    Document value;
    value.doc_id = std::move(id);
    value.metadata.doc_id = value.doc_id;
    value.metadata.content = std::move(content);
    value.metadata.modality = std::move(modality);
    return value;
}
}

int main() {
    VectorStore store(std::make_unique<InMemoryVectorStoreBackend>(3));
    store.insert(document("a", "alpha"), {1, 0, 0});
    store.insert(document("b", "beta"), {0, 1, 0});
    store.insert(document("c", "gamma", "image"), {1, 0, 0});
    const auto all = store.search({1, 0, 0}, 3);
    assert(all.size() == 3 && all[0].doc_id == "a" && all[1].doc_id == "c");
    const auto text = store.search({1, 0, 0}, 3, "text");
    assert(text.size() == 2 && text[0].doc_id == "a");
    bool invalid = false;
    try { store.insert(document("bad", "bad"), {1, 0}); } catch(const std::invalid_argument&) { invalid = true; }
    assert(invalid);

    const auto persisted = std::filesystem::temp_directory_path() / "agent-framework-vectorstore-wp31.json";
    assert(store.save_index(persisted.string()));
    VectorStore restored(std::make_unique<InMemoryVectorStoreBackend>());
    assert(restored.load_index(persisted.string()));
    const auto restored_text = restored.search({1, 0, 0}, 3, "text");
    assert(restored_text.size() == 2 && restored_text[0].doc_id == "a");
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
    assert(results.size() == 1 && results.front().doc_id == "rag");
    assert(context.find("GNSS radio occultation") != std::string::npos);
    std::cout << "test_vectorstore_wp31: ok\n";
}
