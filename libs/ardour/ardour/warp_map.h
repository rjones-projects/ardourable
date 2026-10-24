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

#include <cstddef>
#include <map>
#include <vector>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

class XMLNode;

namespace ARDOUR {

/** A piecewise-linear time map for an audio region, in the spirit of
 * Ableton Live's warp markers.
 *
 * All positions are region-relative sample offsets: "source" is the
 * position in the unstretched audio (counted from the first sample the
 * region reads), "target" is where that sample should land in the
 * stretched output. The region's first and last samples are implicit
 * anchors at (0, 0) and (source_length, target_length); markers are the
 * user-placed interior anchors between them.
 *
 * Invariant: anchors are strictly increasing in both source and target,
 * so every segment has a positive, finite stretch ratio.
 *
 * Not RT-safe (allocates); intended for the GUI/model thread and offline
 * rendering.
 */
class LIBARDOUR_API WarpMap
{
public:
	struct Marker {
		Marker (samplecnt_t s = 0, samplecnt_t t = 0) : source (s), target (t) {}
		samplecnt_t source;
		samplecnt_t target;
		bool operator== (Marker const& o) const { return source == o.source && target == o.target; }
	};

	typedef std::vector<Marker> Markers;

	WarpMap ();
	WarpMap (samplecnt_t source_length, samplecnt_t target_length);

	samplecnt_t source_length () const { return _source_length; }
	samplecnt_t target_length () const { return _target_length; }

	/** Change the overall lengths. Interior marker targets are rescaled
	 * proportionally; markers that no longer fit strictly inside the
	 * source range, or that collapse after rounding, are dropped.
	 * @return false (and no change) if either length is < 1.
	 */
	bool set_lengths (samplecnt_t source_length, samplecnt_t target_length);

	/** Add an interior marker. Fails if @p source is not strictly inside
	 * (0, source_length), already has a marker, or if @p target would
	 * break the strictly-increasing invariant.
	 */
	bool add_marker (samplecnt_t source, samplecnt_t target);

	/** Pin @p source at the position it currently maps to, so that later
	 * moves of neighbouring markers do not drag it (Live's "insert warp
	 * marker" behaviour).
	 */
	bool add_marker_at_source (samplecnt_t source);

	/** Move the marker at @p source to @p new_target, subject to the same
	 * ordering invariant as add_marker().
	 */
	bool move_marker (samplecnt_t source, samplecnt_t new_target);

	bool remove_marker (samplecnt_t source);
	void clear_markers () { _markers.clear (); }

	Markers const& markers () const { return _markers; }
	bool empty () const { return _markers.empty (); }

	/** Number of linear segments (markers + 1). */
	size_t n_segments () const { return _markers.size () + 1; }

	/** Stretch ratio (target duration / source duration) of segment @p i,
	 * or 0 if @p i is out of range.
	 */
	double segment_ratio (size_t i) const;

	/** Map a source offset to a target offset. Values outside
	 * [0, source_length] are clamped.
	 */
	samplecnt_t source_to_target (samplecnt_t source) const;

	/** Inverse of source_to_target(). Values outside [0, target_length]
	 * are clamped.
	 */
	samplecnt_t target_to_source (samplecnt_t target) const;

	/** Build a key-frame map suitable for
	 * RubberBand::RubberBandStretcher::setKeyFrameMap(), with marker
	 * targets scaled so that the whole map spans @p output_length
	 * samples. Endpoints are not included; the stretcher's overall time
	 * ratio covers them. Entries that would become non-increasing after
	 * rounding are skipped.
	 */
	std::map<size_t, size_t> key_frames (samplecnt_t output_length) const;

	XMLNode& get_state () const;
	int      set_state (XMLNode const&);

	bool operator== (WarpMap const& o) const;
	bool operator!= (WarpMap const& o) const { return !(*this == o); }

	static const char* xml_node_name;

private:
	samplecnt_t _source_length;
	samplecnt_t _target_length;
	Markers     _markers; /* sorted by source, interior anchors only */

	Marker anchor (size_t i) const; /* 0 = start, n_segments() = end */
	bool   target_fits (size_t anchor_index_before, size_t anchor_index_after, samplecnt_t target) const;

	static samplecnt_t lerp (samplecnt_t x, samplecnt_t x0, samplecnt_t x1, samplecnt_t y0, samplecnt_t y1);
};

} // namespace ARDOUR
