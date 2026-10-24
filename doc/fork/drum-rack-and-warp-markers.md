# Drum Rack and Warp Markers for Ardour — design

Status: design + first slice (core-library models, no UI yet)
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

1. **`DrumPadRouter`** (this slice): pure MIDI routing. Splits the input stream by
   trigger note, rewrites to each pad's play note, implements choke groups,
   tracks sounding notes so it can flush cleanly. No allocation, no locks.
2. **`DrumRack`** (next slice): owns the pads' plugins and buffers, runs them, and
   mixes. Modelled on `RegionFxPlugin`, which already hosts `Plugin`s outside a
   `Route`'s processor list via `PlugInsertBase` (`libs/ardour/region_fx_plugin.cc`).

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

### 2.3 Threading and state

- Pad layout changes happen on the GUI thread. `DrumRack` will hold its pad set in
  a `SerializedRCUManager` (as `DiskIOProcessor` does for its channel list). The RT thread
  picks up a new pad set at the top of `run()`, calls `flush()` on the old router
  first, then switches. `DrumPadRouter::set_pads()` itself is not RT-safe and is
  never called from `run()`.
- Plugin instantiation per pad is done off the RT thread; the new pad set is only
  published once every plugin is configured and activated.
- Session XML: a `DrumRack` node with one `Pad` child per pad (trigger, play, choke,
  mute, gain, pan) and nested plugin state, reusing `Plugin::get_state()`. A
  missing plugin on load keeps the pad and marks it unavailable (same approach as
  `UnknownProcessor`).

### 2.4 Audio path (next slice)

- Each pad gets a scratch `BufferSet` from the session's thread buffers and its
  own `ChanMapping`, so pads can't read each other's buffers.
- Per-pad gain and pan as `AutomationControl`s owned by the rack, so they're
  automatable and appear in generic controller mappings.
- Latency: the rack reports the maximum pad latency and delays shorter pads to
  match, so layered pads stay phase-aligned.
- Optional per-pad outputs (Live's "return chains" equivalent) are a later stage.
  Ardour's flexible pin-routing already supports multi-out instruments, so the rack
  can expose `N*2` outputs and let the user route them.

### 2.5 UI (later slice)

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
| 1 | `DrumPadRouter` + `WarpMap` + RubberBand key-frame hook + unit tests | **This branch** |
| 2 | `DrumRack` processor hosting per-pad plugins, mixing to stereo, session state | Next |
| 3 | `AudioRegion` warp persistence + re-render | Next |
| 4 | Drum Rack pad-grid UI; warp markers on waveform | Later |
| 5 | Warp in clip launcher; Push 2 pad mapping; per-pad outputs | Later |

## 5. Testing

- Unit tests (cppunit): `libs/ardour/test/warp_map_test.cc` and
  `libs/ardour/test/drum_pad_router_test.cc`. They're part of the combined
  `run-tests` program, and with `--single-tests` also build on their own:

  ```sh
  ./waf configure --test --single-tests
  ./waf build --targets=libardour,unit-test-warp_map,unit-test-drum_pad_router,ardev_common_waf.sh
  (TOP=$PWD; . build/gtk2_ardour/ardev_common_waf.sh
   build/libs/ardour/test_warp_map && build/libs/ardour/test_drum_pad_router)
  ```

  The test binaries need the dev environment from `ardev_common_waf.sh`;
  without it `ARDOUR::init()` hangs. (`libs/ardour/run-tests.sh --single`
  currently prefixes absolute paths with `./`, so call the binaries directly.)
- Slice 2 adds a session-level test that loads a DrumRack with two ACE Reasonable
  Synth pads (bundled, `libs/plugins/reasonablesynth.lv2`) and checks choke
  behaviour in the rendered audio.
- Slice 3 adds a render test: a click track with markers at known positions,
  checking that the clicks land on the target samples (± stretcher tolerance).

## 6. Upstream

Ardour is GPLv2+, so the fork stays GPLv2+. Keeping both features as separate
classes with narrow hooks (`TimeFXRequest::key_frames`, a new processor type)
keeps the diff easy to rebase onto upstream `master` and easy to propose
upstream later. Discuss on discourse.ardour.org before sending anything; the
developers have strong views on UI and RT design.
