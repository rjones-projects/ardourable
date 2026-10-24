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

#include <cstring>

#include "ardour/drum_pad_router.h"

using namespace ARDOUR;

namespace {

const uint8_t MIDI_NOTE_OFF       = 0x80;
const uint8_t MIDI_NOTE_ON        = 0x90;
const uint8_t MIDI_POLY_PRESSURE  = 0xA0;
const uint8_t MIDI_CONTROL        = 0xB0;
const uint8_t MIDI_SYSTEM         = 0xF0;
const uint8_t CC_ALL_SOUND_OFF    = 120;
const uint8_t CC_ALL_NOTES_OFF    = 123;
const uint8_t MAX_CHOKE_GROUP     = 16;
const uint8_t RELEASE_VELOCITY    = 64;

} // namespace

DrumPadRouter::DrumPadRouter ()
{
	/* reserve once so set_pads() never reallocates under a reader that
	 * only holds pad indices */
	_pads.reserve (max_pads);
	for (int i = 0; i < 128; ++i) {
		_note_to_pad[i] = -1;
	}
	reset_state ();
}

void
DrumPadRouter::reset_state ()
{
	memset (_active, 0, sizeof (_active));
}

bool
DrumPadRouter::set_pads (std::vector<DrumPadConfig> const& pads)
{
	if (pads.size () > max_pads) {
		return false;
	}

	int16_t map[128];
	for (int i = 0; i < 128; ++i) {
		map[i] = -1;
	}

	for (size_t n = 0; n < pads.size (); ++n) {
		DrumPadConfig const& p (pads[n]);

		if (p.choke_group > MAX_CHOKE_GROUP) {
			return false;
		}
		if (p.play_note != DrumPadConfig::no_note && p.play_note > 127) {
			return false;
		}
		if (p.trigger_note == DrumPadConfig::no_note) {
			continue; /* unassigned pad: allowed, never fires */
		}
		if (p.trigger_note > 127 || map[p.trigger_note] >= 0) {
			return false;
		}
		map[p.trigger_note] = (int16_t)n;
	}

	_pads.assign (pads.begin (), pads.end ());
	memcpy (_note_to_pad, map, sizeof (_note_to_pad));
	reset_state ();
	return true;
}

uint32_t
DrumPadRouter::active_voices (uint32_t n) const
{
	if (n >= _pads.size ()) {
		return 0;
	}
	uint32_t sum = 0;
	for (int ch = 0; ch < 16; ++ch) {
		sum += _active[n][ch];
	}
	return sum;
}

void
DrumPadRouter::release (uint32_t p, samplecnt_t time, Sink& sink, bool all_sound_off)
{
	uint8_t const note = _pads[p].delivered_note ();

	for (uint8_t ch = 0; ch < 16; ++ch) {
		if (_active[p][ch] == 0) {
			continue;
		}
		/* one note-off per sounding note: instruments that stack
		 * retriggered voices need each one released */
		uint8_t const off[3] = { (uint8_t)(MIDI_NOTE_OFF | ch), note, RELEASE_VELOCITY };
		for (uint8_t v = 0; v < _active[p][ch]; ++v) {
			sink.deliver (p, time, off, 3);
		}
		if (all_sound_off) {
			uint8_t const aso[3] = { (uint8_t)(MIDI_CONTROL | ch), CC_ALL_SOUND_OFF, 0 };
			sink.deliver (p, time, aso, 3);
		}
		_active[p][ch] = 0;
	}
}

void
DrumPadRouter::choke (uint8_t group, uint32_t except, samplecnt_t time, Sink& sink)
{
	for (uint32_t p = 0; p < _pads.size (); ++p) {
		if (p != except && _pads[p].choke_group == group) {
			release (p, time, sink, true);
		}
	}
}

void
DrumPadRouter::broadcast (samplecnt_t time, uint8_t const* buf, uint32_t size, Sink& sink)
{
	for (uint32_t p = 0; p < _pads.size (); ++p) {
		if (!_pads[p].muted) {
			sink.deliver (p, time, buf, size);
		}
	}
}

void
DrumPadRouter::process (samplecnt_t time, uint8_t const* buf, uint32_t size, Sink& sink)
{
	if (size == 0) {
		return;
	}

	uint8_t const status = buf[0];

	if (status < 0x80) {
		return; /* running status / stray data byte: not routable */
	}

	if (status >= MIDI_SYSTEM) {
		broadcast (time, buf, size, sink);
		return;
	}

	uint8_t const type = status & 0xF0;
	uint8_t const ch   = status & 0x0F;

	if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF || type == MIDI_POLY_PRESSURE) {
		if (size < 3) {
			return;
		}

		int32_t const p = pad_for_note (buf[1]);
		if (p < 0) {
			return;
		}

		DrumPadConfig const& pad (_pads[p]);
		uint8_t out[3]   = { status, pad.delivered_note (), buf[2] };
		bool const is_on = (type == MIDI_NOTE_ON && buf[2] > 0);

		if (is_on) {
			if (pad.muted) {
				return;
			}
			if (pad.choke_group) {
				choke (pad.choke_group, p, time, sink);
			}
			if (_active[p][ch] < 0xff) {
				++_active[p][ch];
			}
			sink.deliver (p, time, out, 3);
			return;
		}

		if (_active[p][ch] == 0) {
			/* not sounding (choked, flushed, or muted when it fired):
			 * drop the stale note-off / aftertouch */
			return;
		}

		if (type == MIDI_POLY_PRESSURE) {
			sink.deliver (p, time, out, 3);
			return;
		}

		/* note-off, or note-on with velocity 0 */
		--_active[p][ch];
		sink.deliver (p, time, out, 3);
		return;
	}

	if (type == MIDI_CONTROL && size >= 3 && (buf[1] == CC_ALL_SOUND_OFF || buf[1] == CC_ALL_NOTES_OFF)) {
		for (uint32_t p = 0; p < _pads.size (); ++p) {
			_active[p][ch] = 0;
		}
	}

	/* CC, program change, channel pressure, pitch bend */
	broadcast (time, buf, size, sink);
}

void
DrumPadRouter::flush (samplecnt_t time, Sink& sink)
{
	for (uint32_t p = 0; p < _pads.size (); ++p) {
		release (p, time, sink, false);
	}
	reset_state ();
}
