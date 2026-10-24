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

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "pbd/id.h"
#include "pbd/rcu.h"

#include "ardour/chan_mapping.h"
#include "ardour/drum_pad_router.h"
#include "ardour/libardour_visibility.h"
#include "ardour/plugin.h"
#include "ardour/processor.h"

class XMLNode;

namespace ARDOUR {

class BufferSet;
class Session;

/** An Ableton-style Drum Rack: one MIDI-in, stereo-out processor that
 * hosts one instrument plugin per pad.
 *
 * Incoming MIDI is split by DrumPadRouter (trigger note -> pad, choke
 * groups). Each pad's plugin runs on its own scratch buffers, and the
 * pads are summed to stereo with a per-pad gain. A mono plugin feeds
 * both channels; a plugin with more than two outputs contributes its
 * first two.
 *
 * Threading: the pad set is RCU-managed. All pad edits (add, remove,
 * reconfigure, gain) happen on a non-RT thread, which prepares plugins
 * and buffers before publishing a new pad set. run() picks the new set
 * up at the start of the next cycle. Notes held across a swap carry
 * over when the pad keeps the same plugin and play note; otherwise they
 * are released.
 */
class LIBARDOUR_API DrumRack : public Processor
{
public:
	static constexpr uint32_t n_outputs = 2;

	/** What the GUI/Lua layer sees of a pad. */
	struct PadInfo {
		PBD::ID                 id;
		std::string             name;
		DrumPadConfig           config;
		float                   gain; ///< linear
		std::shared_ptr<Plugin> plugin; ///< null for an empty or unavailable pad
		bool                    unavailable; ///< plugin state saved but plugin not found on this system
	};

	DrumRack (Session&, std::string const& name = "Drum Rack");
	~DrumRack ();

	/* Pad editing. Not RT-safe; call from the GUI or a script. */

	/** Append a pad. @p plugin may be null (an empty pad). The plugin must
	 * not be shared with any other processor or pad.
	 * @return the new pad's index, or -1 if the rack is full or the
	 * config is invalid (e.g. trigger note already used).
	 */
	int32_t add_pad (DrumPadConfig const&, std::shared_ptr<Plugin> plugin, std::string const& name = "");

	bool remove_pad (uint32_t n);
	bool set_pad_config (uint32_t n, DrumPadConfig const&);
	bool set_pad_gain (uint32_t n, float gain);

	uint32_t n_pads () const;
	bool     pad_info (uint32_t n, PadInfo&) const;

	/* Processor */

	void run (BufferSet&, samplepos_t start, samplepos_t end, double speed, pframes_t nframes, bool result_required);
	bool can_support_io_configuration (const ChanCount& in, ChanCount& out);
	int  set_block_size (pframes_t);
	void set_owner (SessionObject*);

	void realtime_handle_transport_stopped ();
	void realtime_locate (bool);

	/** Highest latency of any pad's plugin. Pads are not yet delay-
	 * compensated against each other.
	 */
	samplecnt_t signal_latency () const;

	XMLNode& state () const;
	int      set_state (XMLNode const&, int version);

	static const char* type_name;

private:
	struct Pad {
		Pad ();

		PBD::ID                    id;
		std::string                name;
		DrumPadConfig              config;
		float                      gain;
		std::shared_ptr<Plugin>    plugin;
		std::shared_ptr<BufferSet> bufs;   ///< scratch for this pad's plugin, owned by the pad
		ChanMapping                in_map; ///< identity over the plugin's inputs, precomputed off the RT thread
		ChanMapping                out_map;
		ChanCount                  buf_count; ///< channels the plugin needs in bufs
		std::shared_ptr<XMLNode>   unavailable_state; ///< plugin XML kept verbatim when the plugin can't be loaded
	};

	struct PadSet {
		PadSet () {}
		/* copies the pads but not the router's note state: a copy is a
		 * fresh layout, and the writer must not read state the RT thread
		 * is mutating. */
		PadSet (PadSet const&);

		std::vector<Pad>      pads;
		/* RT-only state; mutated through the const shared_ptr the RT
		 * thread gets from the RCU reader. */
		mutable DrumPadRouter router;

		bool sync_router ();
	};

	SerializedRCUManager<PadSet> _pad_set;

	/* RT thread only: the set used last cycle, kept so held notes can be
	 * handed over or released when a new set is published. The RCU
	 * manager's dead-wood list also holds it, so it is never freed here. */
	std::shared_ptr<PadSet const> _rt_last;

	std::atomic<int> _flush_request;
	std::atomic<pframes_t> _block_size; ///< written under the process lock, read when preparing pads

	bool prepare_pad (Pad&) const;
	void size_buffers (Pad const&, pframes_t) const;
	void handover (PadSet const& from, PadSet const& to);

	XMLNode& pad_state (Pad const&) const;
	bool     pad_from_state (XMLNode const&, int version, Pad&) const;
};

} // namespace ARDOUR
