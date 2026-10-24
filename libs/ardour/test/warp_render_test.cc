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

/* Renders a click track through warp markers with RubberBand and checks
 * where the clicks land.
 */

#include <cmath>
#include <iostream>
#include <vector>

#include <glibmm/miscutils.h>

#include <cppunit/extensions/HelperMacros.h>

#include "pbd/compose.h"
#include "pbd/xml++.h"

#include "ardour/audioregion.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/sndfilesource.h"
#include "ardour/source_factory.h"
#include "ardour/stretch.h"
#include "ardour/timefx_request.h"
#include "ardour/warp_map.h"
#include "ardour/warp_render.h"

#include "test_needing_session.h"
#include "test_util.h"

using namespace ARDOUR;

class WarpRenderTest : public TestNeedingSession
{
	CPPUNIT_TEST_SUITE (WarpRenderTest);
	CPPUNIT_TEST (uniformMapMatchesPlainStretch);
	CPPUNIT_TEST (clicksLandOnWarpedPositions);
	CPPUNIT_TEST (rendersFromOriginalAudio);
	CPPUNIT_TEST (plainStretchKeepsWarp);
	CPPUNIT_TEST (rejectsMismatchedMap);
	CPPUNIT_TEST (copiesKeepWarpSplitsDont);
	CPPUNIT_TEST (regionStateRoundTrip);
	CPPUNIT_TEST_SUITE_END ();

public:
	void setUp ();
	void tearDown ();

	void uniformMapMatchesPlainStretch ();
	void clicksLandOnWarpedPositions ();
	void rendersFromOriginalAudio ();
	void plainStretchKeepsWarp ();
	void rejectsMismatchedMap ();
	void copiesKeepWarpSplitsDont ();
	void regionStateRoundTrip ();

private:
	samplecnt_t                   _rate;
	samplecnt_t                   _length;
	std::vector<samplecnt_t>      _clicks; ///< click onsets in the source
	std::shared_ptr<Source>       _source;
	std::shared_ptr<AudioRegion>  _region;

	samplecnt_t ms (double m) const { return (samplecnt_t)llrint (m * _rate / 1000.0); }

	/** onset of the first sample louder than half the click amplitude
	 * within @p window of @p expected in @p r, or -1 */
	samplecnt_t find_onset (std::shared_ptr<AudioRegion> r, samplecnt_t expected, samplecnt_t window) const;

	/** assert every click in @p r is within tolerance of map(source onset) */
	void check_clicks (std::shared_ptr<AudioRegion> r, WarpMap const& expected, double scale = 1.0) const;
};

CPPUNIT_TEST_SUITE_REGISTRATION (WarpRenderTest);

namespace {

const float  click_amplitude = 0.8f;
const double click_freq      = 2000.0;
/* RubberBand doesn't place a stretched transient exactly. Measured here
 * (R3 engine, crisp transients, 48 kHz) a plain uniform stretch puts click
 * peaks within ~1.5 ms at 1.5x but ~7 ms early at 2.2x, and warped
 * segments behave the same as a uniform stretch at their own ratio. The
 * tests keep segment ratios below ~1.5 so a 5 ms tolerance holds; that's
 * still far smaller than the 60-150 ms marker moves they check. */
const double tolerance_ms    = 5.0;

} // namespace

void
WarpRenderTest::setUp ()
{
	TestNeedingSession::setUp ();

	_rate   = _session->sample_rate ();
	_length = 4 * _rate;

	/* a 2 ms 2 kHz burst every 500 ms, starting at 100 ms */
	std::vector<Sample> data (_length, 0.f);
	for (samplecnt_t at = ms (100); at + ms (2) < _length; at += ms (500)) {
		_clicks.push_back (at);
		for (samplecnt_t i = 0; i < ms (2); ++i) {
			data[at + i] = click_amplitude * sin (2.0 * M_PI * click_freq * i / _rate);
		}
		/* start the burst at its peak so the onset is unambiguous */
		data[at] = click_amplitude;
	}

	std::string const path = Glib::build_filename (new_test_output_dir (), "clicks.wav");
	_source = SourceFactory::createWritable (DataType::AUDIO, *_session, path, _rate);
	std::shared_ptr<SndFileSource> sfs = std::dynamic_pointer_cast<SndFileSource> (_source);
	CPPUNIT_ASSERT (sfs);
	CPPUNIT_ASSERT_EQUAL (_length, sfs->write (&data[0], _length));
	sfs->flush ();

	PBD::PropertyList plist;
	plist.add (Properties::start, timepos_t (0));
	plist.add (Properties::length, timecnt_t (_length));
	plist.add (Properties::name, std::string ("clicks"));
	_region = std::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (_source, plist));
	CPPUNIT_ASSERT (_region);
	CPPUNIT_ASSERT_EQUAL (_length, _region->unstretched_length ());
}

void
WarpRenderTest::tearDown ()
{
	_region.reset ();
	_source.reset ();
	_clicks.clear ();
	TestNeedingSession::tearDown ();
}

samplecnt_t
WarpRenderTest::find_onset (std::shared_ptr<AudioRegion> r, samplecnt_t expected, samplecnt_t window) const
{
	samplecnt_t const from = std::max<samplecnt_t> (0, expected - window);
	samplecnt_t const to   = std::min<samplecnt_t> (r->length_samples (), expected + window);
	if (to <= from) {
		return -1;
	}

	std::vector<Sample> buf (to - from);
	r->read (&buf[0], from, to - from, 0);

	/* the loudest sample: stretching smears a little energy ahead of a
	 * transient (more at larger ratios), so a threshold crossing reads
	 * early, but the click's peak stays put */
	samplecnt_t best = -1;
	float       peak = 0.5f * click_amplitude;
	for (samplecnt_t i = 0; i < to - from; ++i) {
		if (fabsf (buf[i]) > peak) {
			peak = fabsf (buf[i]);
			best = from + i;
		}
	}
	return best;
}

void
WarpRenderTest::check_clicks (std::shared_ptr<AudioRegion> r, WarpMap const& expected, double scale) const
{
	samplecnt_t const tol = ms (tolerance_ms);

	for (size_t c = 0; c < _clicks.size (); ++c) {
		samplecnt_t const want = (samplecnt_t)llrint (expected.source_to_target (_clicks[c]) * scale);
		samplecnt_t const got  = find_onset (r, want, ms (50));

		std::string const msg = string_compose ("click %1: expected at %2, found at %3 (tolerance %4)", c, want, got, tol);
		CPPUNIT_ASSERT_MESSAGE (msg, got >= 0);
		CPPUNIT_ASSERT_MESSAGE (msg, std::abs (got - want) <= tol);
	}
}

void
WarpRenderTest::uniformMapMatchesPlainStretch ()
{
	/* sanity check for the click detector: no markers, 1.5x longer */
	WarpMap m (_length, _length * 3 / 2);
	std::shared_ptr<AudioRegion> out = render_warp (_region, m);
	CPPUNIT_ASSERT (out);
	CPPUNIT_ASSERT_EQUAL (_length * 3 / 2, out->length_samples ());
	check_clicks (out, m);
}

void
WarpRenderTest::clicksLandOnWarpedPositions ()
{
	/* same overall length; click 1 pushed 100 ms late, click 5 pulled
	 * 100 ms early. Everything between stretches or squeezes. */
	WarpMap m (_length, _length);
	CPPUNIT_ASSERT (m.add_marker (_clicks[1], _clicks[1] + ms (100)));
	CPPUNIT_ASSERT (m.add_marker (_clicks[5], _clicks[5] - ms (100)));

	std::shared_ptr<AudioRegion> out = render_warp (_region, m);
	CPPUNIT_ASSERT (out);
	CPPUNIT_ASSERT_EQUAL (_length, out->length_samples ());

	/* the markers really moved things: click 1 isn't where it was */
	CPPUNIT_ASSERT (find_onset (out, _clicks[1], ms (20)) < 0);

	check_clicks (out, m);

	/* and the region remembers its warp */
	CPPUNIT_ASSERT (out->warp_map_matches ());
	CPPUNIT_ASSERT (out->warp_map ()->markers () == m.markers ());
}

void
WarpRenderTest::rendersFromOriginalAudio ()
{
	WarpMap m1 (_length, _length);
	CPPUNIT_ASSERT (m1.add_marker (_clicks[3], _clicks[3] + ms (150)));
	std::shared_ptr<AudioRegion> once = render_warp (_region, m1);
	CPPUNIT_ASSERT (once);

	/* re-warp the warped region: edit its map, render again */
	WarpMap m2 (*once->warp_map ());
	CPPUNIT_ASSERT (m2.move_marker (_clicks[3], _clicks[3] - ms (150)));
	std::shared_ptr<AudioRegion> twice = render_warp (once, m2);
	CPPUNIT_ASSERT (twice);

	/* clicks follow m2 alone, measured against the original audio:
	 * the first render's warp isn't baked in underneath */
	check_clicks (twice, m2);
}

void
WarpRenderTest::plainStretchKeepsWarp ()
{
	WarpMap m (_length, _length);
	CPPUNIT_ASSERT (m.add_marker (_clicks[2], _clicks[2] + ms (120)));
	std::shared_ptr<AudioRegion> warped = render_warp (_region, m);
	CPPUNIT_ASSERT (warped);

	/* the editor's ordinary stretch tool: 1.25x longer, no markers */
	TimeFXRequest tsr;
	tsr.time_fraction  = Temporal::ratio_t (5, 4);
	tsr.pitch_fraction = 1.0;
	tsr.opts           = default_warp_options ();
	RBStretch stretch (*_session, tsr);
	CPPUNIT_ASSERT_EQUAL (0, stretch.run (warped));
	CPPUNIT_ASSERT_EQUAL ((size_t)1, stretch.results.size ());

	std::shared_ptr<AudioRegion> longer = std::dynamic_pointer_cast<AudioRegion> (stretch.results.front ());
	CPPUNIT_ASSERT (longer);
	CPPUNIT_ASSERT_EQUAL (_length * 5 / 4, longer->length_samples ());

	/* warp kept and scaled, both in the audio and the stored map. A
	 * dropped warp would put click 0 ~13 ms early, well outside the
	 * tolerance. */
	check_clicks (longer, m, 1.25);
	CPPUNIT_ASSERT (longer->warp_map_matches ());
	CPPUNIT_ASSERT_EQUAL (_length * 5 / 4, longer->warp_map ()->target_length ());
	CPPUNIT_ASSERT_EQUAL ((_clicks[2] + ms (120)) * 5 / 4, longer->warp_map ()->markers ()[0].target);
}

void
WarpRenderTest::rejectsMismatchedMap ()
{
	WarpMap wrong (_length + 10, _length);
	CPPUNIT_ASSERT (!render_warp (_region, wrong));
	CPPUNIT_ASSERT (!render_warp (std::shared_ptr<AudioRegion> (), WarpMap (_length, _length)));
}

void
WarpRenderTest::copiesKeepWarpSplitsDont ()
{
	WarpMap m (_length, _length);
	CPPUNIT_ASSERT (m.add_marker (_clicks[4], _clicks[4] - ms (80)));
	std::shared_ptr<AudioRegion> warped = render_warp (_region, m);
	CPPUNIT_ASSERT (warped);

	std::shared_ptr<AudioRegion> copy = std::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (warped, true));
	CPPUNIT_ASSERT (copy);
	CPPUNIT_ASSERT (copy->warp_map_matches ());

	PBD::PropertyList plist;
	plist.add (Properties::length, timecnt_t (_length / 2));
	std::shared_ptr<AudioRegion> right = std::dynamic_pointer_cast<AudioRegion> (
	        RegionFactory::create (warped, timecnt_t (_length / 2), plist, true));
	CPPUNIT_ASSERT (right);
	CPPUNIT_ASSERT (!right->warp_map ());
}

void
WarpRenderTest::regionStateRoundTrip ()
{
	WarpMap m (_length, _length);
	CPPUNIT_ASSERT (m.add_marker (_clicks[6], _clicks[6] + ms (60)));
	std::shared_ptr<AudioRegion> warped = render_warp (_region, m);
	CPPUNIT_ASSERT (warped);

	XMLNode& node (warped->get_state ());
	CPPUNIT_ASSERT (node.child (WarpMap::xml_node_name));

	/* a region built from the same sources, then given the saved state */
	SourceList srcs;
	srcs.push_back (warped->source (0));
	std::shared_ptr<AudioRegion> restored = std::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (srcs, node));
	delete &node;

	CPPUNIT_ASSERT (restored);
	CPPUNIT_ASSERT (restored->warp_map ());
	CPPUNIT_ASSERT (*restored->warp_map () == *warped->warp_map ());
	CPPUNIT_ASSERT (restored->warp_map_matches ());
}
