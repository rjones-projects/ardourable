/*
 * Copyright (C) 2026 Rolandok
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include <cstdint>
#include <vector>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace ARDOUR {

/** Configuration of one Drum Rack pad (Live calls this a "chain"). */
struct LIBARDOUR_API DrumPadConfig {
	DrumPadConfig (uint8_t trigger = no_note, uint8_t play = no_note, uint8_t choke = 0, bool mute = false)
		: trigger_note (trigger)
		, play_note (play)
		, choke_group (choke)
		, muted (mute)
	{}

	static constexpr uint8_t no_note = 0xff;

	uint8_t trigger_note; ///< incoming note that fires this pad (Live: "Receive"); no_note = unassigned
	uint8_t play_note;    ///< note delivered to the pad's chain (Live: "Play"); no_note = pass trigger_note through
	uint8_t choke_group;  ///< 0 = none, 1..16; a pad firing silences other pads in the same group
	bool    muted;

	uint8_t delivered_note () const { return play_note == no_note ? trigger_note : play_note; }
};

/** The real-time MIDI half of a Drum Rack: splits one incoming MIDI
 * stream into per-pad streams.
 *
 * - Note-on/off and polyphonic aftertouch go only to the pad whose
 *   trigger note matches, rewritten to the pad's play note.
 * - Note-offs for notes the pad is not currently sounding are dropped
 *   (e.g. after a choke), so chains never see stray note-offs.
 * - Channel-wide messages (CC, program change, channel pressure, pitch
 *   bend) and system messages go to every unmuted pad.
 * - A pad with a choke group, when fired, releases every sounding note
 *   on the other pads of that group and sends them All Sound Off
 *   (CC 120) so release tails stop too, like Live's choke.
 *
 * process() and flush() are RT-safe: no allocation, no locks. set_pads()
 * is not, and must not run concurrently with them; the owning processor
 * swaps configurations with flush() + set_pads() under its own
 * synchronisation.
 */
class LIBARDOUR_API DrumPadRouter
{
public:
	/** Receives the routed per-pad events. Called from process()/flush()
	 * on the RT thread, so implementations must be RT-safe.
	 */
	class LIBARDOUR_API Sink
	{
	public:
		virtual ~Sink () {}
		virtual void deliver (uint32_t pad, samplecnt_t time, uint8_t const* buf, uint32_t size) = 0;
	};

	static constexpr uint32_t max_pads = 128;

	DrumPadRouter ();

	/** Replace the pad layout and forget all sounding-note state.
	 * Callers should flush() first, or sounding notes will hang.
	 * @return false (and no change) if there are more than max_pads
	 * pads, two pads share a trigger note, a note is > 127, or a choke
	 * group is > 16.
	 */
	bool set_pads (std::vector<DrumPadConfig> const&);

	uint32_t             n_pads () const { return _pads.size (); }
	DrumPadConfig const& pad (uint32_t n) const { return _pads[n]; }

	/** @return pad index fired by @p note, or -1. */
	int32_t pad_for_note (uint8_t note) const { return note < 128 ? _note_to_pad[note] : -1; }

	/** Route one MIDI event. RT-safe. */
	void process (samplecnt_t time, uint8_t const* buf, uint32_t size, Sink&);

	/** Release every sounding note on every pad and reset state, e.g. on
	 * transport stop or before set_pads(). RT-safe.
	 */
	void flush (samplecnt_t time, Sink&);

	/** Number of notes pad @p n is currently sounding, summed over all
	 * channels.
	 */
	uint32_t active_voices (uint32_t n) const;

private:
	std::vector<DrumPadConfig> _pads;
	int16_t                    _note_to_pad[128];
	uint8_t                    _active[max_pads][16]; ///< sounding-note count per pad per channel

	void release (uint32_t pad, samplecnt_t time, Sink&, bool all_sound_off);
	void choke (uint8_t group, uint32_t except, samplecnt_t time, Sink&);
	void broadcast (samplecnt_t time, uint8_t const* buf, uint32_t size, Sink&);
	void reset_state ();
};

} // namespace ARDOUR
