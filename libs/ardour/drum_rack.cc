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

#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/xml++.h"

#include "evoral/types.h"

#include "ardour/audio_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/buffer_set.h"
#include "ardour/drum_rack.h"
#include "ardour/midi_buffer.h"
#include "ardour/session.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;

const char* DrumRack::type_name = "drumrack";

namespace {

bool
plugin_type_from_string (std::string const& str, PluginType& type)
{
	if (str == X_("ladspa") || str == X_("Ladspa")) {
		type = ARDOUR::LADSPA;
	} else if (str == X_("lv2")) {
		type = ARDOUR::LV2;
	} else if (str == X_("windows-vst")) {
		type = ARDOUR::Windows_VST;
	} else if (str == X_("lxvst")) {
		type = ARDOUR::LXVST;
	} else if (str == X_("mac-vst")) {
		type = ARDOUR::MacVST;
	} else if (str == X_("audiounit")) {
		type = ARDOUR::AudioUnit;
	} else if (str == X_("luaproc")) {
		type = ARDOUR::Lua;
	} else if (str == X_("vst3")) {
		type = ARDOUR::VST3;
	} else {
		return false;
	}
	return true;
}

} // namespace

/* ---- Pad / PadSet ---------------------------------------------------- */

DrumRack::Pad::Pad ()
	: gain (1.f)
{
}

DrumRack::PadSet::PadSet (PadSet const& other)
	: pads (other.pads)
{
	/* fresh router: copy the layout, never the RT-owned note state */
	sync_router ();
}

bool
DrumRack::PadSet::sync_router ()
{
	std::vector<DrumPadConfig> cfg;
	cfg.reserve (pads.size ());
	for (auto const& p : pads) {
		cfg.push_back (p.config);
	}
	return router.set_pads (cfg);
}

/* ---- construction ----------------------------------------------------- */

DrumRack::DrumRack (Session& s, std::string const& name)
	: Processor (s, name, Temporal::TimeDomainProvider (Temporal::AudioTime))
	, _pad_set (new PadSet)
	, _flush_request (0)
	, _block_size (s.get_block_size ())
{
	_display_to_user = true;
}

DrumRack::~DrumRack ()
{
	/* Pads (and their plugins) go with the last PadSet reference. The
	 * RT thread is not running this processor any more. */
	_rt_last.reset ();
}

/* ---- pad preparation (non-RT) ----------------------------------------- */

void
DrumRack::size_buffers (Pad const& pad, pframes_t nframes) const
{
	AudioEngine* engine = AudioEngine::instance ();

	size_t const audio_size = std::max<size_t> (nframes, engine->raw_buffer_size (DataType::AUDIO) / sizeof (Sample));
	size_t const midi_size  = engine->raw_buffer_size (DataType::MIDI);

	pad.bufs->ensure_buffers (DataType::AUDIO, pad.buf_count.n_audio (), audio_size);
	pad.bufs->ensure_buffers (DataType::MIDI, pad.buf_count.n_midi (), midi_size);
}

bool
DrumRack::prepare_pad (Pad& pad) const
{
	if (!pad.bufs) {
		pad.bufs.reset (new BufferSet ());
	}

	if (!pad.plugin) {
		pad.in_map    = ChanMapping ();
		pad.out_map   = ChanMapping ();
		pad.buf_count = ChanCount (DataType::MIDI, 1);
		size_buffers (pad, _block_size);
		return true;
	}

	PluginInfoPtr info = pad.plugin->get_info ();
	if (!info) {
		return false;
	}

	ChanCount const in  = info->n_inputs;
	ChanCount const out = info->n_outputs;

	if (out.n_audio () == 0) {
		error << string_compose (_("Drum Rack: plugin \"%1\" has no audio outputs"), info->name) << endmsg;
		return false;
	}

	/* in-place: inputs and outputs share buffer indices from 0 */
	pad.in_map    = ChanMapping (in);
	pad.out_map   = ChanMapping (out);
	pad.buf_count = ChanCount::max (in, out);
	pad.buf_count.set (DataType::MIDI, std::max<uint32_t> (1, pad.buf_count.n_midi ()));

	pad.plugin->set_insert_id (pad.id);
	pad.plugin->set_owner (owner ());
	pad.plugin->set_block_size (_block_size);

	size_buffers (pad, _block_size);

	pad.plugin->activate ();
	return true;
}

/* ---- pad editing (non-RT) --------------------------------------------- */

int32_t
DrumRack::add_pad (DrumPadConfig const& config, std::shared_ptr<Plugin> plugin, std::string const& name)
{
	Pad pad;
	pad.config = config;
	pad.plugin = plugin;
	pad.name   = name.empty () && plugin ? plugin->get_info ()->name : name;

	if (!prepare_pad (pad)) {
		return -1;
	}

	std::shared_ptr<PadSet> copy = _pad_set.write_copy ();

	if (copy->pads.size () >= DrumPadRouter::max_pads) {
		_pad_set.no_update ();
		return -1;
	}

	copy->pads.push_back (pad);

	if (!copy->sync_router ()) {
		_pad_set.no_update ();
		return -1;
	}

	int32_t const idx = copy->pads.size () - 1;
	_pad_set.update (copy);
	return idx;
}

bool
DrumRack::remove_pad (uint32_t n)
{
	std::shared_ptr<PadSet> copy = _pad_set.write_copy ();

	if (n >= copy->pads.size ()) {
		_pad_set.no_update ();
		return false;
	}

	copy->pads.erase (copy->pads.begin () + n);
	copy->sync_router ();
	_pad_set.update (copy);
	return true;
}

bool
DrumRack::set_pad_config (uint32_t n, DrumPadConfig const& config)
{
	std::shared_ptr<PadSet> copy = _pad_set.write_copy ();

	if (n >= copy->pads.size ()) {
		_pad_set.no_update ();
		return false;
	}

	copy->pads[n].config = config;

	if (!copy->sync_router ()) {
		_pad_set.no_update ();
		return false;
	}

	_pad_set.update (copy);
	return true;
}

bool
DrumRack::set_pad_gain (uint32_t n, float gain)
{
	if (gain < 0.f) {
		return false;
	}

	std::shared_ptr<PadSet> copy = _pad_set.write_copy ();

	if (n >= copy->pads.size ()) {
		_pad_set.no_update ();
		return false;
	}

	copy->pads[n].gain = gain;
	_pad_set.update (copy);
	return true;
}

uint32_t
DrumRack::n_pads () const
{
	return _pad_set.reader ()->pads.size ();
}

bool
DrumRack::pad_info (uint32_t n, PadInfo& info) const
{
	std::shared_ptr<PadSet const> ps = _pad_set.reader ();

	if (n >= ps->pads.size ()) {
		return false;
	}

	Pad const& p (ps->pads[n]);
	info.id          = p.id;
	info.name        = p.name;
	info.config      = p.config;
	info.gain        = p.gain;
	info.plugin      = p.plugin;
	info.unavailable = (bool)p.unavailable_state;
	return true;
}

/* ---- Processor -------------------------------------------------------- */

bool
DrumRack::can_support_io_configuration (const ChanCount& in, ChanCount& out)
{
	if (in.n_midi () == 0) {
		return false;
	}
	out = ChanCount (DataType::AUDIO, n_outputs);
	return true;
}

int
DrumRack::set_block_size (pframes_t nframes)
{
	/* Called with the process lock held: run() is not executing, so
	 * resizing the pads' buffers in place is safe. Pads published later
	 * are sized in prepare_pad(). */
	_block_size = nframes;

	std::shared_ptr<PadSet const> ps = _pad_set.reader ();

	int ret = 0;
	for (auto const& p : ps->pads) {
		size_buffers (p, nframes);
		if (p.plugin && p.plugin->set_block_size (nframes) != 0) {
			ret = -1;
		}
	}
	return ret;
}

void
DrumRack::set_owner (SessionObject* o)
{
	Processor::set_owner (o);

	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	for (auto const& p : ps->pads) {
		if (p.plugin) {
			p.plugin->set_owner (o);
		}
	}
}

samplecnt_t
DrumRack::signal_latency () const
{
	samplecnt_t l = 0;
	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	for (auto const& p : ps->pads) {
		if (p.plugin) {
			l = std::max (l, p.plugin->signal_latency ());
		}
	}
	return l;
}

void
DrumRack::realtime_handle_transport_stopped ()
{
	_flush_request.store (1);

	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	for (auto const& p : ps->pads) {
		if (p.plugin) {
			p.plugin->realtime_handle_transport_stopped ();
		}
	}
}

void
DrumRack::realtime_locate (bool for_loop_end)
{
	_flush_request.store (1);

	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	for (auto const& p : ps->pads) {
		if (p.plugin) {
			p.plugin->realtime_locate (for_loop_end);
		}
	}
}

void
DrumRack::handover (PadSet const& from, PadSet const& to)
{
	/* Sends a released note to whichever pad in the new set owns the
	 * same plugin instance. */
	struct Redirect : public DrumPadRouter::Sink {
		MidiBuffer* target;
		void deliver (uint32_t, samplecnt_t time, uint8_t const* buf, uint32_t size)
		{
			target->push_back (time, Evoral::MIDI_EVENT, size, buf);
		}
	} redirect;

	for (uint32_t i = 0; i < from.pads.size (); ++i) {
		if (from.router.active_voices (i) == 0) {
			continue;
		}

		Pad const& old_pad (from.pads[i]);
		if (!old_pad.plugin) {
			continue;
		}

		/* O(n^2) over at most 128 pads, only when a held pad changes */
		int32_t j = -1;
		for (uint32_t k = 0; k < to.pads.size (); ++k) {
			if (to.pads[k].plugin == old_pad.plugin) {
				j = k;
				break;
			}
		}

		if (j < 0) {
			/* plugin removed: nothing left to release */
			continue;
		}

		Pad const& new_pad (to.pads[j]);

		if (new_pad.config.delivered_note () == old_pad.config.delivered_note () && !new_pad.config.muted) {
			to.router.adopt_voices (from.router, i, j);
		} else {
			redirect.target = &new_pad.bufs->get_midi (0);
			from.router.release_pad (i, 0, redirect);
		}
	}
}

void
DrumRack::run (BufferSet& bufs, samplepos_t start, samplepos_t end, double speed, pframes_t nframes, bool)
{
	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	PadSet const&                 set (*ps);

	for (auto const& p : set.pads) {
		p.bufs->get_midi (0).clear ();
	}

	if (ps != _rt_last) {
		if (_rt_last) {
			handover (*_rt_last, set);
		}
		_rt_last = ps; /* the old set stays alive in the RCU dead wood */
	}

	struct PadSink : public DrumPadRouter::Sink {
		std::vector<Pad> const* pads;
		void deliver (uint32_t pad, samplecnt_t time, uint8_t const* buf, uint32_t size)
		{
			(*pads)[pad].bufs->get_midi (0).push_back (time, Evoral::MIDI_EVENT, size, buf);
		}
	} sink;
	sink.pads = &set.pads;

	if (_flush_request.exchange (0)) {
		set.router.flush (0, sink);
	}

	bool const active = check_active ();

	if (active && bufs.count ().n_midi () > 0) {
		for (auto const ev : bufs.get_midi (0)) {
			set.router.process (ev.time (), ev.buffer (), ev.size (), sink);
		}
	} else if (!active) {
		/* bypassed: release whatever was held, then stay silent */
		set.router.flush (0, sink);
	}

	bufs.set_count (ChanCount (DataType::AUDIO, n_outputs));

	AudioBuffer& out_l (bufs.get_audio (0));
	AudioBuffer& out_r (bufs.get_audio (1));
	out_l.silence (nframes);
	out_r.silence (nframes);

	for (auto const& p : set.pads) {
		if (!p.plugin) {
			continue;
		}

		BufferSet& pb (*p.bufs);
		pb.set_count (p.buf_count);

		for (uint32_t c = 0; c < p.buf_count.n_audio (); ++c) {
			pb.get_audio (c).silence (nframes);
		}

		/* run even when muted or bypassed so release tails and
		 * note-offs are processed */
		p.plugin->connect_and_run (pb, start, end, speed, p.in_map, p.out_map, nframes, 0);

		if (!active || p.config.muted || p.gain == 0.f) {
			continue;
		}

		uint32_t const n_out = p.plugin->get_info ()->n_outputs.n_audio ();
		if (n_out == 1) {
			out_l.accumulate_with_gain_from (pb.get_audio (0), nframes, p.gain);
			out_r.accumulate_with_gain_from (pb.get_audio (0), nframes, p.gain);
		} else {
			out_l.accumulate_with_gain_from (pb.get_audio (0), nframes, p.gain);
			out_r.accumulate_with_gain_from (pb.get_audio (1), nframes, p.gain);
		}
	}
}

/* ---- state ------------------------------------------------------------ */

XMLNode&
DrumRack::pad_state (Pad const& p) const
{
	XMLNode* node = new XMLNode (X_("Pad"));
	node->set_property (X_("id"), p.id.to_s ());
	node->set_property (X_("name"), p.name);
	node->set_property (X_("trigger-note"), (uint32_t)p.config.trigger_note);
	node->set_property (X_("play-note"), (uint32_t)p.config.play_note);
	node->set_property (X_("choke-group"), (uint32_t)p.config.choke_group);
	node->set_property (X_("muted"), p.config.muted);
	node->set_property (X_("gain"), p.gain);

	if (p.plugin) {
		XMLNode* pn = node->add_child (X_("Plugin"));
		pn->set_property (X_("type"), p.plugin->state_node_name ());
		pn->set_property (X_("unique-id"), p.plugin->unique_id ());
		pn->add_child_nocopy (p.plugin->get_state ());
	} else if (p.unavailable_state) {
		node->add_child_copy (*p.unavailable_state);
	}

	return *node;
}

bool
DrumRack::pad_from_state (XMLNode const& node, int version, Pad& pad) const
{
	std::string str;
	if (node.get_property (X_("id"), str)) {
		pad.id = PBD::ID (str);
	}
	node.get_property (X_("name"), pad.name);

	uint32_t v;
	if (node.get_property (X_("trigger-note"), v)) {
		pad.config.trigger_note = v;
	}
	if (node.get_property (X_("play-note"), v)) {
		pad.config.play_note = v;
	}
	if (node.get_property (X_("choke-group"), v)) {
		pad.config.choke_group = v;
	}
	node.get_property (X_("muted"), pad.config.muted);
	node.get_property (X_("gain"), pad.gain);

	XMLNode const* pn = node.child (X_("Plugin"));
	if (!pn) {
		return prepare_pad (pad);
	}

	PluginType  type;
	std::string unique_id;

	if (!pn->get_property (X_("type"), str) || !plugin_type_from_string (str, type) || !pn->get_property (X_("unique-id"), unique_id)) {
		error << string_compose (_("Drum Rack: pad \"%1\" has an invalid plugin description"), pad.name) << endmsg;
		pad.unavailable_state.reset (new XMLNode (*pn));
		return prepare_pad (pad);
	}

	std::shared_ptr<Plugin> plugin;
	if (!_session.get_disable_all_loaded_plugins ()) {
		plugin = find_plugin (_session, unique_id, type);
	}

	if (!plugin) {
		warning << string_compose (_("Drum Rack: plugin for pad \"%1\" (%2) is not available; its state is kept"), pad.name, unique_id) << endmsg;
		pad.unavailable_state.reset (new XMLNode (*pn));
		return prepare_pad (pad);
	}

	/* plugin state files are keyed by insert id: set it before loading */
	plugin->set_insert_id (pad.id);

	XMLNode const* ps = pn->child (plugin->state_node_name ().c_str ());
	if (ps) {
		plugin->set_state (*ps, version);
	}

	pad.plugin = plugin;
	return prepare_pad (pad);
}

XMLNode&
DrumRack::state () const
{
	XMLNode& node = Processor::state ();
	node.set_property (X_("type"), type_name);

	std::shared_ptr<PadSet const> ps = _pad_set.reader ();
	for (auto const& p : ps->pads) {
		node.add_child_nocopy (pad_state (p));
	}

	return node;
}

int
DrumRack::set_state (XMLNode const& node, int version)
{
	if (Processor::set_state (node, version)) {
		return -1;
	}

	std::vector<Pad> pads;

	XMLNodeList const& children (node.children (X_("Pad")));
	for (XMLNodeConstIterator c = children.begin (); c != children.end (); ++c) {
		Pad pad;
		if (!pad_from_state (**c, version, pad)) {
			return -1;
		}
		pads.push_back (pad);
	}

	std::shared_ptr<PadSet> copy = _pad_set.write_copy ();
	copy->pads.swap (pads);

	if (!copy->sync_router ()) {
		_pad_set.no_update ();
		error << _("Drum Rack: invalid pad layout in session state") << endmsg;
		return -1;
	}

	_pad_set.update (copy);
	return 0;
}
