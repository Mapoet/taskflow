/**
 * @file encoder.cpp
 * @brief 编码器实现
 */

#include <agent/encoder/encoder.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace agent_framework {

void Encoder::normalize_vector(Embedding& value) {
    double norm = 0.0;
    for(const auto item : value) norm += static_cast<double>(item) * item;
    if(norm == 0.0) return;
    const float scale = static_cast<float>(1.0 / std::sqrt(norm));
    for(auto& item : value) item *= scale;
}

TextEncoder::TextEncoder(const std::string& model_path) : model_path_(model_path) {}
Embedding TextEncoder::inference(const std::string& text) {
    Embedding result(static_cast<std::size_t>(dimension_), 0.0f);
    // A deterministic hashing encoder is an offline baseline, not a semantic model.
    for(const auto token : text) result[std::hash<unsigned char>{}(static_cast<unsigned char>(token)) % result.size()] += 1.0f;
    normalize_vector(result);
    return result;
}
Embedding TextEncoder::encode(const std::string& input) {
    if(!validate_input(input)) throw std::invalid_argument("text input must not be empty");
    return inference(input);
}
int TextEncoder::get_dimension() const { return dimension_; }
ModalityType TextEncoder::get_modality_type() const { return ModalityType::TEXT; }
std::vector<Embedding> TextEncoder::encode_batch(const std::vector<std::string>& inputs) {
    std::vector<Embedding> result; result.reserve(inputs.size());
    for(const auto& input : inputs) result.push_back(encode(input));
    return result;
}
bool TextEncoder::validate_input(const std::string& input) const { return !input.empty(); }
void TextEncoder::load_model() {}

void EncoderManager::register_encoder(const std::string& modality, std::shared_ptr<Encoder> encoder) {
    if(modality.empty() || !encoder) throw std::invalid_argument("encoder registration requires modality and encoder");
    std::lock_guard<std::mutex> lock(encoders_mutex_); encoders_[modality] = std::move(encoder);
}
std::shared_ptr<Encoder> EncoderManager::get_encoder(const std::string& modality) const {
    std::lock_guard<std::mutex> lock(encoders_mutex_);
    const auto it = encoders_.find(modality); return it == encoders_.end() ? nullptr : it->second;
}
std::shared_ptr<Encoder> EncoderManager::select_encoder(const std::string&) const { return get_encoder("text"); }
std::vector<std::string> EncoderManager::list_encoders() const {
    std::lock_guard<std::mutex> lock(encoders_mutex_); std::vector<std::string> result;
    for(const auto& [name, _] : encoders_) { (void)_; result.push_back(name); }
    return result;
}
Embedding EncoderManager::encode_auto(const std::string& input, const std::string& modality_hint) {
    const auto encoder = get_encoder(modality_hint.empty() ? "text" : modality_hint);
    if(!encoder) throw std::runtime_error("no encoder registered for requested modality");
    return encoder->encode(input);
}
ModalityType EncoderManager::detect_input_type(const std::string&) const { return ModalityType::TEXT; }

} // namespace agent_framework
