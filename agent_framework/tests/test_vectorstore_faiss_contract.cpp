#include <agent/vectorstore/vectorstore.hpp>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
using namespace agent_framework;
namespace {
void require(bool condition, const char* message) {
  if(!condition) throw std::runtime_error(message);
}
Document doc(std::string id, std::string modality="text") { Document d; d.doc_id=std::move(id); d.metadata.doc_id=d.doc_id; d.metadata.content=d.doc_id+" content"; d.metadata.modality=std::move(modality); d.metadata.extra_metadata["scope"] = d.doc_id == "a" ? "primary" : "secondary"; return d; }
std::vector<RetrievalResult> exercise(VectorStoreBackend& backend) {
  backend.insert(doc("a"), {2,0,0}); backend.insert(doc("b"), {0,3,0}); backend.insert(doc("c","image"), {4,0,0});
  backend.insert(doc("b"), {1,0,0});
  auto result=backend.search({9,0,0},3,"text"); require(result.size()==2 && result[0].doc_id=="a" && result[1].doc_id=="b", "upsert/order contract failed");
  const auto filtered=backend.search({9,0,0},3,"text",{{"scope","primary"}}); require(filtered.size()==1&&filtered[0].doc_id=="a", "metadata filter contract failed");
  require(backend.delete_document("b") && !backend.delete_document("b"), "delete contract failed");
  result=backend.search({1,0,0},3,"text"); require(result.size()==1 && result[0].doc_id=="a" && std::fabs(result[0].score-1.0f)<1e-5f, "cosine/filter contract failed"); return result;
}
}
int main(){
  InMemoryVectorStoreBackend memory(3); const auto expected=exercise(memory);
  FaissBackend faiss(3,"Flat"); const auto actual=exercise(faiss);
  require(actual.size()==expected.size()&&actual[0].doc_id==expected[0].doc_id&&std::fabs(actual[0].score-expected[0].score)<1e-5f, "backend parity failed");
  const auto root=std::filesystem::temp_directory_path()/"agent-faiss-contract"; std::filesystem::remove_all(root);
  require(faiss.save_index(root.string()), "first generation save failed"); std::ifstream current(root/"CURRENT"); std::string generation; std::getline(current,generation);
  std::ifstream manifest_in(root/"generations"/generation/"manifest.json"); json manifest; manifest_in>>manifest;
  require(manifest.at("encoder_id")=="external"&&manifest.at("encoder_revision")=="unspecified", "encoder identity missing");
  std::ofstream corrupt(root/"generations"/generation/"index.faiss",std::ios::trunc); corrupt<<"corrupt"; corrupt.close();
  FaissBackend restored(3,"Flat"); require(restored.load_index(root.string()), "records rebuild failed");
  const auto recovered=restored.search({1,0,0},3,"text"); require(recovered.size()==1&&recovered[0].doc_id=="a", "rebuilt index mismatch");

  // A corrupt current generation must fall back to the preceding committed one.
  require(faiss.save_index(root.string()), "second generation save failed"); current=std::ifstream(root/"CURRENT"); std::string newest; std::getline(current,newest);
  require(newest!=generation, "generation revision did not advance");
  std::ofstream corrupt_records(root/"generations"/newest/"records.jsonl",std::ios::app);
  corrupt_records<<"corrupt\n"; corrupt_records.close();
  FaissBackend fallback(3,"Flat"); require(fallback.load_index(root.string()), "generation fallback failed");
  require(fallback.search({1,0,0},3,"text").front().doc_id=="a", "fallback data mismatch");

  FaissBackend wrong_encoder(3,"Flat","different-encoder","v1");
  require(!wrong_encoder.load_index(root.string()), "encoder mismatch was accepted");
  std::filesystem::remove_all(root);
}
