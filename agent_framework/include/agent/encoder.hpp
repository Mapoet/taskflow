/**
 * @file encoder.hpp
 * @brief Encoder 模块：多模态编码器接口
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_ENCODER_H__
#define __AGENT_ENCODER_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace agent_framework {

// ============================================================================
// 编码器接口
// ============================================================================

/**
 * @brief 编码器虚基类
 * 定义统一的多模态编码接口
 */
class Encoder {
public:
    virtual ~Encoder() = default;
    
    /**
     * @brief 编码数据为向量
     * @param input 输入数据（文本、图像路径、音频路径等）
     * @return 向量嵌入
     */
    virtual Embedding encode(const std::string& input) = 0;
    
    /**
     * @brief 获取编码器维度
     * @return 向量维度
     */
    virtual int get_dimension() const = 0;
    
    /**
     * @brief 获取支持的模态类型
     * @return 模态类型枚举值
     */
    virtual ModalityType get_modality_type() const = 0;
    
    /**
     * @brief 批量编码
     * @param inputs 输入数据列表
     * @return 向量嵌入列表
     */
    virtual std::vector<Embedding> encode_batch(const std::vector<std::string>& inputs) = 0;
    
    /**
     * @brief 检查输入是否有效
     * @param input 输入数据
     * @return true 如果输入有效
     */
    virtual bool validate_input(const std::string& input) const = 0;
    
protected:
    /**
     * @brief 归一化向量（可由派生类调用）
     * @param embedding 向量（将被修改）
     */
    void normalize_vector(Embedding& embedding);
};

/**
 * @brief 文本编码器（使用 BERT/Sentence Transformers）
 */
class TextEncoder : public Encoder {
public:
    explicit TextEncoder(const std::string& model_path = "");
    
    Embedding encode(const std::string& input) override;
    int get_dimension() const override;
    ModalityType get_modality_type() const override;
    std::vector<Embedding> encode_batch(const std::vector<std::string>& inputs) override;
    bool validate_input(const std::string& input) const override;
    
private:
    std::string model_path_;
    int dimension_ = 384;  // 默认维度（Sentence Transformers）
    void* model_;  // 模型指针（实际类型取决于模型库）
    
    /**
     * @brief 加载模型
     */
    void load_model();
    
    /**
     * @brief 执行推理
     * @param text 文本
     * @return 向量嵌入
     */
    Embedding inference(const std::string& text);
};

/**
 * @brief 图像编码器（使用 CLIP/BLIP-2）
 */
class ImageEncoder : public Encoder {
public:
    explicit ImageEncoder(const std::string& model_path = "");
    
    Embedding encode(const std::string& input) override;  // input 为图像路径或 base64
    int get_dimension() const override;
    ModalityType get_modality_type() const override;
    std::vector<Embedding> encode_batch(const std::vector<std::string>& inputs) override;
    bool validate_input(const std::string& input) const override;
    
    /**
     * @brief 生成图像描述（用于文本检索）
     * @param image_path 图像路径
     * @return 图像描述文本
     */
    std::string generate_caption(const std::string& image_path);
    
private:
    std::string model_path_;
    int dimension_ = 512;  // CLIP 默认维度
    void* model_;
    
    /**
     * @brief 解码 base64 图像
     * @param base64_data base64 编码的图像数据
     * @return 图像矩阵（cv::Mat 或类似结构）
     */
    void* decode_base64_image(const std::string& base64_data);
    
    /**
     * @brief 执行图像编码
     * @param image 图像矩阵
     * @return 向量嵌入
     */
    Embedding inference_image(void* image);
};

/**
 * @brief 音频编码器（使用 Whisper）
 */
class AudioEncoder : public Encoder {
public:
    explicit AudioEncoder(const std::string& model_path = "");
    
    Embedding encode(const std::string& input) override;  // input 为音频路径或 base64
    int get_dimension() const override;
    ModalityType get_modality_type() const override;
    std::vector<Embedding> encode_batch(const std::vector<std::string>& inputs) override;
    bool validate_input(const std::string& input) const override;
    
    /**
     * @brief 音频转文字（用于文本检索）
     * @param audio_path 音频路径
     * @return 转录文本
     */
    std::string transcribe(const std::string& audio_path);
    
private:
    std::string model_path_;
    int dimension_ = 512;
    void* model_;
    
    /**
     * @brief 解码 base64 音频
     * @param base64_data base64 编码的音频数据
     * @return 音频采样数组
     */
    std::vector<float> decode_base64_audio(const std::string& base64_data);
    
    /**
     * @brief 执行音频编码
     * @param audio_samples 音频采样
     * @return 向量嵌入
     */
    Embedding inference_audio(const std::vector<float>& audio_samples);
};

/**
 * @brief 视频编码器（使用 ViViT 或其他视频模型）
 */
class VideoEncoder : public Encoder {
public:
    explicit VideoEncoder(const std::string& model_path = "");
    
    Embedding encode(const std::string& input) override;  // input 为视频路径
    int get_dimension() const override;
    ModalityType get_modality_type() const override;
    std::vector<Embedding> encode_batch(const std::vector<std::string>& inputs) override;
    bool validate_input(const std::string& input) const override;
    
private:
    std::string model_path_;
    int dimension_ = 768;
    void* model_;
    
    /**
     * @brief 提取视频帧
     * @param video_path 视频路径
     * @return 视频帧列表
     */
    std::vector<void*> extract_frames(const std::string& video_path);
    
    /**
     * @brief 执行视频编码
     * @param frames 视频帧列表
     * @return 向量嵌入
     */
    Embedding inference_video(const std::vector<void*>& frames);
};

// ============================================================================
// 编码器管理器
// ============================================================================

/**
 * @brief 编码器管理器
 */
class EncoderManager {
public:
    /**
     * @brief 注册编码器
     * @param modality 模态类型（"text", "image", "audio", "video"）
     * @param encoder 编码器
     */
    void register_encoder(const std::string& modality, std::shared_ptr<Encoder> encoder);
    
    /**
     * @brief 获取编码器
     * @param modality 模态类型
     * @return 编码器（如果存在）
     */
    std::shared_ptr<Encoder> get_encoder(const std::string& modality) const;
    
    /**
     * @brief 根据输入类型自动选择编码器
     * @param input 输入数据
     * @return 编码器（如果找到）
     */
    std::shared_ptr<Encoder> select_encoder(const std::string& input) const;
    
    /**
     * @brief 列出所有已注册的编码器
     * @return 模态类型列表
     */
    std::vector<std::string> list_encoders() const;
    
    /**
     * @brief 编码数据（自动选择编码器）
     * @param input 输入数据
     * @param modality_hint 模态提示（可选）
     * @return 向量嵌入
     */
    Embedding encode_auto(const std::string& input, const std::string& modality_hint = "");
    
private:
    std::map<std::string, std::shared_ptr<Encoder>> encoders_;
    std::mutex encoders_mutex_;
    
    /**
     * @brief 检测输入类型
     * @param input 输入数据
     * @return 模态类型枚举值
     */
    ModalityType detect_input_type(const std::string& input) const;
};

} // namespace agent_framework

#endif // __AGENT_ENCODER_H__

