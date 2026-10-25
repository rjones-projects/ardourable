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

#include <algorithm>
#include <cmath>
#include <sstream>

#include "pbd/xml++.h"

#include "ardour/warp_map.h"

using namespace ARDOUR;

const char* WarpMap::xml_node_name = "WarpMap";

namespace {

struct SourceLess {
	bool operator() (WarpMap::Marker const& m, samplecnt_t s) const { return m.source < s; }
	bool operator() (samplecnt_t s, WarpMap::Marker const& m) const { return s < m.source; }
};

struct TargetLess {
	bool operator() (samplecnt_t t, WarpMap::Marker const& m) const { return t < m.target; }
};

} // namespace

WarpMap::WarpMap ()
	: _source_length (1)
	, _target_length (1)
{
}

WarpMap::WarpMap (samplecnt_t source_length, samplecnt_t target_length)
	: _source_length (std::max<samplecnt_t> (1, source_length))
	, _target_length (std::max<samplecnt_t> (1, target_length))
{
}

WarpMap::Marker
WarpMap::anchor (size_t i) const
{
	if (i == 0) {
		return Marker (0, 0);
	}
	if (i > _markers.size ()) {
		return Marker (_source_length, _target_length);
	}
	return _markers[i - 1];
}

bool
WarpMap::target_fits (size_t before, size_t after, samplecnt_t target) const
{
	return target > anchor (before).target && target < anchor (after).target;
}

samplecnt_t
WarpMap::lerp (samplecnt_t x, samplecnt_t x0, samplecnt_t x1, samplecnt_t y0, samplecnt_t y1)
{
	if (x1 == x0) {
		return y0;
	}
	double const f = (double)(x - x0) / (double)(x1 - x0);
	return y0 + (samplecnt_t)llrint (f * (double)(y1 - y0));
}

bool
WarpMap::set_lengths (samplecnt_t source_length, samplecnt_t target_length)
{
	if (source_length < 1 || target_length < 1) {
		return false;
	}

	double const scale = (double)target_length / (double)_target_length;

	Markers     kept;
	samplecnt_t prev_target = 0;

	for (Markers::const_iterator m = _markers.begin (); m != _markers.end (); ++m) {
		if (m->source >= source_length) {
			break; /* sorted: everything after is out of range too */
		}
		samplecnt_t const t = (samplecnt_t)llrint ((double)m->target * scale);
		if (t <= prev_target || t >= target_length) {
			continue;
		}
		kept.push_back (Marker (m->source, t));
		prev_target = t;
	}

	_source_length = source_length;
	_target_length = target_length;
	_markers.swap (kept);
	return true;
}

bool
WarpMap::add_marker (samplecnt_t source, samplecnt_t target)
{
	if (source <= 0 || source >= _source_length) {
		return false;
	}

	Markers::iterator pos = std::lower_bound (_markers.begin (), _markers.end (), source, SourceLess ());
	if (pos != _markers.end () && pos->source == source) {
		return false;
	}

	size_t const idx = pos - _markers.begin ();
	/* the new marker would become anchor idx + 1; its neighbours are
	 * anchors idx (before) and idx + 1 (currently after) */
	if (!target_fits (idx, idx + 1, target)) {
		return false;
	}

	_markers.insert (pos, Marker (source, target));
	return true;
}

bool
WarpMap::add_marker_at_source (samplecnt_t source)
{
	return add_marker (source, source_to_target (source));
}

bool
WarpMap::move_marker (samplecnt_t source, samplecnt_t new_target)
{
	Markers::iterator pos = std::lower_bound (_markers.begin (), _markers.end (), source, SourceLess ());
	if (pos == _markers.end () || pos->source != source) {
		return false;
	}

	size_t const k = pos - _markers.begin (); /* this marker is anchor k + 1 */
	if (!target_fits (k, k + 2, new_target)) {
		return false;
	}

	pos->target = new_target;
	return true;
}

bool
WarpMap::remove_marker (samplecnt_t source)
{
	Markers::iterator pos = std::lower_bound (_markers.begin (), _markers.end (), source, SourceLess ());
	if (pos == _markers.end () || pos->source != source) {
		return false;
	}
	_markers.erase (pos);
	return true;
}

double
WarpMap::segment_ratio (size_t i) const
{
	if (i >= n_segments ()) {
		return 0;
	}
	Marker const a = anchor (i);
	Marker const b = anchor (i + 1);
	return (double)(b.target - a.target) / (double)(b.source - a.source);
}

samplecnt_t
WarpMap::source_to_target (samplecnt_t source) const
{
	if (source <= 0) {
		return 0;
	}
	if (source >= _source_length) {
		return _target_length;
	}

	/* number of markers at or before @p source */
	size_t const i = std::upper_bound (_markers.begin (), _markers.end (), source, SourceLess ()) - _markers.begin ();
	Marker const a = anchor (i);
	Marker const b = anchor (i + 1);
	return lerp (source, a.source, b.source, a.target, b.target);
}

samplecnt_t
WarpMap::target_to_source (samplecnt_t target) const
{
	if (target <= 0) {
		return 0;
	}
	if (target >= _target_length) {
		return _source_length;
	}

	size_t const i = std::upper_bound (_markers.begin (), _markers.end (), target, TargetLess ()) - _markers.begin ();
	Marker const a = anchor (i);
	Marker const b = anchor (i + 1);
	return lerp (target, a.target, b.target, a.source, b.source);
}

std::map<size_t, size_t>
WarpMap::key_frames (samplecnt_t output_length) const
{
	std::map<size_t, size_t> kf;

	if (output_length < 1 || _markers.empty ()) {
		return kf;
	}

	double const scale       = (double)output_length / (double)_target_length;
	samplecnt_t  prev_target = 0;

	for (Markers::const_iterator m = _markers.begin (); m != _markers.end (); ++m) {
		samplecnt_t const t = (samplecnt_t)llrint ((double)m->target * scale);
		if (t <= prev_target || t >= output_length) {
			continue;
		}
		kf[(size_t)m->source] = (size_t)t;
		prev_target = t;
	}

	return kf;
}

XMLNode&
WarpMap::get_state () const
{
	XMLNode* node = new XMLNode (xml_node_name);
	node->set_property ("source-length", _source_length);
	node->set_property ("target-length", _target_length);

	for (Markers::const_iterator m = _markers.begin (); m != _markers.end (); ++m) {
		XMLNode* child = node->add_child ("Marker");
		child->set_property ("source", m->source);
		child->set_property ("target", m->target);
	}

	return *node;
}

int
WarpMap::set_state (XMLNode const& node)
{
	if (node.name () != xml_node_name) {
		return -1;
	}

	samplecnt_t sl;
	samplecnt_t tl;

	if (!node.get_property ("source-length", sl) || !node.get_property ("target-length", tl) || sl < 1 || tl < 1) {
		return -1;
	}

	/* build into a scratch map so a bad marker leaves *this untouched */
	WarpMap tmp (sl, tl);

	XMLNodeList const& children (node.children ("Marker"));
	for (XMLNodeConstIterator c = children.begin (); c != children.end (); ++c) {
		samplecnt_t s;
		samplecnt_t t;
		if (!(*c)->get_property ("source", s) || !(*c)->get_property ("target", t)) {
			return -1;
		}
		if (!tmp.add_marker (s, t)) {
			return -1;
		}
	}

	*this = tmp;
	return 0;
}

bool
WarpMap::operator== (WarpMap const& o) const
{
	return _source_length == o._source_length && _target_length == o._target_length && _markers == o._markers;
}

std::string
WarpMap::to_string () const
{
	std::ostringstream os;
	os << _source_length << ' ' << _target_length;
	for (Markers::const_iterator m = _markers.begin (); m != _markers.end (); ++m) {
		os << ' ' << m->source << ' ' << m->target;
	}
	return os.str ();
}

bool
WarpMap::from_string (std::string const& str)
{
	std::istringstream is (str);
	samplecnt_t        sl;
	samplecnt_t        tl;

	if (!(is >> sl >> tl) || sl < 1 || tl < 1) {
		return false;
	}

	WarpMap     tmp (sl, tl);
	samplecnt_t s;
	samplecnt_t t;

	while (is >> s) {
		if (!(is >> t) || !tmp.add_marker (s, t)) {
			return false;
		}
	}
	if (!is.eof ()) {
		return false; /* trailing junk */
	}

	*this = tmp;
	return true;
}
