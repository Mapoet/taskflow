/**
 * @file faiss_adapter.cpp
 * @brief Faiss vector database adapter
 */

#include <agent/vectorstore/vectorstore.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef AGENT_HAVE_FAISS
#include <faiss/Index.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#endif

namespace agent_framework {

namespace {
#ifdef AGENT_HAVE_FAISS
bool metadata_matches(const Document& document, const MetadataFilter& filter) {
    for(const auto& [key, expected] : filter) {
        const auto found = document.metadata.extra_metadata.find(key);
        if(found == document.metadata.extra_metadata.end() || found->second != expected) return false;
    }
    return true;
}

class FaissRegistryLock {
public:
    explicit FaissRegistryLock(const std::filesystem::path& path) {
#if !defined(_WIN32)
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if(fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) {
            if(fd_ >= 0) ::close(fd_);
            throw std::runtime_error("cannot lock Faiss index registry");
        }
#else
        (void)path;
#endif
    }
    ~FaissRegistryLock() {
#if !defined(_WIN32)
        if(fd_ >= 0) { (void)::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
    FaissRegistryLock(const FaissRegistryLock&) = delete;
    FaissRegistryLock& operator=(const FaissRegistryLock&) = delete;
private:
    int fd_ = -1;
};
#endif

void validate_faiss_embedding(const Embedding& embedding, int dimension) {
    if(dimension <= 0 || static_cast<int>(embedding.size()) != dimension) {
        throw std::invalid_argument("Faiss embedding dimension mismatch");
    }
    double norm = 0.0;
    for(float value : embedding) {
        if(!std::isfinite(value)) throw std::invalid_argument("Faiss embedding must contain finite values");
        norm += static_cast<double>(value) * value;
    }
    if(norm == 0.0) throw std::invalid_argument("Faiss embedding must not be a zero vector");
}

#ifdef AGENT_HAVE_FAISS
Embedding normalized(const Embedding& value) {
    double norm = 0.0;
    for(float item : value) norm += static_cast<double>(item) * item;
    const float scale = static_cast<float>(1.0 / std::sqrt(norm));
    Embedding output = value;
    for(float& item : output) item *= scale;
    return output;
}

using FaissIndexPtr = std::unique_ptr<faiss::Index>;

FaissIndexPtr build_faiss_index(int dimension, const std::string& index_type,
                                const std::map<std::string, Embedding>& embeddings) {
    FaissIndexPtr index(faiss::index_factory(
        dimension, index_type.c_str(), faiss::METRIC_INNER_PRODUCT));
    if(!index) throw std::runtime_error("failed to create Faiss index: " + index_type);
    std::vector<float> flattened;
    flattened.reserve(embeddings.size() * static_cast<std::size_t>(dimension));
    for(const auto& [_, value] : embeddings) {
        const auto unit = normalized(value);
        flattened.insert(flattened.end(), unit.begin(), unit.end());
    }
    if(!flattened.empty() && !index->is_trained) {
        if(embeddings.size() < 256)
            throw std::runtime_error("Faiss trained index requires at least 256 vectors");
        index->train(static_cast<faiss::idx_t>(embeddings.size()), flattened.data());
    }
    if(!flattened.empty())
        index->add(static_cast<faiss::idx_t>(embeddings.size()), flattened.data());
    return index;
}
#endif

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

json document_json(const Document& doc, const Embedding& embedding) {
    return {{"doc_id", doc.doc_id}, {"embedding", embedding},
            {"modality", doc.metadata.modality}, {"content", doc.metadata.content},
            {"description", doc.metadata.description},
            {"timestamp", static_cast<long long>(doc.metadata.timestamp)},
            {"extra_metadata", doc.metadata.extra_metadata}};
}

bool parse_document(const json& item, int dimension, Document& doc, Embedding& embedding) {
    try {
        doc.doc_id = item.at("doc_id").get<std::string>();
        embedding = item.at("embedding").get<Embedding>();
        validate_faiss_embedding(embedding, dimension);
        if(doc.doc_id.empty()) return false;
        doc.embedding = embedding;
        doc.metadata.doc_id = doc.doc_id;
        doc.metadata.modality = item.value("modality", "");
        doc.metadata.content = item.value("content", "");
        doc.metadata.description = item.value("description", "");
        doc.metadata.timestamp = static_cast<std::time_t>(item.value("timestamp", 0LL));
        doc.metadata.extra_metadata = item.value("extra_metadata", std::map<std::string, json>{});
        return true;
    } catch(const std::exception&) { return false; }
}

std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) return std::nullopt;
    std::ostringstream bytes; bytes << input.rdbuf();
    return bytes.str();
}

std::optional<std::string> file_sha256(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    if(!bytes) return std::nullopt;
    return skill_sha256_bytes(*bytes);
}

bool durable_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    (void)path; return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if(fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd); return ok;
#endif
}

bool durable_dir(const std::filesystem::path& path) {
#if defined(_WIN32)
    (void)path; return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if(fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd); return ok;
#endif
}
#endif
}

FaissBackend::FaissBackend(int dimension, const std::string& index_type,
                           std::string encoder_id, std::string encoder_revision)
    : dimension_(dimension), index_type_(index_type),
      encoder_id_(std::move(encoder_id)), encoder_revision_(std::move(encoder_revision)),
      index_(nullptr) {
    if(dimension_ <= 0) throw std::invalid_argument("Faiss vector dimension must be positive");
    if(encoder_id_.empty() || encoder_revision_.empty())
        throw std::invalid_argument("Faiss encoder identity must not be empty");
    create_index();
}

FaissBackend::~FaissBackend() {
#ifdef AGENT_HAVE_FAISS
    delete static_cast<faiss::Index*>(index_);
#endif
}

void FaissBackend::create_index() {
#ifdef AGENT_HAVE_FAISS
    auto replacement = build_faiss_index(dimension_, index_type_, {});
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
#else
    throw std::runtime_error("FaissBackend requires AGENT_BUILD_FAISS=ON");
#endif
}

void FaissBackend::rebuild_index() {
#ifdef AGENT_HAVE_FAISS
    auto replacement = build_faiss_index(dimension_, index_type_, embeddings_);
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
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
#ifdef AGENT_HAVE_FAISS
    auto documents = documents_;
    auto embeddings = embeddings_;
    documents[copy.doc_id] = std::move(copy);
    embeddings[doc.doc_id] = embedding;
    auto replacement = build_faiss_index(dimension_, index_type_, embeddings);
    documents_.swap(documents);
    embeddings_.swap(embeddings);
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
#else
    documents_[copy.doc_id] = std::move(copy);
    embeddings_[doc.doc_id] = embedding;
#endif
}

void FaissBackend::insert_batch(const std::vector<Document>& docs, const std::vector<Embedding>& embeddings) {
    if(docs.size() != embeddings.size()) throw std::invalid_argument("documents/embeddings size mismatch");
    std::lock_guard<std::mutex> lock(index_mutex_);
    auto next_documents = documents_;
    auto next_embeddings = embeddings_;
    for(std::size_t index = 0; index < docs.size(); ++index) {
        if(docs[index].doc_id.empty()) throw std::invalid_argument("document id must not be empty");
        validate_faiss_embedding(embeddings[index], dimension_);
        auto copy = docs[index];
        copy.embedding = embeddings[index];
        copy.metadata.doc_id = copy.doc_id;
        next_documents[copy.doc_id] = std::move(copy);
        next_embeddings[docs[index].doc_id] = embeddings[index];
    }
#ifdef AGENT_HAVE_FAISS
    auto replacement = build_faiss_index(dimension_, index_type_, next_embeddings);
    documents_.swap(next_documents);
    embeddings_.swap(next_embeddings);
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
#else
    documents_.swap(next_documents);
    embeddings_.swap(next_embeddings);
#endif
}

std::vector<RetrievalResult> FaissBackend::search(const Embedding& query, int top_k,
                                                   const std::string& modality,
                                                   const MetadataFilter& metadata_filter) {
    if(top_k <= 0) return {};
    validate_faiss_embedding(query, dimension_);
    std::lock_guard<std::mutex> lock(index_mutex_);
    std::vector<RetrievalResult> result;
#ifdef AGENT_HAVE_FAISS
    auto* index = static_cast<faiss::Index*>(index_);
    if(index->ntotal == 0) return result;
    const auto count = index->ntotal;
    std::vector<float> scores(static_cast<std::size_t>(count));
    std::vector<faiss::idx_t> labels(static_cast<std::size_t>(count));
    const auto unit_query = normalized(query);
    index->search(1, unit_query.data(), count, scores.data(), labels.data());
    for(std::size_t i = 0; i < labels.size(); ++i) {
        if(labels[i] < 0) continue;
        auto it = embeddings_.begin();
        std::advance(it, labels[i]);
        const auto document = documents_.find(it->first);
        if(document == documents_.end() ||
           (!modality.empty() && document->second.metadata.modality != modality) ||
           !metadata_matches(document->second, metadata_filter)) continue;
        result.push_back(faiss_result(document->second, it->second, scores[i]));
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score != rhs.score ? lhs.score > rhs.score : lhs.doc_id < rhs.doc_id;
    });
    if(result.size() > static_cast<std::size_t>(top_k)) result.resize(static_cast<std::size_t>(top_k));
#else
    (void)modality;
    (void)metadata_filter;
#endif
    return result;
}

bool FaissBackend::delete_document(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(index_mutex_);
    if(!documents_.contains(doc_id)) return false;
    auto documents = documents_;
    auto embeddings = embeddings_;
    documents.erase(doc_id);
    embeddings.erase(doc_id);
#ifdef AGENT_HAVE_FAISS
    auto replacement = build_faiss_index(dimension_, index_type_, embeddings);
    documents_.swap(documents);
    embeddings_.swap(embeddings);
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
#else
    documents_.swap(documents);
    embeddings_.swap(embeddings);
#endif
    return true;
}

bool FaissBackend::update_document(const Document& doc, const Embedding& embedding) {
    std::lock_guard<std::mutex> lock(index_mutex_);
    if(!documents_.contains(doc.doc_id)) return false;
    validate_faiss_embedding(embedding, dimension_);
    auto documents = documents_;
    auto embeddings = embeddings_;
    documents[doc.doc_id] = doc;
    documents[doc.doc_id].embedding = embedding;
    documents[doc.doc_id].metadata.doc_id = doc.doc_id;
    embeddings[doc.doc_id] = embedding;
#ifdef AGENT_HAVE_FAISS
    auto replacement = build_faiss_index(dimension_, index_type_, embeddings);
    documents_.swap(documents);
    embeddings_.swap(embeddings);
    delete static_cast<faiss::Index*>(index_);
    index_ = replacement.release();
#else
    documents_.swap(documents);
    embeddings_.swap(embeddings);
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
    namespace fs = std::filesystem;
    try {
        const fs::path root(path), generations=root/"generations";
        fs::create_directories(generations);
        FaissRegistryLock registry_lock(root/"lock");
        std::uint64_t revision = 1;
        for(const auto& entry : fs::directory_iterator(generations)) {
            if(!entry.is_directory()) continue;
            const auto candidate = entry.path().filename().string();
            if(candidate.empty() || candidate.find_first_not_of("0123456789") != std::string::npos)
                continue;
            try {
                revision = std::max(revision,
                                    static_cast<std::uint64_t>(std::stoull(candidate)) + 1);
            }
            catch(...) {}
        }
        const std::string name=std::to_string(revision);
#if defined(_WIN32)
        const int process_id = 0;
#else
        const int process_id = ::getpid();
#endif
        const fs::path temporary=generations/(".tmp-"+std::to_string(process_id)+"-"+name), generation=generations/name;
        std::error_code cleanup_error;
        fs::remove_all(temporary, cleanup_error);
        fs::create_directories(temporary);
        const fs::path index_path=temporary/"index.faiss", records_path=temporary/"records.jsonl";
        faiss::write_index(static_cast<faiss::Index*>(index_), index_path.c_str());
        std::ofstream records(records_path, std::ios::binary);
        if(!records) return false;
        for(const auto& [id, doc] : documents_) records << document_json(doc, embeddings_.at(id)).dump() << '\n';
        records.close();
        const auto index_digest=file_sha256(index_path), records_digest=file_sha256(records_path);
        if(!index_digest||!records_digest) return false;
        json manifest={{"schema_version",1},{"revision",revision},{"dimension",dimension_},
            {"metric","cosine"},{"index_type",index_type_},{"record_count",documents_.size()},
            {"encoder_id",encoder_id_},{"encoder_revision",encoder_revision_},
            {"index_sha256",*index_digest},{"records_sha256",*records_digest}};
        const fs::path manifest_path=temporary/"manifest.json";
        std::ofstream manifest_out(manifest_path,std::ios::binary); manifest_out<<manifest.dump(2)<<'\n'; manifest_out.close();
        if(!durable_file(index_path)||!durable_file(records_path)||!durable_file(manifest_path)||!durable_dir(temporary)) return false;
        fs::rename(temporary,generation); durable_dir(generations);
        const fs::path current_tmp=root/("CURRENT.tmp-"+std::to_string(process_id)), current=root/"CURRENT";
        std::ofstream pointer(current_tmp,std::ios::binary); pointer<<name<<'\n'; pointer.close();
        if(!durable_file(current_tmp)) return false;
        fs::rename(current_tmp,current);
        if(!durable_dir(root)) return false;
        std::vector<std::string> committed_generations;
        for(const auto& entry : fs::directory_iterator(generations)) {
            const auto candidate = entry.path().filename().string();
            if(entry.is_directory() && !candidate.empty() &&
               candidate.find_first_not_of("0123456789") == std::string::npos)
                committed_generations.push_back(candidate);
            else if(entry.is_directory() && candidate.rfind(".tmp-", 0) == 0)
                fs::remove_all(entry.path(), cleanup_error);
        }
        std::sort(committed_generations.begin(), committed_generations.end(),
                  [](const auto& lhs, const auto& rhs) { return std::stoull(lhs) > std::stoull(rhs); });
        for(std::size_t index=3; index<committed_generations.size(); ++index)
            fs::remove_all(generations/committed_generations[index], cleanup_error);
        return durable_dir(generations);
    } catch(const std::exception&) { return false; }
#else
    (void)path; return false;
#endif
}

bool FaissBackend::load_index(const std::string& path) {
#ifdef AGENT_HAVE_FAISS
    std::lock_guard<std::mutex> lock(index_mutex_);
    try {
        namespace fs=std::filesystem;
        const fs::path root(path), generations = root / "generations";
        std::vector<std::string> candidates;
        std::ifstream pointer(root / "CURRENT");
        std::string current;
        if(std::getline(pointer, current) && !current.empty() &&
           current.find_first_not_of("0123456789") == std::string::npos)
            candidates.push_back(current);
        if(fs::exists(generations)) {
            std::vector<std::string> history;
            for(const auto& entry : fs::directory_iterator(generations)) {
                const auto name = entry.path().filename().string();
                if(entry.is_directory() && !name.empty() &&
                   name.find_first_not_of("0123456789") == std::string::npos && name != current)
                    history.push_back(name);
            }
            std::sort(history.begin(), history.end(), std::greater<>());
            candidates.insert(candidates.end(), history.begin(), history.end());
        }
        for(const auto& generation_name : candidates) {
            try {
                const fs::path generation = generations / generation_name;
                json manifest; std::ifstream manifest_in(generation / "manifest.json");
                manifest_in >> manifest;
                if(manifest.value("schema_version",0)!=1 ||
                   manifest.value("revision",std::uint64_t{0}) != std::stoull(generation_name) ||
                   manifest.value("dimension",0)!=dimension_ ||
                   manifest.value("metric","")!="cosine" || manifest.value("index_type","")!=index_type_ ||
                   manifest.value("encoder_id","")!=encoder_id_ ||
                   manifest.value("encoder_revision","")!=encoder_revision_) continue;
                const auto records_digest = file_sha256(generation / "records.jsonl");
                if(!records_digest || *records_digest != manifest.value("records_sha256","")) continue;
                std::map<std::string,Document> documents;
                std::map<std::string,Embedding> embeddings;
                std::ifstream records(generation / "records.jsonl");
                bool valid = static_cast<bool>(records);
                for(std::string line; valid && std::getline(records,line);) {
                    if(line.empty()) continue;
                    Document doc; Embedding embedding;
                    if(!parse_document(json::parse(line),dimension_,doc,embedding) ||
                       !documents.emplace(doc.doc_id,doc).second) valid = false;
                    else embeddings.emplace(doc.doc_id,std::move(embedding));
                }
                if(!valid || documents.size()!=manifest.value("record_count",std::size_t(-1))) continue;
                FaissIndexPtr loaded;
                const auto index_digest = file_sha256(generation / "index.faiss");
                if(index_digest && *index_digest == manifest.value("index_sha256","")) {
                    loaded.reset(faiss::read_index((generation / "index.faiss").c_str()));
                    if(!loaded || loaded->d != dimension_ ||
                       loaded->ntotal != static_cast<faiss::idx_t>(documents.size())) loaded.reset();
                }
                if(!loaded) loaded = build_faiss_index(dimension_, index_type_, embeddings);
                documents_.swap(documents);
                embeddings_.swap(embeddings);
                delete static_cast<faiss::Index*>(index_);
                index_ = loaded.release();
                return true;
            } catch(const std::exception&) {
                // Try the previous committed generation.
            }
        }
        return false;
    } catch(const std::exception&) { return false; }
#else
    (void)path; return false;
#endif
}

} // namespace agent_framework
