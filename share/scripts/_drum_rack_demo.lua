ardour {
	["type"]    = "EditorAction",
	name        = "Add Drum Rack (demo kit)",
	license     = "GPLv2+",
	author      = "Rolandok",
	description = [[Adds a Drum Rack to each selected MIDI track: kick (36),
snare (38), closed hat (42) and open hat (46), each pad playing its own
ACE Reasonable Synth on C3. The two hi-hats share choke group 1, so the
closed hat cuts off a ringing open hat. Swap the plugin name for any
instrument you have.]]
}

function factory () return function ()
	local plugin = "ACE Reasonable Synth"
	local kit = {
		-- trigger note, play note, choke group
		{ 36, 60, 0 }, -- kick
		{ 38, 60, 0 }, -- snare
		{ 42, 60, 1 }, -- closed hat
		{ 46, 60, 1 }, -- open hat
	}

	local sel = Editor:get_selection ()
	for r in sel.tracks:routelist ():iter () do
		if not r:to_track ():isnil () and not r:to_track ():to_midi_track ():isnil () then
			local rack = ARDOUR.LuaAPI.new_drum_rack (Session)
			assert (not rack:isnil ())
			for _, pad in ipairs (kit) do
				local idx = ARDOUR.LuaAPI.drum_rack_add_pad (Session, rack, pad[1], pad[2], pad[3], plugin, ARDOUR.PluginType.LV2)
				if idx < 0 then
					print ("Drum Rack: could not add pad for note", pad[1])
				end
			end
			-- first in the chain, ahead of the fader
			r:add_processor_by_index (rack, 0, nil, true)
		end
	end
end end
