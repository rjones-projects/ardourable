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

#include <vector>

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include "ardour/drum_pad_router.h"

using namespace ARDOUR;

namespace {

struct Event {
	uint32_t    pad;
	samplecnt_t time;
	std::vector<uint8_t> bytes;
};

struct RecordingSink : public DrumPadRouter::Sink {
	std::vector<Event> events;
	void deliver (uint32_t pad, samplecnt_t time, uint8_t const* buf, uint32_t size)
	{
		Event e;
		e.pad   = pad;
		e.time  = time;
		e.bytes.assign (buf, buf + size);
		events.push_back (e);
	}
	void clear () { events.clear (); }
};

void
send (DrumPadRouter& r, RecordingSink& s, samplecnt_t t, uint8_t a, uint8_t b, uint8_t c)
{
	uint8_t const buf[3] = { a, b, c };
	r.process (t, buf, 3, s);
}

bool
is (Event const& e, uint32_t pad, uint8_t a, uint8_t b, uint8_t c)
{
	return e.pad == pad && e.bytes.size () == 3 && e.bytes[0] == a && e.bytes[1] == b && e.bytes[2] == c;
}

/* GM-ish layout: kick 36, snare 38, closed hat 42 + open hat 46 choked
 * together, all chains play C3 (60) like a Live Drum Rack default */
std::vector<DrumPadConfig>
kit ()
{
	std::vector<DrumPadConfig> pads;
	pads.push_back (DrumPadConfig (36, 60));
	pads.push_back (DrumPadConfig (38, 60));
	pads.push_back (DrumPadConfig (42, 60, 1));
	pads.push_back (DrumPadConfig (46, 60, 1));
	return pads;
}

} // namespace

class DrumPadRouterTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (DrumPadRouterTest);
	CPPUNIT_TEST (setPadsValidates);
	CPPUNIT_TEST (routesAndRewritesNotes);
	CPPUNIT_TEST (passThroughWhenNoPlayNote);
	CPPUNIT_TEST (dropsUnmappedNotes);
	CPPUNIT_TEST (chokeReleasesGroupMembers);
	CPPUNIT_TEST (chokeDoesNotHitOtherGroupsOrSelf);
	CPPUNIT_TEST (mutedPadIsSilent);
	CPPUNIT_TEST (channelMessagesBroadcast);
	CPPUNIT_TEST (allNotesOffResetsTracking);
	CPPUNIT_TEST (flushReleasesEverything);
	CPPUNIT_TEST (velocityZeroIsNoteOff);
	CPPUNIT_TEST (releasePadOnlyTouchesThatPad);
	CPPUNIT_TEST (adoptVoicesMovesStateSilently);
	CPPUNIT_TEST_SUITE_END ();

public:
	void setUp () {}
	void tearDown () {}

	void setPadsValidates ();
	void routesAndRewritesNotes ();
	void passThroughWhenNoPlayNote ();
	void dropsUnmappedNotes ();
	void chokeReleasesGroupMembers ();
	void chokeDoesNotHitOtherGroupsOrSelf ();
	void mutedPadIsSilent ();
	void channelMessagesBroadcast ();
	void allNotesOffResetsTracking ();
	void flushReleasesEverything ();
	void velocityZeroIsNoteOff ();
	void releasePadOnlyTouchesThatPad ();
	void adoptVoicesMovesStateSilently ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (DrumPadRouterTest);

void
DrumPadRouterTest::setPadsValidates ()
{
	DrumPadRouter r;
	CPPUNIT_ASSERT (r.set_pads (kit ()));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)4, r.n_pads ());
	CPPUNIT_ASSERT_EQUAL ((int32_t)2, r.pad_for_note (42));
	CPPUNIT_ASSERT_EQUAL ((int32_t)-1, r.pad_for_note (37));
	CPPUNIT_ASSERT_EQUAL ((int32_t)-1, r.pad_for_note (200));

	std::vector<DrumPadConfig> dup (kit ());
	dup.push_back (DrumPadConfig (36));
	CPPUNIT_ASSERT (!r.set_pads (dup));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)4, r.n_pads ()); /* unchanged */

	std::vector<DrumPadConfig> bad_group (1, DrumPadConfig (36, 60, 17));
	CPPUNIT_ASSERT (!r.set_pads (bad_group));

	std::vector<DrumPadConfig> bad_note (1, DrumPadConfig (128));
	CPPUNIT_ASSERT (!r.set_pads (bad_note));

	std::vector<DrumPadConfig> too_many (DrumPadRouter::max_pads + 1);
	CPPUNIT_ASSERT (!r.set_pads (too_many));

	/* unassigned pads are fine and never fire */
	std::vector<DrumPadConfig> empty_slots (DrumPadRouter::max_pads);
	CPPUNIT_ASSERT (r.set_pads (empty_slots));
	CPPUNIT_ASSERT_EQUAL ((int32_t)-1, r.pad_for_note (36));
}

void
DrumPadRouterTest::routesAndRewritesNotes ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 10, 0x90, 38, 100);
	send (r, s, 20, 0x80, 38, 0);

	CPPUNIT_ASSERT_EQUAL ((size_t)2, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 1, 0x90, 60, 100));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)10, s.events[0].time);
	CPPUNIT_ASSERT (is (s.events[1], 1, 0x80, 60, 0));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (1));
}

void
DrumPadRouterTest::passThroughWhenNoPlayNote ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (std::vector<DrumPadConfig> (1, DrumPadConfig (40))));

	send (r, s, 0, 0x93, 40, 90); /* channel 4 */
	CPPUNIT_ASSERT_EQUAL ((size_t)1, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 0, 0x93, 40, 90));
}

void
DrumPadRouterTest::dropsUnmappedNotes ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 37, 100);
	send (r, s, 0, 0x80, 37, 0);
	send (r, s, 0, 0x80, 36, 0); /* note-off with no matching note-on */
	send (r, s, 0, 0xA0, 36, 40); /* aftertouch on a silent pad */
	CPPUNIT_ASSERT (s.events.empty ());
}

void
DrumPadRouterTest::chokeReleasesGroupMembers ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 46, 100);   /* open hat rings */
	s.clear ();

	send (r, s, 50, 0x90, 42, 110);  /* closed hat chokes it */
	CPPUNIT_ASSERT_EQUAL ((size_t)3, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 3, 0x80, 60, 64));   /* release open hat */
	CPPUNIT_ASSERT (is (s.events[1], 3, 0xB0, 120, 0));   /* kill its tail */
	CPPUNIT_ASSERT (is (s.events[2], 2, 0x90, 60, 110));  /* then fire */
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)50, s.events[0].time);
	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (3));

	/* the open hat's own note-off arrives later and must be dropped */
	s.clear ();
	send (r, s, 80, 0x80, 46, 0);
	CPPUNIT_ASSERT (s.events.empty ());
}

void
DrumPadRouterTest::chokeDoesNotHitOtherGroupsOrSelf ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 36, 100); /* kick, no group */
	send (r, s, 0, 0x90, 42, 100); /* closed hat, group 1 */
	s.clear ();

	send (r, s, 10, 0x90, 42, 90); /* retrigger closed hat */
	CPPUNIT_ASSERT_EQUAL ((size_t)1, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 2, 0x90, 60, 90));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)2, r.active_voices (2));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, r.active_voices (0));
}

void
DrumPadRouterTest::mutedPadIsSilent ()
{
	std::vector<DrumPadConfig> pads (kit ());
	pads[0].muted = true;

	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (pads));

	send (r, s, 0, 0x90, 36, 100);
	send (r, s, 0, 0x80, 36, 0);
	send (r, s, 0, 0xB0, 1, 64);

	/* CC reaches the three unmuted pads only */
	CPPUNIT_ASSERT_EQUAL ((size_t)3, s.events.size ());
	for (size_t i = 0; i < s.events.size (); ++i) {
		CPPUNIT_ASSERT (s.events[i].pad != 0);
		CPPUNIT_ASSERT_EQUAL ((uint8_t)0xB0, s.events[i].bytes[0]);
	}
}

void
DrumPadRouterTest::channelMessagesBroadcast ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0xE0, 0x00, 0x50); /* pitch bend */
	CPPUNIT_ASSERT_EQUAL ((size_t)4, s.events.size ());

	s.clear ();
	uint8_t const pc[2] = { 0xC0, 5 }; /* program change: 2 bytes */
	r.process (0, pc, 2, s);
	CPPUNIT_ASSERT_EQUAL ((size_t)4, s.events.size ());
	CPPUNIT_ASSERT_EQUAL ((size_t)2, s.events[3].bytes.size ());

	s.clear ();
	uint8_t const clock[1] = { 0xF8 };
	r.process (0, clock, 1, s);
	CPPUNIT_ASSERT_EQUAL ((size_t)4, s.events.size ());

	s.clear ();
	uint8_t const stray[2] = { 0x40, 0x40 }; /* data byte, no status */
	r.process (0, stray, 2, s);
	r.process (0, stray, 0, s);
	CPPUNIT_ASSERT (s.events.empty ());
}

void
DrumPadRouterTest::allNotesOffResetsTracking ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 36, 100);
	send (r, s, 0, 0x91, 38, 100); /* other channel */
	send (r, s, 0, 0xB0, 123, 0);  /* all notes off, channel 1 only */

	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (0));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, r.active_voices (1));

	s.clear ();
	send (r, s, 0, 0x80, 36, 0); /* now stale */
	CPPUNIT_ASSERT (s.events.empty ());
}

void
DrumPadRouterTest::flushReleasesEverything ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 36, 100);
	send (r, s, 0, 0x90, 36, 100); /* stacked retrigger */
	send (r, s, 0, 0x92, 38, 100);
	s.clear ();

	r.flush (99, s);
	CPPUNIT_ASSERT_EQUAL ((size_t)3, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 0, 0x80, 60, 64));
	CPPUNIT_ASSERT (is (s.events[1], 0, 0x80, 60, 64));
	CPPUNIT_ASSERT (is (s.events[2], 1, 0x82, 60, 64));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)99, s.events[2].time);

	for (uint32_t p = 0; p < r.n_pads (); ++p) {
		CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (p));
	}
}

void
DrumPadRouterTest::velocityZeroIsNoteOff ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 36, 100);
	send (r, s, 5, 0x90, 36, 0);

	CPPUNIT_ASSERT_EQUAL ((size_t)2, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[1], 0, 0x90, 60, 0));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (0));
}

void
DrumPadRouterTest::releasePadOnlyTouchesThatPad ()
{
	DrumPadRouter r;
	RecordingSink s;
	CPPUNIT_ASSERT (r.set_pads (kit ()));

	send (r, s, 0, 0x90, 36, 100);
	send (r, s, 0, 0x90, 38, 100);
	s.clear ();

	r.release_pad (0, 7, s);
	CPPUNIT_ASSERT_EQUAL ((size_t)1, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 0, 0x80, 60, 64));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, r.active_voices (0));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, r.active_voices (1));

	s.clear ();
	r.release_pad (99, 0, s); /* out of range: no-op */
	CPPUNIT_ASSERT (s.events.empty ());
}

void
DrumPadRouterTest::adoptVoicesMovesStateSilently ()
{
	DrumPadRouter before;
	DrumPadRouter after;
	RecordingSink s;
	CPPUNIT_ASSERT (before.set_pads (kit ()));

	/* new layout: snare moved to index 0 */
	std::vector<DrumPadConfig> pads;
	pads.push_back (DrumPadConfig (38, 60));
	pads.push_back (DrumPadConfig (36, 60));
	CPPUNIT_ASSERT (after.set_pads (pads));

	send (before, s, 0, 0x91, 38, 100); /* snare on channel 2 */
	s.clear ();

	after.adopt_voices (before, 1, 0);
	CPPUNIT_ASSERT (s.events.empty ());
	CPPUNIT_ASSERT_EQUAL ((uint32_t)0, before.active_voices (1));
	CPPUNIT_ASSERT_EQUAL ((uint32_t)1, after.active_voices (0));

	/* the held note's note-off now gets through the new router */
	send (after, s, 0, 0x81, 38, 0);
	CPPUNIT_ASSERT_EQUAL ((size_t)1, s.events.size ());
	CPPUNIT_ASSERT (is (s.events[0], 0, 0x81, 60, 0));

	after.adopt_voices (before, 50, 0); /* out of range: no-op */
	after.adopt_voices (before, 0, 50);
}
