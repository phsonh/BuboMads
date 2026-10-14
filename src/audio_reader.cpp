#include <sndfile.h>
#include <samplerate.h>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// 后续的 DSP 算法只需要认识这个类型。
struct Audio {
    int sampleRate = 48000;
    std::vector<float> samples;  // 单声道、连续排列的 float32 PCM 采样
};

Audio loadAudio(const std::string& path, int targetRate = 48000) {
    if (targetRate <= 0) {
        throw std::invalid_argument("targetRate must be positive");
    }

    SF_INFO info{};
    SNDFILE* raw = sf_open(path.c_str(), SFM_READ, &info);
    if (!raw) {
        throw std::runtime_error("Cannot open audio: " + path + " (" + sf_strerror(nullptr) + ")");
    }

    // RAII：异常或正常返回时自动关闭文件
    const auto closeFile = [](SNDFILE* f) { if (f) sf_close(f); };
    std::unique_ptr<SNDFILE, decltype(closeFile)> file(raw, closeFile);

    if (info.channels <= 0 || info.samplerate <= 0) {
        throw std::runtime_error("Invalid audio channel count or sample rate");
    }

    // 阶段 1：文件解码成 float PCM，然后把多声道合并为单声道。
    // 按帧读取：一帧包含每个声道各一个采样点。
    constexpr sf_count_t kChunkFrames = 4096;
    std::vector<float> chunk(static_cast<std::size_t>(kChunkFrames) * info.channels);
    std::vector<float> mono;

    while (true) {
        const sf_count_t frames = sf_readf_float(file.get(), chunk.data(), kChunkFrames);
        if (frames == 0) break;

        for (sf_count_t i = 0; i < frames; ++i) {
            double sum = 0.0;
            const std::size_t offset = static_cast<std::size_t>(i) * info.channels;
            for (int channel = 0; channel < info.channels; ++channel) {
                sum += chunk[offset + static_cast<std::size_t>(channel)];
            }
            mono.push_back(static_cast<float>(sum / info.channels));
        }
    }

    if (sf_error(file.get()) != SF_ERR_NO_ERROR) {
        throw std::runtime_error(std::string("Audio decode error: ") + sf_strerror(file.get()));
    }

    if (mono.empty() || info.samplerate == targetRate) {
        return Audio{targetRate, std::move(mono)};
    }

    // 阶段 2：把原采样率转换为指定采样率（例如 44100 -> 48000）。
    const double ratio = static_cast<double>(targetRate) / info.samplerate;
    const double capacity = std::ceil(static_cast<double>(mono.size()) * ratio) + 256.0;
    if (mono.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()) ||
        !std::isfinite(capacity) ||
        capacity > static_cast<double>(std::numeric_limits<long>::max())) {
        throw std::runtime_error("Audio too large for this simple in-memory resampler");
    }

    std::vector<float> resampled(static_cast<std::size_t>(capacity));
    SRC_DATA data{};
    data.data_in = mono.data();
    data.input_frames = static_cast<long>(mono.size());
    data.data_out = resampled.data();
    data.output_frames = static_cast<long>(resampled.size());
    data.src_ratio = ratio;

    const int error = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 1);
    if (error != 0) {
        throw std::runtime_error(std::string("Resampling failed: ") + src_strerror(error));
    }
    if (data.input_frames_used != data.input_frames) {
        throw std::runtime_error("Resampling output buffer was too small");
    }

    resampled.resize(static_cast<std::size_t>(data.output_frames_gen));
    return Audio{targetRate, std::move(resampled)};
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <audio.wav|audio.mp3|audio.ogg>\n";
        return 1;
    }

    try {
        const Audio audio = loadAudio(argv[1]);
        std::cout << "sampleRate: " << audio.sampleRate << " Hz\n";
        std::cout << "channels:   1\n";
        std::cout << "samples:    " << audio.samples.size() << "\n";
        std::cout << "duration:   "
                  << static_cast<double>(audio.samples.size()) / audio.sampleRate
                  << " s\n";
        std::cout << "first samples: ";
        for (std::size_t i = 0; i < audio.samples.size() && i < 10; ++i) {
            std::cout << audio.samples[i] << (i == 9 ? "" : " ");
        }
        std::cout << '\n';
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
