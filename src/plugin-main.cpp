#include <obs-module.h>

#include <mfapi.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-music-folder", "en-US")

extern void register_music_folder_source();

bool obs_module_load(void)
{
    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        blog(LOG_ERROR, "[Music Folder Player] Media Foundation startup failed (0x%08X).",
             static_cast<unsigned int>(hr));
        return false;
    }

    register_music_folder_source();
    blog(LOG_INFO, "[Music Folder Player] Loaded.");
    return true;
}

void obs_module_unload(void)
{
    MFShutdown();
    blog(LOG_INFO, "[Music Folder Player] Unloaded.");
}
