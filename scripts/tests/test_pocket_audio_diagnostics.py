import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pocket_serial_capture', ROOT / 'scripts/pocket_serial_capture.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def recording():
    pcm = struct.pack('<16000h', *([-32768, -1234, 0, 1234, 32767] * 3200))
    rows = [f'PCM {offset} {pcm[offset * 2:(offset + 64) * 2].hex()}'
            for offset in range(0, 16000, 64)]
    log = '\n'.join(['PCM_BEGIN rate=16000 samples=16000', *rows,
                     'PCM_END samples=16000 rx_errors=0 overruns=2 clipped=6400'])
    return pcm, log.encode()


class AudioDiagnosticsTests(unittest.TestCase):
    def test_custom_opus_output_avoids_extra_resampling_and_legacy_rates_are_preserved(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        source = (ROOT / 'main/audio/audio_service.cc').read_text(encoding='utf-8')
        start = source.index('void AudioService::SetDecodeSampleRate(')
        method = source[start:source.index('\nvoid AudioService::PushTaskToEncodeQueue(', start)]
        fixture = r'''
#include <cassert>
#include <mutex>
constexpr int ESP_AUDIO_MONO=1;
const char* TAG="test";
template <class... Args> void log(Args...) {}
#define ESP_LOGE(...) log(__VA_ARGS__)
#define ESP_LOGI(...) log(__VA_ARGS__)
struct esp_opus_dec_cfg_t { int rate, duration; };
struct esp_ae_rate_cvt_cfg_t { int input, output, channels; };
#define OPUS_DEC_CFG(rate, duration) esp_opus_dec_cfg_t{rate, duration}
#define RATE_CVT_CFG(input, output, channels) esp_ae_rate_cvt_cfg_t{input, output, channels}
int opens=0, resamplers=0, closed_resamplers=0, last_rate=0;
int esp_opus_dec_open(const esp_opus_dec_cfg_t* cfg, unsigned, void** decoder) {
    ++opens; last_rate=cfg->rate; *decoder=reinterpret_cast<void*>(1); return 0;
}
void esp_opus_dec_close(void*) {}
int esp_ae_rate_cvt_open(const esp_ae_rate_cvt_cfg_t*, void** resampler) {
    ++resamplers; *resampler=reinterpret_cast<void*>(2); return 0;
}
void esp_ae_rate_cvt_close(void*) { ++closed_resamplers; }
struct Codec { int rate=16000; int output_sample_rate() { return rate; } };
struct Board {
    bool custom=true;
    Codec codec;
    static Board& GetInstance() { static Board board; return board; }
    bool UsesCustomBackend() { return custom; }
    Codec* GetAudioCodec() { return &codec; }
};
class AudioService {
public:
    Codec* codec_=&Board::GetInstance().codec;
    int decoder_sample_rate_=16000, decoder_duration_ms_=60, decoder_frame_size_=960;
    void* opus_decoder_=reinterpret_cast<void*>(1);
    void* output_resampler_=nullptr;
    std::mutex decoder_mutex_;
    void SetDecodeSampleRate(int, int);
};
'''
        checks = r'''
int main() {
    AudioService service;
    service.SetDecodeSampleRate(24000, 60);
    assert(opens == 0 && resamplers == 0 && service.decoder_frame_size_ == 960);
    service.SetDecodeSampleRate(24000, 40);
    assert(opens == 1 && last_rate == 16000 && service.decoder_frame_size_ == 640);
    assert(resamplers == 0);
    Board::GetInstance().custom=false;
    service.SetDecodeSampleRate(24000, 60);
    assert(opens == 2 && last_rate == 24000 && service.decoder_frame_size_ == 1440);
    assert(resamplers == 1);
    Board::GetInstance().custom=true;
    service.SetDecodeSampleRate(24000, 60);
    assert(opens == 3 && last_rate == 16000 && service.decoder_frame_size_ == 960);
    assert(closed_resamplers == 1 && service.output_resampler_ == nullptr);
    // Unsupported native rates retain the established conversion path.
    Board::GetInstance().codec.rate=44100;
    service.SetDecodeSampleRate(24000, 60);
    assert(last_rate == 24000 && resamplers == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'native-rate.cpp', Path(directory) / 'native-rate.exe'
            cpp.write_text(fixture + method + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_burst_reply_packets_survive_before_main_task_and_short_reply_resumes_capture(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        source = (ROOT / 'main/application.cc').read_text(encoding='utf-8')
        start = source.index('    protocol_->OnIncomingAudio(')
        registration = source[start:source.index('\n\n    protocol_->OnAudioChannelOpened', start)]
        start = source.index('void Application::PrepareIncomingReply()')
        methods = source[start:source.index('\nvoid Application::HandleStateChangedEvent()', start)]
        start = source.index('case kDeviceStateSpeaking:', source.index('void Application::HandleStateChangedEvent()'))
        speaking = source[start:source.index('case kDeviceStateNotifying:', start)]
        fixture = r'''
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
constexpr int kDeviceStateIdle=0, kDeviceStateListening=1, kDeviceStateSpeaking=2;
constexpr int kListeningModeManualStop=0, kListeningModeAutoStop=1, kListeningModeRealtime=2;
namespace Lang { namespace Strings { const char* SPEAKING="speaking"; } }
struct AudioStreamPacket { int id; };
struct AudioService {
    int resets=0;
    bool processing=true;
    std::vector<int> packets;
    void ResetDecoder() { ++resets; packets.clear(); }
    void PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet) { packets.push_back(packet->id); }
    void EnableVoiceProcessing(bool enabled) { processing=enabled; }
    void EnableWakeWordDetection(bool) {}
    bool IsAfeWakeWord() { return false; }
};
struct Display { void SetStatus(const char*) {} };
struct Board {
    bool custom=true;
    static Board& GetInstance() { static Board board; return board; }
    bool UsesCustomBackend() { return custom; }
};
struct Protocol {
    std::function<void(std::unique_ptr<AudioStreamPacket>)> incoming;
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) { incoming=callback; }
};
class Application {
public:
    int state_=kDeviceStateListening, listening_mode_=kListeningModeAutoStop;
    bool aborted_=false;
    std::atomic<bool> custom_reply_audio_ready_{false};
    AudioService audio_service_;
    Display display_;
    std::unique_ptr<Protocol> protocol_=std::make_unique<Protocol>();
    std::deque<std::function<void()>> tasks_;
    int GetDeviceState() { return state_; }
    void SetDeviceState(int state) { state_=state; }
    void Schedule(std::function<void()> callback) { tasks_.push_back(callback); }
    void Drain() { while (!tasks_.empty()) { auto task=tasks_.front(); tasks_.pop_front(); task(); } }
    void PrepareIncomingReply();
    void FinishIncomingReply();
    void Install() {
'''
        middle = '''\n    }\n    void HandleStateChangedEvent() {\n        auto display=&display_;\n        switch(state_) {\n'''
        checks = r'''
        default: break;
        }
    }
};
int main() {
    Application burst;
    burst.Install();
    burst.audio_service_.packets.push_back(999);
    burst.PrepareIncomingReply();
    burst.protocol_->incoming(std::make_unique<AudioStreamPacket>(AudioStreamPacket{42}));
    burst.FinishIncomingReply(); // start, audio, stop all arrive before the main task.
    burst.Drain();
    assert(burst.state_ == kDeviceStateListening && !burst.custom_reply_audio_ready_);
    assert(!burst.audio_service_.processing && burst.audio_service_.resets == 1);
    assert(burst.audio_service_.packets == std::vector<int>{42});
    burst.protocol_->incoming(std::make_unique<AudioStreamPacket>(AudioStreamPacket{43}));
    assert(burst.audio_service_.packets == std::vector<int>{42});
    burst.PrepareIncomingReply();
    burst.protocol_->incoming(std::make_unique<AudioStreamPacket>(AudioStreamPacket{44}));
    burst.Drain();
    burst.HandleStateChangedEvent(); // duplicate STATE_CHANGED delivery must retain audio.
    assert(burst.audio_service_.resets == 2 && burst.audio_service_.packets == std::vector<int>{44});
    Board::GetInstance().custom=false;
    Application legacy;
    legacy.Install(); legacy.PrepareIncomingReply(); legacy.Drain(); legacy.HandleStateChangedEvent();
    assert(legacy.audio_service_.resets == 1 && !legacy.custom_reply_audio_ready_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'reply-start.cpp', Path(directory) / 'reply-start.exe'
            cpp.write_text(fixture + registration + middle + speaking + checks + methods, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_real_speaker_write_retries_partial_dma_without_duplicate_samples(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        source = (ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai/pocket_audio_codec.cc').read_text()
        start = source.index('int PocketAudioCodec::WriteSamples(')
        end = source.index('\nuint32_t PocketAudioCodec::PlaybackRms()', start)
        fixture = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include "audio_math.h"
constexpr int ESP_OK=0, ESP_ERR_TIMEOUT=1, ESP_ERR_INVALID_STATE=2;
struct Step { int result; size_t bytes; };
std::vector<Step> steps;
std::vector<uint8_t> written;
size_t calls=0;
int i2s_channel_write(int, const void* data, size_t length, size_t* bytes, unsigned timeout) {
    assert(timeout == 150 && calls < steps.size());
    const auto step=steps[calls++];
    *bytes=std::min(length, step.bytes);
    const auto* pointer=static_cast<const uint8_t*>(data);
    written.insert(written.end(), pointer, pointer+*bytes);
    return step.result;
}
int64_t esp_timer_get_time() { return 123456000; }
class PocketAudioCodec {
public:
    static constexpr int kChunk=128;
    int tx_handle_=1;
    bool ready_=true;
    std::atomic<bool> playback_{true};
    std::atomic<int> safe_volume_{80};
    std::atomic<uint32_t> tx_errors_{0}, playback_rms_{0}, tx_rms_{0}, tx_peak_{0};
    std::atomic<uint32_t> playback_at_ms_{0}, output_frames_{0};
    float speaker_previous_=0, speaker_filtered_=0;
    std::mutex output_mutex_;
    std::array<int32_t, kChunk*2> tx_words_{};
    std::array<float, kChunk> voice_samples_{};
    float speaker_gain_=2.0f;
    int WriteSamples(const int16_t*, int, float, float=1.0f);
};
'''
        checks = r'''
int main() {
    std::array<int16_t, 160> input{};
    std::array<int32_t, 320> expected{};
    float previous=0, filtered=0;
    for (size_t i=0; i<input.size(); ++i) {
        input[i]=static_cast<int16_t>(4000*std::sin(i*6.2831853*440/16000));
        expected[2*i]=expected[2*i+1]=pocket::ConvertSpeaker(input[i], previous, filtered, 80, 0.8f);
    }
    PocketAudioCodec recovered;
    steps={{ESP_ERR_TIMEOUT,256},{ESP_OK,128},{ESP_OK,640},{ESP_OK,256}};
    assert(recovered.WriteSamples(input.data(), input.size(), 0.8f) == 160);
    assert(calls == 4 && recovered.output_frames_ == 160 && recovered.tx_errors_ == 2);
    assert(written.size() == sizeof(expected));
    assert(std::memcmp(written.data(), expected.data(), written.size()) == 0);
    // A permanently stalled channel is bounded and counts already accepted frames.
    calls=0; written.clear();
    steps={{ESP_ERR_TIMEOUT,256},{ESP_ERR_TIMEOUT,0},{ESP_ERR_TIMEOUT,0}};
    PocketAudioCodec stalled;
    assert(stalled.WriteSamples(input.data(), input.size(), 0.8f) == 32);
    assert(calls == 3 && stalled.output_frames_ == 32 && stalled.tx_errors_ == 3);
    assert(written.size() == 256 && stalled.playback_rms_ == 0);
    calls=0; written.clear(); steps={{ESP_ERR_INVALID_STATE,0}};
    PocketAudioCodec disabled;
    assert(disabled.WriteSamples(input.data(), input.size(), 0.8f) == 0 && calls == 1);
    disabled.playback_=false;
    assert(disabled.WriteSamples(input.data(), input.size(), 0.8f) == 0 && calls == 1);
    // Exercise the actual boosted voice branch, not just the math helper.
    calls=0; written.clear(); steps={{ESP_OK,1024},{ESP_OK,256}};
    for (size_t i=0; i<input.size(); ++i) input[i]=static_cast<int16_t>(32000*std::sin(i*6.2831853*440/16000));
    PocketAudioCodec voice;
    voice.safe_volume_=100;
    assert(voice.WriteSamples(input.data(), input.size(), 0.8f, 2.0f) == 160);
    assert(voice.tx_errors_ == 0 && voice.tx_peak_ <= 21299 && written.size() == sizeof(expected));
    std::array<int32_t, 320> output{};
    std::memcpy(output.data(), written.data(), written.size());
    for (size_t i=0; i<input.size(); ++i) {
        assert(output[2*i] == output[2*i+1]);
        assert(std::abs(static_cast<int64_t>(output[2*i]) / 65536) <= 21299);
    }
    for (size_t i=0; i<32; ++i) assert(std::abs(voice.tx_words_[2*i]/65536.0f-
        voice.voice_samples_[i]*voice.speaker_gain_*32767) < 1.1f);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'partial-write.cpp', Path(directory) / 'partial-write.exe'
            cpp.write_text(fixture + source[start:end] + checks)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai'),
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_real_tones_duration_gain_and_state_restoration(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        source = (ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai/pocket_audio_codec.cc').read_text()
        start = source.index('bool PocketAudioCodec::DiagnosticTone(')
        end = source.index('\nvoid PocketAudioCodec::PrintSignalExpectations()', start)
        fixture = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include "audio_math.h"
template<class... Args> void Log(Args...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define TAG "test"
class PocketAudioCodec {
public:
    std::atomic<int> safe_volume_{20};
    std::atomic<bool> playback_{false}, diagnostic_playback_{false};
    float speaker_previous_=0, speaker_filtered_=0;
    bool ready_=true, fail_=false;
    static constexpr int kDmaDescriptors=8;
    std::array<int32_t, 320> tx_words_{};
    uint32_t frames_=0, peak_=0, block_peak_=0;
    bool DiagnosticTone(bool extended=false, bool loud=false);
    void PrintSignalExpectations() {}
    void EnableOutput(bool enabled) { playback_.store(enabled); }
    uint32_t TxPeak() const { return block_peak_; }
    int WriteSamples(const int16_t* data, int samples, float output_gain, float voice_boost=1.0f) {
        assert(playback_.load());
        assert(voice_boost == 1.0f);
        if (fail_) return 0;
        block_peak_=0;
        for (int i=0; i<samples; ++i) {
            const int32_t word=pocket::ConvertSpeaker(data[i], speaker_previous_,
                                                     speaker_filtered_, safe_volume_.load(), output_gain);
            tx_words_[i*2]=tx_words_[i*2+1]=word;
            const auto peak=static_cast<uint32_t>(std::abs(word / 65536));
            assert(peak <= static_cast<uint32_t>(output_gain * 32767));
            block_peak_=std::max(block_peak_, peak);
        }
        peak_=std::max(peak_, block_peak_);
        frames_+=samples;
        return samples;
    }
};
'''
        checks = r'''
int main() {
    PocketAudioCodec short_tone;
    assert(short_tone.DiagnosticTone());
    assert(short_tone.frames_ == 6080 && short_tone.peak_ >= 110 && short_tone.peak_ <= 130);
    assert(short_tone.safe_volume_ == 20 && !short_tone.playback_ && !short_tone.diagnostic_playback_);
    PocketAudioCodec long_tone;
    long_tone.safe_volume_=37;
    long_tone.playback_=true;
    assert(long_tone.DiagnosticTone(true));
    assert(long_tone.frames_ == 81280 && long_tone.peak_ >= 1800 && long_tone.peak_ <= 2000);
    assert(long_tone.peak_ > 14 * short_tone.peak_);
    assert(long_tone.safe_volume_ == 37 && long_tone.playback_ && !long_tone.diagnostic_playback_);
    PocketAudioCodec loud_tone;
    loud_tone.safe_volume_=59;
    assert(loud_tone.DiagnosticTone(false, true));
    assert(loud_tone.frames_ == 81280 && loud_tone.peak_ >= 19200 && loud_tone.peak_ <= 19400);
    assert(loud_tone.peak_ >= 9.9 * long_tone.peak_);
    assert(loud_tone.safe_volume_ == 59 && !loud_tone.playback_ && !loud_tone.diagnostic_playback_);
    PocketAudioCodec failed;
    failed.fail_=true;
    assert(!failed.DiagnosticTone(false, true));
    assert(failed.safe_volume_ == 20 && !failed.playback_ && !failed.diagnostic_playback_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'tone.cpp', Path(directory) / 'tone.exe'
            cpp.write_text(fixture + source[start:end] + checks)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/custom-esp32-nodemcu-pocket-ai'),
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_five_second_export_and_crc_detects_valid_hex_corruption(self):
        pcm, _ = recording()
        pcm *= 5
        rows = [f'PCM {offset} {pcm[offset * 2:(offset + 64) * 2].hex()}'
                for offset in range(0, 80000, 64)]
        log = '\n'.join(['PCM_BEGIN rate=16000 samples=80000', *rows,
                         f'PCM_END samples=80000 rx_errors=0 overruns=0 clipped=32000 crc32={zlib.crc32(pcm):08x}']).encode()
        data, metrics = capture.extract_pcm(log)
        self.assertEqual(data, pcm)
        self.assertEqual(metrics['frames'], 80000)
        self.assertEqual(metrics['clipping_percent'], 40)
        with self.assertRaisesRegex(ValueError, 'CRC32 mismatch'):
            capture.extract_pcm(log.replace(b'PCM 0 0080', b'PCM 0 0180', 1))
        with self.assertRaisesRegex(ValueError, 'missing its CRC32'):
            capture.extract_pcm(log.rsplit(b' crc32=', 1)[0])

    def test_baud_handshake_split_markers_and_recovery(self):
        class Port:
            baudrate = 115200
            sent = []
            def write(self, value): self.sent.append(value)
            def flush(self): pass
        port = Port()
        transport = capture.PcmSerialTransport(port)
        transport.feed(b'other log\nPCM_UART baud=921')
        self.assertEqual(port.baudrate, 115200)
        transport.feed(b'600\r\n')
        self.assertEqual(port.baudrate, 921600)
        self.assertEqual(port.sent, [b'\x06'])
        transport.feed(b'PCM_UART baud=115200\n')
        self.assertEqual(port.baudrate, 115200)
        self.assertEqual(port.sent, [b'\x06'])

    def test_export_preserves_signed_samples_and_reports_dropped_dma(self):
        pcm, log = recording()
        actual, metrics = capture.extract_pcm(log)
        self.assertEqual(actual, pcm)
        self.assertEqual(metrics['peak'], 32768)
        self.assertEqual(metrics['overruns'], 2)
        self.assertEqual(metrics['clipped'], 6400)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'mic.wav'
            capture.write_wav(log, path)
            with wave.open(str(path)) as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
                self.assertEqual(wav.readframes(16000), pcm)

    def test_export_rejects_missing_duplicate_reordered_or_truncated_rows(self):
        _, log = recording()
        rows = log.splitlines()
        for bad in (rows[:-1], rows[:8] + rows[9:], rows[:8] + rows[7:],
                    rows[:8] + [rows[9], rows[8]] + rows[10:],
                    [rows[0], rows[1][:-1], *rows[2:]]):
            with self.subTest(rows=len(bad)), self.assertRaises(ValueError):
                capture.extract_pcm(b'\n'.join(bad))

    def test_interleaved_logs_and_latest_recording(self):
        pcm, log = recording()
        mixed = log.replace(b'PCM 64 ', b'I (100) SystemInfo: heap\nPCM 64 ')
        self.assertEqual(capture.extract_pcm(log + b'\n' + mixed)[0], pcm)
        with self.assertRaises(ValueError):
            capture.extract_pcm(log + b'\nPCM_BEGIN rate=16000 samples=16000\n')

    def test_real_diagnostics_releases_queue_mutex_before_uart_logging(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('native C++ compiler not installed')
        source = (ROOT / 'main/audio/audio_service.cc').read_text(encoding='utf-8')
        start = source.index('void AudioService::PrintDiagnostics()')
        end = source.index('\nbool AudioService::IsAfeWakeWord()', start)
        function = source[start:end]
        # Compile the actual production method with a logger that attempts to
        # acquire the queue mutex from another thread at every logging call.
        fixture = r'''
#include <cassert>
#include <mutex>
#include <thread>
#include <array>
#define INCLUDE_xTaskGetHandle 0
constexpr unsigned MAX_ENCODE_TASKS_IN_QUEUE=2, MAX_SEND_PACKETS_IN_QUEUE=40,
    MAX_DECODE_PACKETS_IN_QUEUE=20, MAX_PLAYBACK_TASKS_IN_QUEUE=2;
struct Queue { unsigned count; unsigned size() const { return count; } };
class AudioService {
public:
    std::mutex audio_queue_mutex_;
    Queue audio_encode_queue_{1}, audio_send_queue_{2}, audio_decode_queue_{3},
        audio_playback_queue_{1}, audio_testing_queue_{4};
    static constexpr unsigned kAudioTestingPacketCapacity=33;
    void PrintDiagnostics();
};
AudioService service;
template<class... Args> void CheckedLog(const char*, const char*, Args... values) {
    bool acquired = false;
    std::thread producer([&] {
        acquired = service.audio_queue_mutex_.try_lock();
        if (acquired) service.audio_queue_mutex_.unlock();
    });
    producer.join();
    assert(acquired);
    const std::array<unsigned, 10> expected = {1,2,2,40,3,20,1,2,4,33};
    const std::array<unsigned, sizeof...(Args)> actual = {values...};
    assert(actual == expected);
}
#define TAG "AudioService"
#define ESP_LOGI(...) CheckedLog(__VA_ARGS__)
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'diagnostics.cpp', Path(directory) / 'diagnostics.exe'
            cpp.write_text(fixture + function + '\nint main() { service.PrintDiagnostics(); }\n')
            subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
