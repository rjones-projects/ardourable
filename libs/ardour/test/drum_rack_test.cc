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

/* Runs a DrumRack directly (outside a route) with ACE Reasonable Synth on
 * each pad, and checks what comes out. Needs the bundled LV2 plugins on
 * the search path: run with the environment from ardev_common_waf.sh.
 */

#include <cmath>
#include <memory>

#include <cppunit/extensions/HelperMacros.h>

#include <glibmm/timer.h>

#include "pbd/xml++.h"

#include "temporal/tempo.h"

#include "ardour/audio_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/buffer_set.h"
#include "ardour/drum_rack.h"
#include "ardour/lua_api.h"
#include "ardour/midi_buffer.h"
#include "ardour/midi_track.h"
#include "ardour/plugin.h"
#include "ardour/process_thread.h"
#include "ardour/session.h"

#include "test_needing_session.h"

using namespace ARDOUR;

namespace {

const char* const RSYNTH_URI = "https://community.ardour.org/node/7596";

} // namespace

class DrumRackTest : public TestNeedingSession
{
	CPPUNIT_TEST_SUITE (DrumRackTest);
	CPPUNIT_TEST (unmappedNoteIsSilent);
	CPPUNIT_TEST (padGainZeroIsSilent);
	CPPUNIT_TEST (mappedNoteSounds);
	CPPUNIT_TEST (chokeSilencesGroup);
	CPPUNIT_TEST (noChokeWithoutGroup);
	CPPUNIT_TEST (heldNoteSurvivesGainChange);
	CPPUNIT_TEST (playNoteChangeReleasesHeldNote);
	CPPUNIT_TEST (bypassIsSilent);
	CPPUNIT_TEST (rejectsDuplicateTriggerNote);
	CPPUNIT_TEST (stateRoundTrip);
	CPPUNIT_TEST (routeHostsAndRestoresRack);
	CPPUNIT_TEST_SUITE_END ();

public:
	void setUp ();
	void tearDown ();

	void unmappedNoteIsSilent ();
	void padGainZeroIsSilent ();
	void mappedNoteSounds ();
	void chokeSilencesGroup ();
	void noChokeWithoutGroup ();
	void heldNoteSurvivesGainChange ();
	void playNoteChangeReleasesHeldNote ();
	void bypassIsSilent ();
	void rejectsDuplicateTriggerNote ();
	void stateRoundTrip ();
	void routeHostsAndRestoresRack ();

private:
	std::shared_ptr<Plugin>   synth ();
	std::shared_ptr<DrumRack> rack ();
	void                      note_on (uint8_t note);
	void                      note_off (uint8_t note);
	int                       cycles_for (double seconds) const;
	/** run @p cycles blocks; return the peak of the last one */
	float                     run (DrumRack&, int cycles = 1);

	ProcessThread* _pt;
	BufferSet*     _bufs;
	pframes_t      _nframes;
	samplepos_t    _pos;
};

CPPUNIT_TEST_SUITE_REGISTRATION (DrumRackTest);

void
DrumRackTest::setUp ()
{
	TestNeedingSession::setUp ();

	/* this test thread stands in for a process thread */
	_pt = new ProcessThread ();
	_pt->get_buffers ();
	Temporal::TempoMap::update_thread_tempo_map ();

	_nframes = _session->get_block_size ();
	_pos     = 0;

	AudioEngine* e = AudioEngine::instance ();
	_bufs = new BufferSet ();
	_bufs->ensure_buffers (DataType::AUDIO, 2, e->raw_buffer_size (DataType::AUDIO) / sizeof (Sample));
	_bufs->ensure_buffers (DataType::MIDI, 1, e->raw_buffer_size (DataType::MIDI));
}

void
DrumRackTest::tearDown ()
{
	delete _bufs;
	_pt->drop_buffers ();
	delete _pt;
	TestNeedingSession::tearDown ();
}

std::shared_ptr<Plugin>
DrumRackTest::synth ()
{
	PluginInfoPtr pip = LuaAPI::new_plugin_info (RSYNTH_URI, LV2);
	CPPUNIT_ASSERT_MESSAGE ("ACE Reasonable Synth not found; is ARDOUR_DLL_PATH set?", pip);
	std::shared_ptr<Plugin> p = pip->load (*_session);
	CPPUNIT_ASSERT (p);
	return p;
}

std::shared_ptr<DrumRack>
DrumRackTest::rack ()
{
	std::shared_ptr<DrumRack> r (new DrumRack (*_session));
	ChanCount out;
	CPPUNIT_ASSERT (r->can_support_io_configuration (ChanCount (DataType::MIDI, 1), out));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)2, out.n_audio ());
	CPPUNIT_ASSERT (r->configure_io (ChanCount (DataType::MIDI, 1), out));
	r->set_block_size (_nframes);
	r->activate ();
	return r;
}

void
DrumRackTest::note_on (uint8_t note)
{
	uint8_t const ev[3] = { 0x90, note, 100 };
	_bufs->get_midi (0).push_back (0, Evoral::MIDI_EVENT, 3, ev);
}

float
DrumRackTest::run (DrumRack& r, int cycles)
{
	float peak = 0;
	for (int c = 0; c < cycles; ++c) {
		_bufs->set_count (ChanCount (DataType::MIDI, 1));
		r.run (*_bufs, _pos, _pos + _nframes, 1.0, _nframes, true);
		_pos += _nframes;

		CPPUNIT_ASSERT_EQUAL ((uint32_t)2, _bufs->count ().n_audio ());

		peak = 0;
		for (uint32_t ch = 0; ch < 2; ++ch) {
			Sample const* d = _bufs->get_audio (ch).data ();
			for (pframes_t i = 0; i < _nframes; ++i) {
				peak = std::max (peak, fabsf (d[i]));
			}
		}
		/* input events are consumed once */
		_bufs->get_midi (0).clear ();
	}
	return peak;
}

void
DrumRackTest::unmappedNoteIsSilent ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));

	note_on (40);
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, 4));
}

void
DrumRackTest::padGainZeroIsSilent ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));
	CPPUNIT_ASSERT (r->set_pad_gain (0, 0.f));

	note_on (36);
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, 4));
}

void
DrumRackTest::mappedNoteSounds ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));
	CPPUNIT_ASSERT_EQUAL ((int32_t)1, r->add_pad (DrumPadConfig (38, 60), synth ()));
	CPPUNIT_ASSERT (r->set_pad_gain (1, 0.f));

	/* pad 1 is silent, so any sound proves note 36 reached pad 0 only */
	note_on (38);
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, 4));

	note_on (36);
	CPPUNIT_ASSERT (run (*r, 4) > 0.01f);
}

void
DrumRackTest::chokeSilencesGroup ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (46, 60, 1), synth ())); /* open hat */
	CPPUNIT_ASSERT_EQUAL ((int32_t)1, r->add_pad (DrumPadConfig (42, 60, 1), synth ())); /* closed hat */
	CPPUNIT_ASSERT (r->set_pad_gain (1, 0.f));

	note_on (46);
	CPPUNIT_ASSERT (run (*r, 4) > 0.01f);

	/* closed hat chokes the held open hat: All Sound Off cuts it dead */
	note_on (42);
	run (*r, 1);
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, 2));
}

void
DrumRackTest::noChokeWithoutGroup ()
{
	/* control for chokeSilencesGroup: same notes, no choke group */
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (46, 60), synth ()));
	CPPUNIT_ASSERT_EQUAL ((int32_t)1, r->add_pad (DrumPadConfig (42, 60), synth ()));
	CPPUNIT_ASSERT (r->set_pad_gain (1, 0.f));

	note_on (46);
	CPPUNIT_ASSERT (run (*r, 4) > 0.01f);

	note_on (42);
	run (*r, 1);
	CPPUNIT_ASSERT (run (*r, 2) > 0.01f);
}

int
DrumRackTest::cycles_for (double seconds) const
{
	return (int)ceil (seconds * _session->sample_rate () / _nframes);
}

void
DrumRackTest::note_off (uint8_t note)
{
	uint8_t const ev[3] = { 0x80, note, 0 };
	_bufs->get_midi (0).push_back (0, Evoral::MIDI_EVENT, 3, ev);
}

/* Reasonable Synth's envelope: 5 ms attack, 800 ms decay, 100 ms release.
 * A held note is still audible ~200 ms in; a released one is not.
 */
void
DrumRackTest::heldNoteSurvivesGainChange ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));

	note_on (36);
	CPPUNIT_ASSERT (run (*r, 2) > 0.01f);

	/* publishes a new pad set; the held note must carry over, not be
	 * released (a release would be silent well within 200 ms) */
	CPPUNIT_ASSERT (r->set_pad_gain (0, 0.5f));
	CPPUNIT_ASSERT (run (*r, cycles_for (0.2)) > 0.005f);

	/* and its note-off must still get through the new router */
	note_off (36);
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, cycles_for (0.2)));
}

void
DrumRackTest::playNoteChangeReleasesHeldNote ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));

	note_on (36);
	CPPUNIT_ASSERT (run (*r, 2) > 0.01f);

	/* the plugin was told to play 60; once it should play 62 instead, the
	 * old note can't be matched to a later note-off, so it is released */
	CPPUNIT_ASSERT (r->set_pad_config (0, DrumPadConfig (36, 62)));
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, cycles_for (0.2)));
}

void
DrumRackTest::bypassIsSilent ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));

	note_on (36);
	CPPUNIT_ASSERT (run (*r, 4) > 0.01f);

	r->deactivate ();
	CPPUNIT_ASSERT_EQUAL (0.f, run (*r, 2));
}

void
DrumRackTest::rejectsDuplicateTriggerNote ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth ()));
	CPPUNIT_ASSERT_EQUAL ((int32_t)-1, r->add_pad (DrumPadConfig (36, 61), synth ()));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, r->n_pads ());

	CPPUNIT_ASSERT_EQUAL ((int32_t)1, r->add_pad (DrumPadConfig (38, 60), synth ()));
	CPPUNIT_ASSERT (!r->set_pad_config (1, DrumPadConfig (36, 60)));
	CPPUNIT_ASSERT (!r->remove_pad (5));
}

void
DrumRackTest::stateRoundTrip ()
{
	std::shared_ptr<DrumRack> r = rack ();
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth (), "Kick"));
	CPPUNIT_ASSERT_EQUAL ((int32_t)1, r->add_pad (DrumPadConfig (42, DrumPadConfig::no_note, 3, true), synth (), "Hat"));
	CPPUNIT_ASSERT_EQUAL ((int32_t)2, r->add_pad (DrumPadConfig (50), std::shared_ptr<Plugin> (), "Empty"));
	CPPUNIT_ASSERT (r->set_pad_gain (1, 0.25f));

	XMLNode& node (r->get_state ());
	std::string type;
	CPPUNIT_ASSERT (node.get_property ("type", type));
	CPPUNIT_ASSERT_EQUAL (std::string (DrumRack::type_name), type);

	DrumRack restored (*_session);
	CPPUNIT_ASSERT_EQUAL (0, restored.set_state (node, PBD::Stateful::current_state_version));
	delete &node;

	CPPUNIT_ASSERT_EQUAL ((uint32_t)3, restored.n_pads ());

	DrumRack::PadInfo a, b;
	for (uint32_t n = 0; n < 3; ++n) {
		CPPUNIT_ASSERT (r->pad_info (n, a));
		CPPUNIT_ASSERT (restored.pad_info (n, b));
		CPPUNIT_ASSERT (a.id == b.id);
		CPPUNIT_ASSERT_EQUAL (a.name, b.name);
		CPPUNIT_ASSERT_EQUAL (a.config.trigger_note, b.config.trigger_note);
		CPPUNIT_ASSERT_EQUAL (a.config.play_note, b.config.play_note);
		CPPUNIT_ASSERT_EQUAL (a.config.choke_group, b.config.choke_group);
		CPPUNIT_ASSERT_EQUAL (a.config.muted, b.config.muted);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (a.gain, b.gain, 1e-6);
		CPPUNIT_ASSERT_EQUAL ((bool)a.plugin, (bool)b.plugin);
		if (a.plugin) {
			CPPUNIT_ASSERT_EQUAL (a.plugin->unique_id (), b.plugin->unique_id ());
			/* a fresh instance, not the original */
			CPPUNIT_ASSERT (a.plugin != b.plugin);
		}
		CPPUNIT_ASSERT (!b.unavailable);
	}
}

namespace {

std::shared_ptr<DrumRack>
find_rack (std::shared_ptr<Route> route)
{
	for (uint32_t n = 0;; ++n) {
		std::shared_ptr<Processor> p = route->nth_processor (n);
		if (!p) {
			return std::shared_ptr<DrumRack> ();
		}
		if (std::shared_ptr<DrumRack> r = std::dynamic_pointer_cast<DrumRack> (p)) {
			return r;
		}
	}
}

} // namespace

void
DrumRackTest::routeHostsAndRestoresRack ()
{
	std::list<std::shared_ptr<MidiTrack> > tracks = _session->new_midi_track (
	        ChanCount (DataType::MIDI, 1), ChanCount (DataType::AUDIO, 2), false,
	        PluginInfoPtr (), 0, std::shared_ptr<RouteGroup> (), 1, "Drums",
	        PresentationInfo::max_order, Normal, false);
	CPPUNIT_ASSERT_EQUAL ((size_t)1, tracks.size ());
	std::shared_ptr<Route> route = tracks.front ();

	std::shared_ptr<DrumRack> r (new DrumRack (*_session));
	CPPUNIT_ASSERT_EQUAL ((int32_t)0, r->add_pad (DrumPadConfig (36, 60), synth (), "Kick"));

	Route::ProcessorStreams err;
	CPPUNIT_ASSERT_EQUAL (0, route->add_processor_by_index (r, 0, &err, true));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, r->input_streams ().n_midi ());
	CPPUNIT_ASSERT_EQUAL ((uint32_t)2, r->output_streams ().n_audio ());

	/* let the engine's own process threads run the rack for a while */
	Glib::usleep (200000);
	CPPUNIT_ASSERT (r->active ());

	/* drop it, then rebuild it from the route's saved state: this goes
	 * through Route::set_processor_state()'s "drumrack" branch */
	XMLNode& state (route->get_state ());
	PBD::ID const rack_id = r->id ();
	CPPUNIT_ASSERT_EQUAL (0, route->remove_processor (r, 0, true));
	r.reset ();
	CPPUNIT_ASSERT (!find_rack (route));

	CPPUNIT_ASSERT_EQUAL (0, route->set_state (state, PBD::Stateful::current_state_version));
	delete &state;

	std::shared_ptr<DrumRack> restored = find_rack (route);
	CPPUNIT_ASSERT (restored);
	CPPUNIT_ASSERT (restored->id () == rack_id);
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, restored->n_pads ());

	DrumRack::PadInfo info;
	CPPUNIT_ASSERT (restored->pad_info (0, info));
	CPPUNIT_ASSERT_EQUAL (std::string ("Kick"), info.name);
	CPPUNIT_ASSERT (info.plugin);

	Glib::usleep (100000);
	CPPUNIT_ASSERT (restored->active ());
}
