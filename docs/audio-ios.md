# Speakers on iOS 26 (lisa): the user-space side (2026-11-16, host only)

How sound from iOS 26 (23F84) reaches lisa's speakers, what was built, what was read in the dyld cache and the IPSW,
and what only the phone can answer. The kernel side is [`kernel/drivers/audio/README.md`](../kernel/drivers/audio/README.md)
(`sound=4` serves `/dev/hakios-audio`). **Nothing here has run on the phone.**

## 1. The chain

```
app / SpringBoard (AudioServicesPlaySystemSound, AVAudioPlayer, ...)
  -> audiomxd (launchd com.apple.audiomxd, user mobile: AudioSession, SystemSoundServer, AudioQueue, the HAL)
     -> VirtualAudio.plugin (/Library/Audio/Plug-Ins/HAL): routes, DSP chains, volume
        -> DLisaAudio.driver (/Library/Audio/Plug-Ins/HAL, ours): device "Speaker", 2 x 16 bit at 48 kHz
           -> /dev/hakios-audio (write, never blocks; ioctl HAKIOS_AUDIO_SET_VOLUME)
              -> dlisa.sound.c sound=4: DC blocker, gain (sound_atten + the volume), clip, ASM -> ADM -> AFE PRIMARY_MI2S_RX
                 -> TFA9873 0x35 (bottom) and 0x34 (top), powered up only while there is sound
```

## 2. What the 23F84 binaries say (static reading; iPad17,1 IPSW, `ipsw dyld str/disass`)

- **audiomxd hosts the HAL** in iOS 26 (mediaserverd no longer exists): its launchd plist serves
  `com.apple.audio.AudioSession`, `...SystemSoundServer-iOS`, `...AudioQueueServer`, `...driver-registrar`,
  `com.apple.coremedia.volumecontroller.xpc`, and runs as `mobile`. `/dev/hakios-audio` is 0666.
- **Where CoreAudio looks for plug-ins**: `/Library/Audio/Plug-Ins/HAL` and `/Library/Apple/Audio/Plug-Ins/HAL` (CoreAudio
  strings), plus the system's `/System/Library/Audio/Plug-Ins/HAL` (BuiltinAudioPlugin, AppleAOPAudioPlugin,
  BTAudioHALPlugin, AirPlayHalogen, ...; our root has none of those: `select-system-26.py` leaves `/System/Library/Audio`
  out but for UISounds). MediaExperience names `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` explicitly. A bundle
  ending `.driver` is what CoreAudio takes (`".driver"` in CoreAudio). CoreAudio also has a remote path for plug-ins
  (`AudioServerPlugInRegisterRemoteUsingDriverService`, `com.apple.audio.Core-Audio-Driver-Service.helper`), but no such
  helper daemon is in this root: the plug-in's first log line names the process that loaded it.
- **The interface** is still the macOS one: CFPlugIn type `443ABAB8-E7B3-491A-B985-BEB9187030DB`, the factory named in
  `CFPlugInFactories`. Apple's own `BuiltinAudioPlugin.driver` (26.5) has exactly that shape, plus
  `AudioServerPlugIn_IOKitUserClients` (`IOAudio2DeviceUserClient`, `IOPAudio*`), and `AppleAOPAudioPlugin` adds
  `AudioServerPlugIn_LoadingConditions` (IOService matching). Neither key is needed by a plug-in that opens no IOKit
  user client.
- **The iPhone's own device names** in 26: BuiltinAudioPlugin builds `Speaker`, `Receiver`, `HPMic`, `HP16Mic`,
  `Digital Mic` from the device tree's `/product/audio` (`builtin-plugin-name`). VirtualAudio has device classes
  `Device_Speaker_Aspen`, `Device_Receiver_Aspen`, `Device_Hawking_Aspen`, `Device_Actuator_Aspen`, `Device_Codec_Aspen`,
  `Device_AOP_Aspen`, `Device_Null_Aspen`, ... and the strings `Speaker`, `Receiver`, `Hawking`, `Actuator`, `Codec`,
  `Null_Device`. Those are HakiOS's UIDs (iOS 16): the plug-in keeps them by default.
- **Product**: VirtualAudio's `PlatformUtilities_iOS::GetProductType` maps `MGGetProductType()` (a 32-bit product id) to
  its own ProductID with a large compare tree; unknown ones go through `IsAcousticIDHandled` and else
  `ProductID::Unknown`, for which there are routing handlers of their own (`RoutingHandler_Playback_Unk`,
  `_Record_Unk`, `_PlaybackAndRecord_Unk`). Device settings come in families (`DeviceSettings_Aspen`, `_D20Family`,
  `_D40Family`). The D16 identity's product id was not resolved offline (libMobileGestalt keys its product table by a
  20-byte hash); libdlisa now prints it from audiomxd.
- **Tunings** moved from `/Library/Audio/Tunings/<model>` (iOS 16: `D22`) to `/Library/Audio/Tunings/AID<AcousticID>`
  (AudioToolbox's `"AID%d"`, MobileGestalt key `AcousticID`). The iPad IPSW has `AID2028` and `AID2029` (its own
  models: `VAD/graph_configurations.plist`, `speaker_general.*`, `HAL/speaker_hal.*`, ...) and `Generic`. Our root keeps
  `Generic` only. Whether VirtualAudio refuses a speaker route without its product's AID folder, or only plays without
  the speaker's DSP chain, is one of the phone's answers.
- **Speaker protection**: VirtualAudio builds `DSPChain_SpeakerProtection` / `ClosedLoopSpeakerProtection` and says
  `HAL Speaker Protection is not available for DSP chain %s` when it cannot; the speaker device's properties
  `speaker-thiele-small`, `speaker-trim-gains`, `speaker-eq` are what an iPhone's speaker gives it. The plug-in gives
  none; the log shows what is asked (`asked of object N (Speaker) what it does not have`).

## 3. What was built

| Piece | Where | What |
| --- | --- | --- |
| `DLisaAudio.driver` | `userland/audio/` (`build-audio.sh`) | HakiOS's `audio_plugin.c` ported: devices by boot-arg, volume control to the kernel, console log |
| libdlisa in audiomxd | `userland/inject/audio-probe.c`, conf | `dlisa[audiomxd]: audio: MobileGestalt product type ..., AcousticID ..., closed-loop haptics ...` 2 s after start; with `dlisa_oslog=audiomxd`, VirtualAudio's and the HAL's own os_log lines on the console |
| `dlisa_sndtest` | `userland/inject/sound-test.c` (SpringBoard) | plays UISounds files as system sounds at given seconds |
| test root | `userland/audio/build-root-audio.sh` | BASE (int14) + the three above |

### Plug-in boot-args

| Boot-arg | Meaning |
| --- | --- |
| `dlisa_audio=0` | loads, publishes nothing |
| `dlisa_audio_devs=LIST` | roles `speaker`, `receiver`, `hawking`, `actuator`, `codec`, each `ROLE[:UID]`; default `speaker,hawking,actuator,codec` with UIDs `Speaker`, `Hawking`, `Actuator`, `Codec` |
| `dlisa_sndtest=S[:NAME],...` | (libdlisa, SpringBoard) play `/System/Library/Audio/UISounds/NAME.caf` S seconds after SpringBoard started; default `ReceivedMessage` |
| `dlisa_oslog=audiomxd` | (libdlisa) audiomxd's os_log on the console |

### Plug-in log markers (`dlisa-audio[PROCESS PID]:`)

- `factory called`, then `loaded by the HAL: on; devices: Speaker, Hawking, Actuator, Codec; /dev/hakios-audio: open`.
- `a client of Speaker: pid N`, `Speaker: I/O starts`.
- `the volume is V: X dB under the loudest the kernel allows` (VirtualAudio sets it; HakiOS saw it set once to the top).
- `Speaker: N frames in the last second to /dev/hakios-audio, peak P of 32768 (volume V)` once a second while sound plays.
- `Speaker: I/O stops after T s, S s of it sound, peak P, F frames not taken`.
- `asked of object N (UID) what it does not have: 'xxxx'`: the properties VirtualAudio wanted and did not get (the
  next change to make, if it does not route).

## 4. Volume and the HUD

On iOS 16 (HakiOS) VirtualAudio kept the speaker in software volume: the buttons and Control Center scale the samples,
the device's own control was set once to the top. SpringBoard shows the volume HUD when the active route's volume
changes; on periph-p1 the keys reached backboardd but no HUD came, because audiomxd had no output device. With the
plug-in routed, `buttons_seq=vu@...` should give the HUD (screenshot) and `peak` lines that rise and fall with it.

## 5. Open questions only the phone answers (in this order)

1. Does audiomxd load the plug-in (the `factory called` line, which process)?
2. Does VirtualAudio take the `Speaker` device and route to it (`a client of Speaker`, `I/O starts`), or what does it ask
   that we lack, and what do its own lines say (`Unable to locate a concrete route`, `Unsupported product type`,
   `HAL Speaker Protection is not available`, tunings not found)?
3. The product type and AcousticID of the D16 identity (libdlisa's line). If VirtualAudio wants `AID<n>`: a symlink
   `/Library/Audio/Tunings/AID<n>` to the iPad's `AID2028` (VAD + HAL, 0.6 MB) is the next root.
4. Whether the frames reach the DSP and the amplifiers (`Darwin plays: the amplifiers are powered up`, the DSP's frames a
   second near 48000, `underflows` 0), and the TFA's temperature and flags while they play.
