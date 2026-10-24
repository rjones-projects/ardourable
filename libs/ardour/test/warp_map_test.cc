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

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include "pbd/xml++.h"

#include "ardour/warp_map.h"

using namespace ARDOUR;

class WarpMapTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (WarpMapTest);
	CPPUNIT_TEST (uniformMapping);
	CPPUNIT_TEST (piecewiseMapping);
	CPPUNIT_TEST (inverseMapping);
	CPPUNIT_TEST (rejectsInvalidMarkers);
	CPPUNIT_TEST (moveAndRemove);
	CPPUNIT_TEST (pinAtSource);
	CPPUNIT_TEST (segmentRatios);
	CPPUNIT_TEST (keyFramesScaleToOutput);
	CPPUNIT_TEST (setLengthsRescales);
	CPPUNIT_TEST (stateRoundTrip);
	CPPUNIT_TEST (badStateLeavesMapUntouched);
	CPPUNIT_TEST_SUITE_END ();

public:
	void setUp () {}
	void tearDown () {}

	void uniformMapping ();
	void piecewiseMapping ();
	void inverseMapping ();
	void rejectsInvalidMarkers ();
	void moveAndRemove ();
	void pinAtSource ();
	void segmentRatios ();
	void keyFramesScaleToOutput ();
	void setLengthsRescales ();
	void stateRoundTrip ();
	void badStateLeavesMapUntouched ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (WarpMapTest);

void
WarpMapTest::uniformMapping ()
{
	WarpMap m (1000, 2000);
	CPPUNIT_ASSERT (m.empty ());
	CPPUNIT_ASSERT_EQUAL ((size_t)1, m.n_segments ());
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)0, m.source_to_target (0));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1000, m.source_to_target (500));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)2000, m.source_to_target (1000));
	/* clamped outside the region */
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)0, m.source_to_target (-10));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)2000, m.source_to_target (5000));
}

void
WarpMapTest::piecewiseMapping ()
{
	/* first quarter of the audio fills the first half of the output */
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 500));

	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)250, m.source_to_target (125));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)500, m.source_to_target (250));
	/* second segment: 750 source samples into 500 target samples */
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)750, m.source_to_target (625));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1000, m.source_to_target (1000));
}

void
WarpMapTest::inverseMapping ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 500));
	CPPUNIT_ASSERT (m.add_marker (800, 900));

	for (samplecnt_t s = 0; s <= 1000; s += 50) {
		samplecnt_t const t = m.source_to_target (s);
		/* round trip within rounding of the steepest segment */
		samplecnt_t const back = m.target_to_source (t);
		CPPUNIT_ASSERT (back >= s - 2 && back <= s + 2);
	}
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)250, m.target_to_source (500));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)800, m.target_to_source (900));
}

void
WarpMapTest::rejectsInvalidMarkers ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 500));

	CPPUNIT_ASSERT (!m.add_marker (0, 10));      /* at implicit start anchor */
	CPPUNIT_ASSERT (!m.add_marker (1000, 990));  /* at implicit end anchor */
	CPPUNIT_ASSERT (!m.add_marker (-5, 10));     /* outside */
	CPPUNIT_ASSERT (!m.add_marker (250, 600));   /* duplicate source */
	CPPUNIT_ASSERT (!m.add_marker (500, 400));   /* would run backwards past (250,500) */
	CPPUNIT_ASSERT (!m.add_marker (500, 500));   /* zero-length target segment */
	CPPUNIT_ASSERT (!m.add_marker (100, 0));     /* collides with start */
	CPPUNIT_ASSERT (!m.add_marker (900, 1000));  /* collides with end */
	CPPUNIT_ASSERT_EQUAL ((size_t)1, m.markers ().size ());

	CPPUNIT_ASSERT (m.add_marker (100, 50));     /* before existing: fine */
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)100, m.markers ()[0].source);
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)250, m.markers ()[1].source);
}

void
WarpMapTest::moveAndRemove ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 250));
	CPPUNIT_ASSERT (m.add_marker (500, 500));
	CPPUNIT_ASSERT (m.add_marker (750, 750));

	CPPUNIT_ASSERT (m.move_marker (500, 300));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)300, m.markers ()[1].target);

	CPPUNIT_ASSERT (!m.move_marker (500, 250));  /* onto left neighbour */
	CPPUNIT_ASSERT (!m.move_marker (500, 800));  /* past right neighbour */
	CPPUNIT_ASSERT (!m.move_marker (499, 300));  /* no such marker */
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)300, m.markers ()[1].target);

	CPPUNIT_ASSERT (m.remove_marker (500));
	CPPUNIT_ASSERT (!m.remove_marker (500));
	CPPUNIT_ASSERT_EQUAL ((size_t)2, m.markers ().size ());

	m.clear_markers ();
	CPPUNIT_ASSERT (m.empty ());
}

void
WarpMapTest::pinAtSource ()
{
	WarpMap m (1000, 2000);
	CPPUNIT_ASSERT (m.add_marker_at_source (400));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)800, m.markers ()[0].target);

	/* moving the pinned marker leaves the mapping either side linear */
	CPPUNIT_ASSERT (m.move_marker (400, 1000));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)500, m.source_to_target (200));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1500, m.source_to_target (700));
}

void
WarpMapTest::segmentRatios ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 500));
	CPPUNIT_ASSERT_EQUAL ((size_t)2, m.n_segments ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (2.0, m.segment_ratio (0), 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (500.0 / 750.0, m.segment_ratio (1), 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.0, m.segment_ratio (2), 1e-9);
}

void
WarpMapTest::keyFramesScaleToOutput ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.key_frames (2000).empty ());

	CPPUNIT_ASSERT (m.add_marker (250, 500));
	CPPUNIT_ASSERT (m.add_marker (600, 700));

	std::map<size_t, size_t> kf = m.key_frames (2000);
	CPPUNIT_ASSERT_EQUAL ((size_t)2, kf.size ());
	CPPUNIT_ASSERT_EQUAL ((size_t)1000, kf[250]);
	CPPUNIT_ASSERT_EQUAL ((size_t)1400, kf[600]);

	/* scaled down so far that markers collide: later one is skipped */
	kf = m.key_frames (2);
	CPPUNIT_ASSERT_EQUAL ((size_t)1, kf.size ());
	CPPUNIT_ASSERT_EQUAL ((size_t)1, kf[250]);

	CPPUNIT_ASSERT (m.key_frames (0).empty ());
}

void
WarpMapTest::setLengthsRescales ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (250, 500));
	CPPUNIT_ASSERT (m.add_marker (900, 950));

	CPPUNIT_ASSERT (!m.set_lengths (0, 100));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1000, m.source_length ());

	/* double the output: targets scale, sources stay */
	CPPUNIT_ASSERT (m.set_lengths (1000, 2000));
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1000, m.markers ()[0].target);
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)1900, m.markers ()[1].target);

	/* trim the source: the marker past the new end is dropped */
	CPPUNIT_ASSERT (m.set_lengths (800, 2000));
	CPPUNIT_ASSERT_EQUAL ((size_t)1, m.markers ().size ());
	CPPUNIT_ASSERT_EQUAL ((samplecnt_t)250, m.markers ()[0].source);
}

void
WarpMapTest::stateRoundTrip ()
{
	WarpMap m (48000, 96000);
	CPPUNIT_ASSERT (m.add_marker (12000, 30000));
	CPPUNIT_ASSERT (m.add_marker (36000, 70000));

	XMLNode& node (m.get_state ());
	CPPUNIT_ASSERT_EQUAL (std::string (WarpMap::xml_node_name), node.name ());

	WarpMap restored;
	CPPUNIT_ASSERT_EQUAL (0, restored.set_state (node));
	CPPUNIT_ASSERT (restored == m);

	delete &node;
}

void
WarpMapTest::badStateLeavesMapUntouched ()
{
	WarpMap m (1000, 1000);
	CPPUNIT_ASSERT (m.add_marker (500, 500));
	WarpMap const before (m);

	XMLNode wrong_name ("NotAWarpMap");
	CPPUNIT_ASSERT_EQUAL (-1, m.set_state (wrong_name));
	CPPUNIT_ASSERT (m == before);

	XMLNode backwards (WarpMap::xml_node_name);
	backwards.set_property ("source-length", (samplecnt_t)1000);
	backwards.set_property ("target-length", (samplecnt_t)1000);
	XMLNode* a = backwards.add_child ("Marker");
	a->set_property ("source", (samplecnt_t)200);
	a->set_property ("target", (samplecnt_t)600);
	XMLNode* b = backwards.add_child ("Marker");
	b->set_property ("source", (samplecnt_t)400);
	b->set_property ("target", (samplecnt_t)300); /* runs backwards */

	CPPUNIT_ASSERT_EQUAL (-1, m.set_state (backwards));
	CPPUNIT_ASSERT (m == before);
}
