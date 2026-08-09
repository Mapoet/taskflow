/**
 * @file vectorstore.cpp
 * @brief VectorStore implementation
 */

#include <agent/vectorstore/vectorstore.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace agent_framework {

namespace {
bool metadata_matches(const Document& document, const MetadataFilter& filter) {
    for(const auto& [key, expected] : filter) {
        const auto found = document.metadata.extra_metadata.find(key);
        if(found == document.metadata.extra_metadata.end() || found->second != expected) return false;
    }
    return true;
}

float cosine_similarity(const Embedding& lhs, const Embedding& rhs) {
    if(lhs.size() != rhs.size() || lhs.empty()) throw std::invalid_argument("embedding dimension mismatch");
    double dot = 0.0, left_norm = 0.0, right_norm = 0.0;
    for(std::size_t i = 0; i < lhs.size(); ++i) {
        dot += static_cast<double>(lhs[i]) * rhs[i];
        left_norm += static_cast<double>(lhs[i]) * lhs[i];
        right_norm += static_cast<double>(rhs[i]) * rhs[i];
    }
    if(left_norm == 0.0 || right_norm == 0.0) return 0.0f;
    return static_cast<float>(dot / std::sqrt(left_norm * right_norm));
}

RetrievalResult to_result(const Document& doc, const Embedding& embedding, float score) {
    RetrievalResult result;
    result.doc_id = doc.doc_id;
    result.score = score;
    result.embedding = embedding;
    result.content = doc.metadata.content;
    result.modality = doc.metadata.modality;
    result.description = doc.metadata.description;
    result.metadata = doc.metadata.extra_metadata;
    return result;
}
}

InMemoryVectorStoreBackend::InMemoryVectorStoreBackend(int dimension) : dimension_(dimension) {
    if(dimension < 0) throw std::invalid_argument("vector dimension must be non-negative");
}

void InMemoryVectorStoreBackend::validate_embedding(const Embedding& embedding) const {
    if(embedding.empty()) throw std::invalid_argument("embedding must not be empty");
    if(dimension_ != 0 && static_cast<int>(embedding.size()) != dimension_)
        throw std::invalid_argument("embedding dimension mismatch");
    double norm = 0.0;
    for(float value : embedding) {
        if(!std::isfinite(value)) throw std::invalid_argument("embedding must contain finite values");
        norm += static_cast<double>(value) * value;
    }
    if(norm == 0.0) throw std::invalid_argument("embedding must not be a zero vector");
}

void InMemoryVectorStoreBackend::insert(const Document& doc, const Embedding& embedding) {
    if(doc.doc_id.empty()) throw std::invalid_argument("document id must not be empty");
    validate_embedding(embedding);
    std::lock_guard<std::mutex> lock(mutex_);
    if(dimension_ == 0) dimension_ = static_cast<int>(embedding.size());
    Document copy = doc;
    copy.embedding = embedding;
    copy.metadata.doc_id = copy.doc_id;
    documents_[copy.doc_id] = std::move(copy);
}

void InMemoryVectorStoreBackend::insert_batch(const std::vector<Document>& docs,
                                              const std::vector<Embedding>& embeddings) {
    if(docs.size() != embeddings.size()) throw std::invalid_argument("documents/embeddings size mismatch");
    for(std::size_t i = 0; i < docs.size(); ++i) insert(docs[i], embeddings[i]);
}

std::vector<RetrievalResult> InMemoryVectorStoreBackend::search(
    const Embedding& query, int top_k, const std::string& modality,
    const MetadataFilter& metadata_filter) {
    if(top_k <= 0) return {};
    validate_embedding(query);
    std::vector<RetrievalResult> results;
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& [id, doc] : documents_) {
        (void)id;
        if((!modality.empty() && doc.metadata.modality != modality) ||
           !metadata_matches(doc, metadata_filter)) continue;
        results.push_back(to_result(doc, doc.embedding, cosine_similarity(query, doc.embedding)));
    }
    std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score != rhs.score ? lhs.score > rhs.score : lhs.doc_id < rhs.doc_id;
    });
    if(results.size() > static_cast<std::size_t>(top_k)) results.resize(static_cast<std::size_t>(top_k));
    return results;
}

bool InMemoryVectorStoreBackend::delete_document(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return documents_.erase(doc_id) != 0;
}

bool InMemoryVectorStoreBackend::update_document(const Document& doc, const Embedding& embedding) {
    if(doc.doc_id.empty()) return false;
    validate_embedding(embedding);
    std::lock_guard<std::mutex> lock(mutex_);
    if(documents_.find(doc.doc_id) == documents_.end()) return false;
    Document copy = doc;
    copy.embedding = embedding;
    copy.metadata.doc_id = copy.doc_id;
    documents_[copy.doc_id] = std::move(copy);
    return true;
}

json InMemoryVectorStoreBackend::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"backend", "memory"}, {"dimension", dimension_}, {"documents", documents_.size()}};
}

bool InMemoryVectorStoreBackend::save_index(const std::string& path) {
    if(path.empty()) return false;
    json payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        payload["format"] = "agent_framework.memory_vectorstore.v1";
        payload["dimension"] = dimension_;
        payload["documents"] = json::array();
        for(const auto& [doc_id, doc] : documents_) {
            payload["documents"].push_back({
                {"doc_id", doc_id}, {"embedding", doc.embedding},
                {"modality", doc.metadata.modality}, {"content", doc.metadata.content},
                {"description", doc.metadata.description},
                {"timestamp", static_cast<long long>(doc.metadata.timestamp)},
                {"extra_metadata", doc.metadata.extra_metadata}
            });
        }
    }
    const std::filesystem::path destination(path);
    const std::filesystem::path temporary = destination.string() + ".tmp";
    try {
        std::ofstream output(temporary);
        if(!output) return false;
        output << payload.dump(2) << '\n';
        output.close();
        if(!output) return false;
        std::filesystem::rename(temporary, destination);
        return true;
    } catch(const std::exception&) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

bool InMemoryVectorStoreBackend::load_index(const std::string& path) {
    try {
        std::ifstream input(path);
        if(!input) return false;
        json payload;
        input >> payload;
        if(payload.value("format", "") != "agent_framework.memory_vectorstore.v1") return false;
        const int loaded_dimension = payload.at("dimension").get<int>();
        if(loaded_dimension < 0) return false;
        std::map<std::string, Document> loaded;
        for(const auto& item : payload.at("documents")) {
            Document doc;
            doc.doc_id = item.at("doc_id").get<std::string>();
            doc.embedding = item.at("embedding").get<Embedding>();
            if(doc.doc_id.empty() || static_cast<int>(doc.embedding.size()) != loaded_dimension) return false;
            doc.metadata.doc_id = doc.doc_id;
            doc.metadata.modality = item.value("modality", "");
            doc.metadata.content = item.value("content", "");
            doc.metadata.description = item.value("description", "");
            doc.metadata.timestamp = static_cast<std::time_t>(item.value("timestamp", 0LL));
            doc.metadata.extra_metadata = item.value("extra_metadata", std::map<std::string, json>{});
            if(!loaded.emplace(doc.doc_id, std::move(doc)).second) return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        dimension_ = loaded_dimension;
        documents_ = std::move(loaded);
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

VectorStore::VectorStore(std::unique_ptr<VectorStoreBackend> backend) : backend_(std::move(backend)) {
    if(!backend_) throw std::invalid_argument("VectorStore requires a backend");
}
void VectorStore::insert(const Document& doc, const Embedding& embedding) {
    std::lock_guard<std::mutex> lock(backend_mutex_); backend_->insert(doc, embedding);
}
std::vector<RetrievalResult> VectorStore::search(const Embedding& query, int top_k,
                                                  const std::string& modality,
                                                  const MetadataFilter& metadata_filter) {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    return backend_->search(query, top_k, modality, metadata_filter);
}
std::vector<RetrievalResult> VectorStore::hybrid_search(const std::string&, const Embedding& query, int top_k) {
    return search(query, top_k);
}
bool VectorStore::delete_document(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(backend_mutex_); return backend_->delete_document(doc_id);
}
bool VectorStore::update_document(const Document& doc, const Embedding& embedding) {
    std::lock_guard<std::mutex> lock(backend_mutex_); return backend_->update_document(doc, embedding);
}
bool VectorStore::save_index(const std::string& path) {
    std::lock_guard<std::mutex> lock(backend_mutex_); return backend_->save_index(path);
}
bool VectorStore::load_index(const std::string& path) {
    std::lock_guard<std::mutex> lock(backend_mutex_); return backend_->load_index(path);
}
json VectorStore::get_statistics() const {
    std::lock_guard<std::mutex> lock(backend_mutex_); return backend_->get_statistics();
}
void VectorStore::register_encoder(const std::string& modality, std::shared_ptr<Encoder> encoder) {
    if(!encoder) throw std::invalid_argument("encoder must not be null");
    std::lock_guard<std::mutex> lock(encoders_mutex_); encoders_[modality] = std::move(encoder);
}
std::shared_ptr<Encoder> VectorStore::get_encoder(const std::string& modality) const {
    std::lock_guard<std::mutex> lock(encoders_mutex_);
    const auto it = encoders_.find(modality); return it == encoders_.end() ? nullptr : it->second;
}
void VectorStore::switch_backend(std::unique_ptr<VectorStoreBackend> backend) {
    if(!backend) throw std::invalid_argument("backend must not be null");
    std::lock_guard<std::mutex> lock(backend_mutex_); backend_ = std::move(backend);
}

} // namespace agent_framework
