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

#ifdef WAF_BUILD
#include "libardour-config.h"
#endif

#include <rubberband/RubberBandStretcher.h>

#include "pbd/compose.h"
#include "pbd/error.h"

#include "ardour/audioregion.h"
#include "ardour/session.h"
#include "ardour/stretch.h"
#include "ardour/timefx_request.h"
#include "ardour/warp_map.h"
#include "ardour/warp_render.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using namespace RubberBand;

int
ARDOUR::default_warp_options ()
{
	RubberBandStretcher::Options o = RubberBandStretcher::OptionTransientsCrisp;
#ifdef HAVE_RUBBERBAND_3_0_0
	o |= RubberBandStretcher::OptionEngineFiner;
#endif
	return (int)o;
}

std::shared_ptr<AudioRegion>
ARDOUR::render_warp (std::shared_ptr<AudioRegion> region, WarpMap const& map)
{
	return render_warp (region, map, default_warp_options (), 0);
}

std::shared_ptr<AudioRegion>
ARDOUR::render_warp (std::shared_ptr<AudioRegion> region, WarpMap const& map, int opts, Progress* progress)
{
	if (!region || region->length_samples () < 1) {
		return std::shared_ptr<AudioRegion> ();
	}

	if (map.source_length () != region->unstretched_length ()) {
		error << string_compose (_("Warp: markers describe %1 samples of audio but region \"%2\" reads %3"),
		                         map.source_length (), region->name (), region->unstretched_length ())
		      << endmsg;
		return std::shared_ptr<AudioRegion> ();
	}

	TimeFXRequest tsr;
	tsr.algorithm      = TimeFXRequest::Rubberband;
	tsr.time_fraction  = Temporal::ratio_t (map.target_length (), region->length_samples ());
	tsr.pitch_fraction = 1.0;
	tsr.opts           = opts;
	tsr.key_frames     = map.key_frames (map.target_length ());

	RBStretch stretch (region->session (), tsr);

	if (stretch.run (region, progress) || stretch.results.empty ()) {
		return std::shared_ptr<AudioRegion> ();
	}

	std::shared_ptr<AudioRegion> out = std::dynamic_pointer_cast<AudioRegion> (stretch.results.front ());
	if (!out) {
		return out;
	}

	/* RBEffect sets the new length to length * time_fraction, which is
	 * exactly the map's target length. Keep the map so the warp survives
	 * later re-renders. */
	std::shared_ptr<WarpMap> kept (new WarpMap (map));
	kept->set_lengths (out->unstretched_length (), out->length_samples ());
	out->set_warp_map (kept);

	return out;
}
