#include <obs-module.h>
#include <util/platform.h>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr size_t kFramesPerBlock = 1024;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;

struct MusicFolderSource {
    obs_source_t *source = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false;
    bool reloadRequested = true;
    fs::path folder;
};

static std::wstring utf8_to_wide(const char *text)
{
    if (!text || !*text)
        return {};

    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (needed <= 0)
        return {};

    std::wstring result(static_cast<size_t>(needed), L'\\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), needed);
    result.pop_back();
    return result;
}

static std::string wide_to_utf8(const std::wstring &text)
{
    if (text.empty())
        return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};

    std::string result(static_cast<size_t>(needed), '\\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), needed, nullptr, nullptr);
    result.pop_back();
    return result;
}

static bool is_supported_audio_file(const fs::path &file)
{
    std::wstring ext = file.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    // Windows Media Foundation can decode these common local audio formats.
    // Actual decoder availability depends on the Windows codec installation.
    return ext == L".mp3" || ext == L".wav" || ext == L".flac" ||
           ext == L".m4a" || ext == L".aac" || ext == L".wma";
}

static std::vector<fs::path> scan_folder(const fs::path &folder)
{
    std::vector<fs::path> files;
    if (folder.empty())
        return files;

    std::error_code ec;
    if (!fs::exists(folder, ec) || ec || !fs::is_directory(folder, ec) || ec)
        return files;

    ec.clear();
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code itemError;
        if (it->is_regular_file(itemError) && !itemError && is_supported_audio_file(it->path()))
            files.push_back(it->path());
    }

    std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) {
        std::wstring left = a.filename().wstring();
        std::wstring right = b.filename().wstring();
        std::transform(left.begin(), left.end(), left.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        std::transform(right.begin(), right.end(), right.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return left < right;
    });
    return files;
}

static HRESULT create_audio_reader(const fs::path &file, ComPtr<IMFSourceReader> &reader)
{
    reader.Reset();
    HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader);
    if (FAILED(hr))
        return hr;

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr))
        return hr;

    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) return hr;
    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * sizeof(int16_t));
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                               kSampleRate * kChannels * sizeof(int16_t));
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                               0x00000003U);
    if (FAILED(hr)) return hr;
    hr = outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;

    hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(hr)) return hr;
    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) return hr;

    return reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outputType.Get());
}

static bool should_stop_or_reload(MusicFolderSource *state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->stopping || state->reloadRequested;
}

static bool wait_for_audio_time(MusicFolderSource *state,
                                std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    state->wake.wait_until(lock, deadline, [state] {
        return state->stopping || state->reloadRequested;
    });
    return !state->stopping && !state->reloadRequested;
}

static bool output_interleaved_pcm16(MusicFolderSource *state,
                                     const int16_t *interleaved,
                                     size_t totalFrames,
                                     uint64_t &nextTimestamp,
                                     std::chrono::steady_clock::time_point &deadline)
{
    std::array<float, kFramesPerBlock> left{};
    std::array<float, kFramesPerBlock> right{};

    size_t offset = 0;
    while (offset < totalFrames) {
        if (should_stop_or_reload(state))
            return false;

        const size_t frames = std::min(kFramesPerBlock, totalFrames - offset);
        for (size_t i = 0; i < frames; ++i) {
            left[i] = static_cast<float>(interleaved[(offset + i) * 2]) / 32768.0f;
            right[i] = static_cast<float>(interleaved[(offset + i) * 2 + 1]) / 32768.0f;
        }

        const uint64_t durationNs =
            static_cast<uint64_t>(frames) * kNanosecondsPerSecond / kSampleRate;
        const uint64_t now = os_gettime_ns();
        if (nextTimestamp < now)
            nextTimestamp = now;

        const auto steadyNow = std::chrono::steady_clock::now();
        if (deadline < steadyNow)
            deadline = steadyNow;

        obs_source_audio audio{};
        audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
        audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
        audio.frames = static_cast<uint32_t>(frames);
        audio.speakers = SPEAKERS_STEREO;
        audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec = kSampleRate;
        audio.timestamp = nextTimestamp;

        obs_source_output_audio(state->source, &audio);
        nextTimestamp += durationNs;
        deadline += std::chrono::nanoseconds(durationNs);
        offset += frames;

        if (!wait_for_audio_time(state, deadline))
            return false;
    }
    return true;
}

static void worker_main(MusicFolderSource *state)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        blog(LOG_ERROR, "[Music Folder Player] Could not initialize COM (0x%08X).",
             static_cast<unsigned int>(coHr));
        return;
    }

    fs::path folder;
    std::vector<fs::path> tracks;
    size_t trackIndex = 0;
    ComPtr<IMFSourceReader> reader;
    uint64_t nextTimestamp = os_gettime_ns();
    auto deadline = std::chrono::steady_clock::now();

    for (;;) {
        bool reload = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopping)
                break;
            if (state->reloadRequested) {
                folder = state->folder;
                state->reloadRequested = false;
                reload = true;
            }
        }

        if (reload) {
            reader.Reset();
            tracks = scan_folder(folder);
            trackIndex = 0;
            nextTimestamp = os_gettime_ns();
            deadline = std::chrono::steady_clock::now();
            if (tracks.empty() && !folder.empty()) {
                blog(LOG_WARNING, "[Music Folder Player] No supported audio files found in the selected folder.");
            } else if (!tracks.empty()) {
                blog(LOG_INFO, "[Music Folder Player] Found %zu audio file(s).", tracks.size());
            }
        }

        if (tracks.empty()) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait_for(lock, 500ms, [state] {
                return state->stopping || state->reloadRequested;
            });
            continue;
        }

        if (!reader) {
            const fs::path &track = tracks[trackIndex];
            const HRESULT openHr = create_audio_reader(track, reader);
            if (FAILED(openHr)) {
                blog(LOG_WARNING, "[Music Folder Player] Could not open a track (0x%08X); skipping it.",
                     static_cast<unsigned int>(openHr));
                trackIndex = (trackIndex + 1) % tracks.size();
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait_for(lock, 200ms, [state] {
                    return state->stopping || state->reloadRequested;
                });
                continue;
            }

            const std::string trackName = wide_to_utf8(track.filename().wstring());
            blog(LOG_INFO, "[Music Folder Player] Now playing: %s", trackName.c_str());
            nextTimestamp = os_gettime_ns();
            deadline = std::chrono::steady_clock::now();
        }

        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG sampleTime = 0;
        ComPtr<IMFSample> sample;
        const HRESULT readHr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &sampleTime, &sample);

        if (FAILED(readHr)) {
            blog(LOG_WARNING, "[Music Folder Player] A track could not be decoded (0x%08X); skipping it.",
                 static_cast<unsigned int>(readHr));
            reader.Reset();
            trackIndex = (trackIndex + 1) % tracks.size();
            continue;
        }

        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT bufferHr = sample->ConvertToContiguousBuffer(&buffer);
            if (SUCCEEDED(bufferHr)) {
                BYTE *bytes = nullptr;
                DWORD maxLength = 0;
                DWORD currentLength = 0;
                bufferHr = buffer->Lock(&bytes, &maxLength, &currentLength);
                if (SUCCEEDED(bufferHr)) {
                    const size_t bytesPerFrame = sizeof(int16_t) * kChannels;
                    const size_t safeLength = std::min<size_t>(currentLength, maxLength);
                    const size_t frameCount = safeLength / bytesPerFrame;
                    if (frameCount > 0) {
                        const auto *pcm = reinterpret_cast<const int16_t *>(bytes);
                        (void)output_interleaved_pcm16(state, pcm, frameCount, nextTimestamp, deadline);
                    }
                    buffer->Unlock();
                }
            }
        }

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            reader.Reset();
            trackIndex = (trackIndex + 1) % tracks.size();
        }
    }

    reader.Reset();
    if (uninitializeCom)
        CoUninitialize();
}

static void request_rescan(MusicFolderSource *state)
{
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->reloadRequested = true;
    }
    state->wake.notify_all();
}

static const char *music_source_name(void *)
{
    return obs_module_text("MusicFolderPlayer");
}

static void music_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "source_name", "");
    obs_data_set_default_string(settings, "folder", "");
}

static void *music_create(obs_data_t *settings, obs_source_t *source)
{
    auto *state = new MusicFolderSource();
    state->source = source;

    const char *initialName = obs_data_get_string(settings, "source_name");
    if (!initialName || !*initialName) {
        const char *obsName = obs_source_get_name(source);
        if (obsName && *obsName)
            obs_data_set_string(settings, "source_name", obsName);
    } else if (source && std::string(initialName) != obs_source_get_name(source)) {
        obs_source_set_name(source, initialName);
    }

    const char *folderUtf8 = obs_data_get_string(settings, "folder");
    state->folder = fs::path(utf8_to_wide(folderUtf8));
    state->worker = std::thread(worker_main, state);

    obs_source_set_audio_active(source, true);
    return state;
}

static void music_destroy(void *data)
{
    auto *state = static_cast<MusicFolderSource *>(data);
    if (!state)
        return;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
    }
    state->wake.notify_all();
    if (state->worker.joinable())
        state->worker.join();

    if (state->source)
        obs_source_set_audio_active(state->source, false);
    delete state;
}

static void music_update(void *data, obs_data_t *settings)
{
    auto *state = static_cast<MusicFolderSource *>(data);
    if (!state)
        return;

    const char *desiredName = obs_data_get_string(settings, "source_name");
    if (desiredName && *desiredName && state->source &&
        std::string(desiredName) != obs_source_get_name(state->source)) {
        obs_source_set_name(state->source, desiredName);
    }

    const char *folderUtf8 = obs_data_get_string(settings, "folder");
    fs::path newFolder;
    try {
        newFolder = fs::path(utf8_to_wide(folderUtf8));
    } catch (...) {
        newFolder.clear();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->folder = std::move(newFolder);
        state->reloadRequested = true;
    }
    state->wake.notify_all();
}

static bool folder_modified(void *data, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
    auto *state = static_cast<MusicFolderSource *>(data);
    if (!state || !settings)
        return false;

    const char *folderUtf8 = obs_data_get_string(settings, "folder");
    fs::path selectedFolder;
    try {
        selectedFolder = fs::path(utf8_to_wide(folderUtf8));
    } catch (...) {
        selectedFolder.clear();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->folder = std::move(selectedFolder);
        state->reloadRequested = true;
    }
    state->wake.notify_all();
    return false;
}

static bool rescan_button_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    request_rescan(static_cast<MusicFolderSource *>(data));
    return false;
}

static obs_source_info g_music_source_info{};

static obs_properties_t *music_get_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    // Native OBS controls inherit the active OBS theme.
    obs_properties_add_text(props, "source_name", obs_module_text("SourceName"), OBS_TEXT_DEFAULT);
    obs_property_t *folder = obs_properties_add_path(props, "folder", obs_module_text("MusicFolder"),
                                                       OBS_PATH_DIRECTORY, nullptr, nullptr);
    // Apply the folder as soon as it is selected, before the properties dialog closes.
    obs_property_set_modified_callback2(folder, folder_modified, data);
    obs_properties_add_button2(props, "add_music", obs_module_text("AddRescan"),
                                rescan_button_clicked, data);
    return props;
}

} // namespace

void register_music_folder_source()
{
    g_music_source_info.id = "music_folder_player";
    g_music_source_info.type = OBS_SOURCE_TYPE_INPUT;
    g_music_source_info.output_flags = OBS_SOURCE_AUDIO;
    g_music_source_info.get_name = music_source_name;
    g_music_source_info.create = music_create;
    g_music_source_info.destroy = music_destroy;
    g_music_source_info.update = music_update;
    g_music_source_info.get_defaults = music_get_defaults;
    g_music_source_info.get_properties = music_get_properties;
    g_music_source_info.icon_type = OBS_ICON_TYPE_MEDIA;

    obs_register_source(&g_music_source_info);
}
