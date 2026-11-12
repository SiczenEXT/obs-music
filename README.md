# OBS Music Folder Player

A native Windows x64 audio-input source for OBS Studio. It plays supported audio files from a selected folder in alphabetical order, advances automatically, and repeats the playlist. Audio is sent directly to libobs, so it appears as **one source in the OBS Audio Mixer**.

## OBS-native settings UI

The plugin uses OBS's standard source-properties controls, so labels, the path picker, and the button follow the active OBS theme automatically. The Properties panel contains, in order:

1. **Source name** — name shown in OBS Sources. Apply the properties to commit a rename.
2. **Music folder** — choose the folder containing audio files. Selecting a folder starts/reloads playback.
3. **Play / Pause** — toggle playback without losing the current position.
4. **Stop and restart** — pause and rewind to the first track.
5. **Previous track** and **Next track** — change tracks immediately.
6. **Add / rescan folder** — rescan the selected folder after adding/removing songs.

No custom window, browser source, VLC, or desktop-audio capture is used.

## Playback

- One stereo audio source, shown in OBS Audio Mixer.
- Starts playing as soon as a valid folder is selected.
- Plays files sequentially in case-insensitive alphabetical filename order.
- Restarts at the first track after the last track.
- Scans the selected folder's top level (not subfolders).
- Uses Windows Media Foundation to decode MP3, WAV, FLAC, M4A, AAC, and WMA where the Windows installation provides the codec.
- Converts output to 48 kHz stereo PCM before sending it to OBS.
- Decodes on a worker thread and keeps up to 3 seconds queued ahead of the output thread.
- Sends fixed 10 ms stereo audio blocks; initial playback and recovery after a track change prebuffer 500 ms.

Use audio that you have permission to play on stream.

## Download the installable build

Every push to `main` runs the Windows x64 build in GitHub Actions. Open the repository's **Actions** tab, select **Build Windows OBS Plugin**, open the latest successful run, and download the `OBS-Music-Folder-Player-Windows-x64` artifact. The ZIP contains the OBS plugin folder, including the compiled DLL.

## Install

1. Close OBS Studio.
2. Extract the ZIP.
3. Copy the extracted `obs-music-folder` directory into `%PROGRAMDATA%\obs-studio\plugins\`.
4. Start OBS again.
5. In **Sources**, press **+** and add **Music Folder Player**.
6. Open its Properties, set the source name, choose the music folder, then press **Add / rescan folder** if you changed the folder contents.
7. Confirm **Music Folder Player** is visible in the Audio Mixer.
8. Open **Settings → Hotkeys** and find the Music Folder Player bindings to assign your preferred keys. The plugin registers Toggle Playback, Stop and Rewind, Previous Track, and Next Track.

The installed layout is:

```text
%PROGRAMDATA%\obs-studio\plugins\obs-music-folder\
  bin\64bit\obs-music-folder.dll
  data\locale\en-US.ini
```

The build targets Windows x64 and uses the OBS plugin-template dependency setup pinned in `.github/workflows/build-windows.yml`.
