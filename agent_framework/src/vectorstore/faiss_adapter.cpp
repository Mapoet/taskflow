/**
 * @file faiss_adapter.cpp
 * @brief Faiss vector database adapter
 */

#include <agent/vectorstore/vectorstore.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

#ifdef AGENT_HAVE_FAISS
#include <faiss/Index.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#endif

namespace agent_framework {

namespace {
void validate_faiss_embedding(const Embedding& embedding, int dimension) {
    if(dimension <= 0 || static_cast<int>(embedding.size()) != dimension) {
        throw std::invalid_argument("Faiss embedding dimension mismatch");
    }
}

#ifdef AGENT_HAVE_FAISS
RetrievalResult faiss_result(const Document& document, const Embedding& embedding, float score) {
    RetrievalResult result;
    result.doc_id = document.doc_id;
    result.score = score;
    result.embedding = embedding;
    result.content = document.metadata.content;
    result.modality = document.metadata.modality;
    result.description = document.metadata.description;
    result.metadata = document.metadata.extra_metadata;
    return result;
}
#endif
}

FaissBackend::FaissBackend(int dimension, const std::string& index_type)
    : dimension_(dimension), index_type_(index_type), index_(nullptr) {
    if(dimension_ <= 0) throw std::invalid_argument("Faiss vector dimension must be positive");
    create_index();
}

FaissBackend::~FaissBackend() {
#ifdef AGENT_HAVE_FAISS
    delete static_cast<faiss::Index*>(index_);
#endif
}

void FaissBackend::create_index() {
#ifdef AGENT_HAVE_FAISS
    delete static_cast<faiss::Index*>(index_);
    // Inner product over normalized embeddings is cosine similarity.
    index_ = faiss::index_factory(dimension_, index_type_.c_str(), faiss::METRIC_INNER_PRODUCT);
    if(!index_) throw std::runtime_error("failed to create Faiss index: " + index_type_);
#else
    throw std::runtime_error("FaissBackend requires AGENT_BUILD_FAISS=ON");
#endif
}

void FaissBackend::insert(const Document& doc, const Embedding& embedding) {
    if(doc.doc_id.empty()) throw std::invalid_argument("document id must not be empty");
    validate_faiss_embedding(embedding, dimension_);
    std::lock_guard<std::mutex> lock(index_mutex_);
    Document copy = doc;
    copy.doc_id = doc.doc_id;
    copy.embedding = embedding;
    copy.metadata.doc_id = copy.doc_id;
    documents_[copy.doc_id] = std::move(copy);
    embeddings_[doc.doc_id] = embedding;
#ifdef AGENT_HAVE_FAISS
    // Rebuild gives stable document-id mapping and makes update/delete exact.
    create_index();
    std::vector<float> flattened;
    flattened.reserve(embeddings_.size() * static_cast<std::size_t>(dimension_));
    for(const auto& [_, value] : embeddings_) flattened.insert(flattened.end(), value.begin(), value.end());
    if(!flattened.empty()) static_cast<faiss::Index*>(index_)->add(embeddings_.size(), flattened.data());
#endif
}

void FaissBackend::insert_batch(const std::vector<Document>& docs, const std::vector<Embedding>& embeddings) {
    if(docs.size() != embeddings.size()) throw std::invalid_argument("documents/embeddings size mismatch");
    for(std::size_t index = 0; index < docs.size(); ++index) insert(docs[index], embeddings[index]);
}

std::vector<RetrievalResult> FaissBackend::search(const Embedding& query, int top_k, const std::string& modality) {
    if(top_k <= 0) return {};
    validate_faiss_embedding(query, dimension_);
    std::lock_guard<std::mutex> lock(index_mutex_);
    std::vector<RetrievalResult> result;
#ifdef AGENT_HAVE_FAISS
    auto* index = static_cast<faiss::Index*>(index_);
    if(index->ntotal == 0) return result;
    const auto count = std::min<faiss::idx_t>(index->ntotal, static_cast<faiss::idx_t>(std::max(top_k * 4, top_k)));
    std::vector<float> scores(static_cast<std::size_t>(count));
    std::vector<faiss::idx_t> labels(static_cast<std::size_t>(count));
    index->search(1, query.data(), count, scores.data(), labels.data());
    for(std::size_t i = 0; i < labels.size(); ++i) {
        if(labels[i] < 0) continue;
        auto it = embeddings_.begin();
        std::advance(it, labels[i]);
        const auto document = documents_.find(it->first);
        if(document == documents_.end() || (!modality.empty() && document->second.metadata.modality != modality)) continue;
        result.push_back(faiss_result(document->second, it->second, scores[i]));
        if(static_cast<int>(result.size()) == top_k) break;
    }
#else
    (void)modality;
#endif
    return result;
}

bool FaissBackend::delete_document(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(index_mutex_);
    if(!documents_.erase(doc_id)) return false;
    embeddings_.erase(doc_id);
#ifdef AGENT_HAVE_FAISS
    create_index();
    std::vector<float> flattened;
    for(const auto& [_, value] : embeddings_) flattened.insert(flattened.end(), value.begin(), value.end());
    if(!flattened.empty()) static_cast<faiss::Index*>(index_)->add(embeddings_.size(), flattened.data());
#endif
    return true;
}

bool FaissBackend::update_document(const Document& doc, const Embedding& embedding) {
    std::lock_guard<std::mutex> lock(index_mutex_);
    if(!documents_.contains(doc.doc_id)) return false;
    validate_faiss_embedding(embedding, dimension_);
    documents_[doc.doc_id] = doc;
    documents_[doc.doc_id].embedding = embedding;
    documents_[doc.doc_id].metadata.doc_id = doc.doc_id;
    embeddings_[doc.doc_id] = embedding;
#ifdef AGENT_HAVE_FAISS
    create_index();
    std::vector<float> flattened;
    for(const auto& [_, value] : embeddings_) flattened.insert(flattened.end(), value.begin(), value.end());
    if(!flattened.empty()) static_cast<faiss::Index*>(index_)->add(embeddings_.size(), flattened.data());
#endif
    return true;
}

json FaissBackend::get_statistics() const {
    std::lock_guard<std::mutex> lock(index_mutex_);
#ifdef AGENT_HAVE_FAISS
    constexpr bool faiss_available = true;
#else
    constexpr bool faiss_available = false;
#endif
    return {{"backend", "faiss"}, {"available", faiss_available},
            {"dimension", dimension_}, {"index_type", index_type_}, {"documents", documents_.size()}};
}

bool FaissBackend::save_index(const std::string& path) {
#ifdef AGENT_HAVE_FAISS
    std::lock_guard<std::mutex> lock(index_mutex_);
    try { faiss::write_index(static_cast<faiss::Index*>(index_), path.c_str()); return true; }
    catch(const std::exception&) { return false; }
#else
    (void)path; return false;
#endif
}

bool FaissBackend::load_index(const std::string& path) {
#ifdef AGENT_HAVE_FAISS
    std::lock_guard<std::mutex> lock(index_mutex_);
    try {
        std::unique_ptr<faiss::Index> loaded(faiss::read_index(path.c_str()));
        if(!loaded || loaded->d != dimension_ || loaded->ntotal != static_cast<faiss::idx_t>(documents_.size())) return false;
        delete static_cast<faiss::Index*>(index_);
        index_ = loaded.release();
        return true;
    } catch(const std::exception&) { return false; }
#else
    (void)path; return false;
#endif
}

} // namespace agent_framework
