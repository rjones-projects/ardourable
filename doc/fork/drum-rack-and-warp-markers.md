# Drum Rack and Warp Markers for Ardour — design

Status: slices 1–2 done (core models + working `DrumRack` processor, scriptable from Lua; no GUI yet)
Base: Ardour `master` after 9.8 (`9.8-241-gfd4ef0523b`)
Branch: `feature/drum-rack-warp-markers`

## 1. Goals

Close two gaps that people moving from Ableton Live notice first:

| Live feature | What it gives you | Ardour today |
|---|---|---|
| Drum Rack | One MIDI track, 128 pads, each pad its own instrument + FX chain, choke groups, per-pad mix | One instrument per track; drum kits rely on a single multi-sample plugin (DrumGizmo, sfizz, etc.) or one track per drum |
| Warp markers | Pin points in audio to beats and drag them; the audio between stretches piecewise | A region has a single stretch ratio (`Region::_stretch`), applied offline by a `Filter` that renders a new source |

Non-goals for now: Live's Simpler/Sampler UI, macro knobs, audio-to-MIDI, automatic transient/beat detection for warping (could reuse `TransientDetector` later).

## 2. Drum Rack

### 2.1 Shape

A new processor, `ARDOUR::DrumRack : public Processor`, that sits in a MIDI track's
processor box in place of an instrument. MIDI in, audio out (plus optional
per-pad audio outputs later). Internally:

```
MIDI in ──► DrumPadRouter ──► pad 0 MIDI ──► instrument ─► fx… ─► gain/pan ─┐
                          ├─► pad 1 MIDI ──► instrument ─► fx… ─► gain/pan ─┼─► sum ─► audio out
                          └─► pad N …                                       ┘
```

Two layers, so the RT-critical logic can be tested without a session:

1. **`DrumPadRouter`** (slice 1): pure MIDI routing. Splits the input stream by
   trigger note, rewrites to each pad's play note, implements choke groups,
   tracks sounding notes so it can flush cleanly. No allocation, no locks.
2. **`DrumRack`** (slice 2): owns the pads' plugins and buffers, runs them, and
   mixes. Like `RegionFxPlugin` (`libs/ardour/region_fx_plugin.cc`), it drives
   `Plugin`s directly rather than through a `PluginInsert`; plugins don't depend
   on having an insert (`Plugin::_pib` is only used by `PlugInsertBase` itself).

### 2.2 Pad semantics (matches Live where it matters)

| Term | Meaning |
|---|---|
| trigger note | Incoming note that fires the pad (Live: "Receive"). One pad per note. |
| play note | Note the pad's chain receives (Live: "Play", default C3/60). Unset = pass the trigger note through. |
| choke group | 0 = none, 1–16. Firing a pad releases every sounding note on the other pads in its group and sends them All Sound Off (CC 120) so release tails stop. A pad never chokes itself. |
| mute | Pad drops note-ons and channel messages. |

Routing rules in `DrumPadRouter::process()`:

- Note on/off and polyphonic aftertouch go to the matching pad only.
- Note-offs and aftertouch for a pad that isn't sounding are dropped, so a choked
  pad's own late note-off never reaches its chain.
- CC, program change, channel pressure, pitch bend and system messages go to every
  unmuted pad (as Live does for chains).
- CC 120/123 reset the router's tracking for that channel.
- `flush()` releases everything; called on transport stop/locate and before a pad
  layout swap.

### 2.3 Threading and state (implemented)

- The pad set (pads + router) lives in a `SerializedRCUManager`, as
  `DiskIOProcessor` does for its channel list. Every edit (add, remove,
  reconfigure, gain) copies the set off the RT thread, prepares plugins and
  buffers, and publishes. `run()` picks up the new set at the start of a cycle.
- Copying a set copies the layout but **not** the router's note state, so the
  writer never reads memory the RT thread is mutating.
- On a swap, `run()` hands held notes over: if a pad keeps the same plugin
  instance and play note, its sounding-note count moves to the new router
  (`DrumPadRouter::adopt_voices`), so held notes keep sounding and their
  note-offs still arrive. Otherwise the notes are released into that plugin.
  Changing a pad's gain therefore never cuts a note; changing its play note does.
- Old sets are freed only off the RT thread: the RT thread keeps a reference
  to the last set it used, and the RCU dead-wood list holds another, so the
  RT thread never drops the last reference.
- Session XML: the processor node (`type="drumrack"`) has one `Pad` child per pad
  (id, name, trigger/play note, choke group, mute, gain) with a `Plugin` child
  carrying type, unique id and the plugin's own state. Each pad's id is the
  plugin's insert id, so LV2 state files don't collide. A plugin that isn't
  installed keeps its pad and XML verbatim, so saving doesn't lose it.
  `Route::set_processor_state()` recreates the rack from `type="drumrack"`.

### 2.4 Audio path

Implemented:

- MIDI in, stereo out. A track with audio inputs as well loses the audio at
  the rack, as with any instrument.
- Each pad owns its `BufferSet` (sized to the engine's maximum buffer), and its
  in/out `ChanMapping`s are built when the pad is prepared, so `run()` doesn't
  allocate.
- Mono plugins feed both channels; plugins with more than two outputs
  contribute their first two.
- Per-pad gain (linear). Muted and zero-gain pads still run, so release tails
  and note-offs are processed and unmuting is click-free at the plugin.
- Bypass releases held notes and outputs silence.
- Transport stop and locate release held notes, both in the router and via
  each plugin's own `realtime_handle_transport_stopped()`/`realtime_locate()`.

Still to do:

- Per-pad pan, and gain as `AutomationControl`s so both are automatable and
  map to controllers.
- Latency: the rack reports its highest pad latency, but doesn't yet delay
  lower-latency pads to match.
- FX chains per pad (today a pad is one plugin).
- Optional per-pad outputs (Live's "return chains" equivalent).

### 2.5 Using it today (Lua)

There's no GUI yet, but the rack is fully usable from Ardour's Lua scripting
window (Window > Scripting) or as an editor action:

```lua
local rack = ARDOUR.LuaAPI.new_drum_rack (Session)
-- trigger note, play note (255 = pass through), choke group, plugin, type
ARDOUR.LuaAPI.drum_rack_add_pad (Session, rack, 36, 60, 0, "ACE Reasonable Synth", ARDOUR.PluginType.LV2)
ARDOUR.LuaAPI.drum_rack_add_pad (Session, rack, 42, 60, 1, "ACE Reasonable Synth", ARDOUR.PluginType.LV2)
ARDOUR.LuaAPI.drum_rack_add_pad (Session, rack, 46, 60, 1, "ACE Reasonable Synth", ARDOUR.PluginType.LV2)
route:add_processor_by_index (rack, 0, nil, true)

rack:to_drumrack ():set_pad_gain (0, 0.8)
ARDOUR.LuaAPI.drum_rack_set_pad (rack, 1, 42, 60, 1, true) -- mute pad 1
```

`share/scripts/_drum_rack_demo.lua` adds a four-pad kit (hats choked
together) to the selected MIDI tracks. Swap in any instrument plugin by name.
The rack is saved with the session. It should show in the mixer strip's
processor box like any processor that's displayed to the user, but the GUI
wasn't built for this slice, so that's unverified; either way its pads can't be
edited from there until the pad-grid UI exists.

### 2.6 UI (later slice)

- A 4×4 pad grid (16 visible, scrollable to 128), drag-and-drop a plugin or sample
  onto a pad. Ardour bundles no one-shot sampler (ACE Fluid Synth plays SF2
  only), so dropping a sample needs either a small built-in sampler or an
  external one such as sfizz.
- Choke group, play note, mute and solo per pad in an inspector.
- The Push 2 surface already supports a pad grid; map pads to rack pads when a
  DrumRack is the selected track's instrument.

## 3. Warp markers

### 3.1 Shape

Two places in Ardour stretch audio, and they need different strategies:

| Where | Engine today | Plan |
|---|---|---|
| Timeline audio regions | Offline `RBStretch`/`SPStretch`/`STStretch` filters render a new source (`libs/ardour/rb_effect.cc`) | **Offline, piecewise via RubberBand's key-frame map** (this slice) |
| Cue/clip launcher (`AudioTrigger`) | RubberBand in `OptionProcessRealTime` mode (`triggerbox.cc`) | Real-time: change the time ratio per segment as playback crosses markers. Key-frame maps are offline-only. |

Doing the timeline first keeps the first slice out of the RT path entirely.

### 3.2 Data model: `WarpMap` (this slice)

A piecewise-linear map from region-relative source samples to output samples:

- Implicit anchors at `(0, 0)` and `(source_length, target_length)`.
- User markers are interior anchors, strictly increasing in both coordinates,
  so every segment has a positive, finite ratio.
- Operations: add, add-at-current-position (pin), move, remove, map both ways,
  per-segment ratio, rescale on length change, XML state.
- `key_frames(output_length)` produces the `std::map<size_t,size_t>` that
  `RubberBandStretcher::setKeyFrameMap()` takes, scaled to the render length.

`TimeFXRequest` gains a `key_frames` map, and `RBEffect::run()` passes it to the
stretcher after the study pass and before the first `process()` call. That order
is what RubberBand's own header requires (`setKeyFrameMap` "may not be called
after the first call to process()"; the overall ratio still comes from
`setTimeRatio`). With an empty map the code path is unchanged.

### 3.3 Next slices

1. **Persist on the region.** Add an optional `WarpMap` to `AudioRegion`, saved
   inside the region's XML node. When warp markers change, re-run the stretch from
   the ancestral source (the existing `ancestral_start`/`stretch` bookkeeping in
   `RBEffect::run()` already handles "stretch from the original, not the last
   render").
2. **Musical time.** Store marker targets in beats (`Temporal::Beats`) rather than
   samples, so a tempo change re-renders correctly. Regions already carry a
   position time domain (`Region::position_time_domain()`), which this can follow.
3. **Editor UI.** Markers drawn on the region's waveform; drag to move, double-click
   to add, a "warp" edit mode alongside the existing stretch tool.
4. **Clips.** Add `WarpMap` to `AudioTrigger`; in `AudioTrigger::run()`, call
   `setTimeRatio()` per segment as the read position crosses each marker.
5. **Auto-warp.** Seed markers from `TransientDetector`/aubio onsets.

### 3.4 Constraints and open questions

- RubberBand wants "a reasonable gap" between key frames. The UI should enforce a
  minimum marker spacing (tens of milliseconds); `WarpMap` itself only enforces
  strict ordering.
- StaffPad and SoundTouch don't take key frames. When those algorithms are
  selected with warp markers present, the fallback is to stretch per segment and
  concatenate, at the cost of possible seams at marker boundaries.
- Re-render on every marker drag is too slow for long regions. Render in the
  background and show the old render until it finishes, or render only the
  affected segments.

## 4. Delivery plan

| Slice | Content | State |
|---|---|---|
| 1 | `DrumPadRouter` + `WarpMap` + RubberBand key-frame hook + unit tests | Done |
| 2 | `DrumRack` processor hosting per-pad plugins, mixing to stereo, session state, Lua API | Done |
| 3 | `AudioRegion` warp persistence + re-render | Next |
| 4 | Drum Rack pad-grid UI; warp markers on waveform | Later |
| 5 | Warp in clip launcher; Push 2 pad mapping; per-pad outputs | Later |

## 5. Testing

- Unit tests (cppunit), all part of the combined `run-tests` program and, with
  `--single-tests`, also built on their own:
  - `warp_map_test.cc` (11): mapping, validation, key frames, state.
  - `drum_pad_router_test.cc` (13): routing, choke, flush, voice hand-over.
  - `drum_rack_test.cc` (11): runs a real `DrumRack` with ACE Reasonable Synth
    (bundled, `libs/plugins/reasonablesynth.lv2`) on each pad in a dummy-backend
    session and checks the rendered audio: note routing, pad gain, choke (and a
    no-choke control), bypass, state round trip, held notes surviving a gain
    change but released by a play-note change, and a MIDI track hosting the
    rack and rebuilding it from saved route state.

  The held-note test leans on the synth's envelope (5 ms attack, 800 ms decay,
  100 ms release): a held note is still audible 200 ms after the swap, a
  released one isn't. Disabling the hand-over makes it fail.

  ```sh
  ./waf configure --test --single-tests
  ./waf build --targets=libardour,unit-test-warp_map,unit-test-drum_pad_router,unit-test-drum_rack,ardev_common_waf.sh
  (cd libs/plugins/reasonablesynth.lv2 && ../../../waf build)   # LV2 bundle incl. .ttl
  (cd libs/panners && ../../waf build)                          # sessions need panners
  (TOP=$PWD; . build/gtk2_ardour/ardev_common_waf.sh
   build/libs/ardour/test_warp_map && build/libs/ardour/test_drum_pad_router &&
   build/libs/ardour/test_drum_rack)
  ```

  The test binaries need the dev environment from `ardev_common_waf.sh`;
  without it `ARDOUR::init()` hangs. (`libs/ardour/run-tests.sh --single`
  currently prefixes absolute paths with `./`, so call the binaries directly.)
- The Lua API was also checked end to end with the headless `luasession` tool:
  create a session, build a four-pad rack through `LuaAPI`, edit pads, add it
  to a MIDI track, save, reload, and find the rack restored with its pads. (In
  this environment `luasession` then hangs on the second `close_session()`;
  it does the same in a script with no Drum Rack at all.)
- Slice 3 adds a render test: a click track with markers at known positions,
  checking that the clicks land on the target samples (± stretcher tolerance).

## 6. Upstream

Ardour is GPLv2+, so the fork stays GPLv2+. Keeping both features as separate
classes with narrow hooks (`TimeFXRequest::key_frames`, a new processor type)
keeps the diff easy to rebase onto upstream `master` and easy to propose
upstream later. Discuss on discourse.ardour.org before sending anything; the
developers have strong views on UI and RT design.
