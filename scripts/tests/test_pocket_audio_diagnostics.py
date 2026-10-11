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
    int Write(const int16_t* data, int samples);
    void PrintSignalExpectations() {}
    void EnableOutput(bool enabled) { playback_.store(enabled); }
    uint32_t TxPeak() const { return block_peak_; }
    int WriteSamples(const int16_t* data, int samples, float output_gain) {
        assert(playback_.load());
        if (output_gain > 0.08f) assert(diagnostic_playback_.load());
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
    loud_tone.safe_volume_=100;
    loud_tone.playback_=true;
    loud_tone.peak_=0;
    const std::array<int16_t, 4> full_scale={32767,-32768,32767,-32768};
    assert(loud_tone.Write(full_scale.data(), full_scale.size()) == 4);
    assert(loud_tone.peak_ <= 2621); // Normal playback retains its cap after the loud test.
    PocketAudioCodec failed;
    failed.fail_=true;
    assert(!failed.DiagnosticTone(false, true));
    assert(failed.safe_volume_ == 20 && !failed.playback_ && !failed.diagnostic_playback_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = Path(directory) / 'tone.cpp', Path(directory) / 'tone.exe'
            write_start = source.index('int PocketAudioCodec::Write(')
            write_end = source.index('\nint PocketAudioCodec::WriteSamples(', write_start)
            cpp.write_text(fixture + source[write_start:write_end] + source[start:end] + checks)
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
