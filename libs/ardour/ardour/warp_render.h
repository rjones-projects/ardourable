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

#include <memory>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace PBD {
class Progress;
}

namespace ARDOUR {

class AudioRegion;
class WarpMap;

/** RubberBand options used by render_warp() when the caller doesn't pass
 * any: the editor's default stretch mode (crisp transients, phase lock),
 * on the R3 "finer" engine when available.
 */
LIBARDOUR_API int default_warp_options ();

/** Render @p region through @p map and return the new region, or null on
 * failure.
 *
 * @p map must describe the audio the region currently reads: its source
 * length must equal region->unstretched_length(). Its target length
 * becomes the new region's length. The render always starts from the
 * region's original (master) audio, so warping a warped region again
 * doesn't stack stretch artefacts.
 *
 * The new region is not placed in any playlist; it carries a copy of
 * @p map, so later RubberBand stretches and pitch shifts keep the warp.
 *
 * Runs offline on the calling thread and may take a while for long
 * regions; pass @p progress to follow or cancel.
 */
LIBARDOUR_API std::shared_ptr<AudioRegion> render_warp (std::shared_ptr<AudioRegion> region, WarpMap const& map, int rubberband_options, PBD::Progress* progress = 0);

LIBARDOUR_API std::shared_ptr<AudioRegion> render_warp (std::shared_ptr<AudioRegion> region, WarpMap const& map);

} // namespace ARDOUR
