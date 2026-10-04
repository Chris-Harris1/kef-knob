# KEF LS50 Wireless II local API

KEF doesn't publish this API. It's the same HTTP API the KEF Connect app uses, and it
was reverse-engineered by the community, mainly
[pykefcontrol](https://github.com/N0ciple/pykefcontrol). It applies to the W2-generation
speakers (LS50 Wireless II, LSX II, LSX II LT, LS60). The original LS50 Wireless (gen 1)
uses a different TCP protocol on port 50001, which [aiokef](https://github.com/basnijholt/aiokef) covers.

- Plain HTTP on port 80, no auth.
- Send commands to the **primary** speaker's IP.
- Values are typed JSON objects: `{"type": "<type>", "<type>": <value>}`.

## Reading a value

```
GET /api/getData?path=<path>&roles=value
→ [ <value> ]
```

Example: `GET /api/getData?path=player:volume&roles=value` → `[{"type":"i32_","i32_":35}]`

## Writing a value

The LS50 W2 takes a JSON POST:

```
POST /api/setData
Content-Type: application/json

{"path": "<path>", "roles": "value", "value": <value>}
```

(Older KEF firmware wanted a GET with `value` URL-encoded as a query parameter.)

## Paths

| What | Path | Value type | Notes |
|---|---|---|---|
| Volume | `player:volume` | `i32_` 0–100 | read/write |
| Mute | `settings:/mediaPlayer/mute` | `bool_` | read/write |
| Source | `settings:/kef/play/physicalSource` | `kefPhysicalSource` | `wifi`, `bluetooth`, `tv`, `optic`, `coaxial`, `analog`, `standby`. Write `powerOn` to wake; write `standby` to sleep. Picking a source also wakes the speaker. |
| Power status | `settings:/kef/host/speakerStatus` | `kefSpeakerStatus` | `standby` or `powerOn` (read only) |
| Now playing | `player:player/data` | object | `state` (`playing`/`paused`/`stopped`), `trackRoles.title`, `trackRoles.mediaData.metaData.{artist,album,albumArtist}`, `trackRoles.icon` (cover-art URL), `status.duration` (ms) |
| Track position | `player:player/data/playTime` | `i64_` ms | |
| Playback control | `player:player/control` | `{"control": "pause" \| "next" \| "previous"}` | uses `"roles": "activate"`; `pause` toggles play/pause |
| Speaker name | `settings:/deviceName` | `string_` | |
| Volume limit on/off | `settings:/kef/host/volumeLimit` | `bool_` | set in KEF Connect |
| Volume limit value | `settings:/kef/host/maximumVolume` | `i32_` | |
| Volume step | `settings:/kef/host/volumeStep` | `i32_` | |
| Model | `settings:/kef/host/modelName` | `string_` | |
| Firmware | `settings:/releasetext` | `string_` | |
| MAC address | `settings:/system/primaryMacAddress` | `string_` | |
| Network info | `network:info` | object | `networkInfo.wireless.{ssid,signalLevel,frequency}` |
| EQ profile | `kef:eqProfile` | object | |

### Examples

```sh
# volume 30
curl -X POST http://SPEAKER/api/setData -H 'Content-Type: application/json' \
  -d '{"path":"player:volume","roles":"value","value":{"type":"i32_","i32_":30}}'

# mute
curl -X POST http://SPEAKER/api/setData -H 'Content-Type: application/json' \
  -d '{"path":"settings:/mediaPlayer/mute","roles":"value","value":{"type":"bool_","bool_":true}}'

# switch to TV (HDMI eARC)
curl -X POST http://SPEAKER/api/setData -H 'Content-Type: application/json' \
  -d '{"path":"settings:/kef/play/physicalSource","roles":"value","value":{"type":"kefPhysicalSource","kefPhysicalSource":"tv"}}'

# next track
curl -X POST http://SPEAKER/api/setData -H 'Content-Type: application/json' \
  -d '{"path":"player:player/control","roles":"activate","value":{"control":"next"}}'
```

## Push updates (event queue)

Subscribing to an event queue means you don't have to poll each value:

```
POST /api/event/modifyQueue
{"subscribe": [{"path": "player:volume", "type": "itemWithValue"}, ...], "unsubscribe": []}
→ "{9b6c0e4e-...}"          (the queue id: a JSON string, braces included)

GET /api/event/pollQueue?queueId=%7B9b6c0e4e-...%7D&timeout=10
→ [ {"path": "player:volume", "itemType": "...", "itemValue": {"type":"i32_","i32_":36}}, ... ]
```

`pollQueue` is a long poll. It returns as soon as something changes, or `[]` once
`timeout` seconds pass. A queue expires if nobody polls it for roughly a minute, after
which the poll returns an error and you have to create a new queue.

The firmware in this repo subscribes to: volume, mute, source, speaker status, player data,
play time, device name, volume limit and maximum volume.
