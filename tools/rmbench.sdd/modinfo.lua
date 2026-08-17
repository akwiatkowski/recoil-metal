-- Minimal Recoil game archive whose only purpose is to load a map and time
-- frames. No units, no gadgets, no widgets framework.
--
-- Why not reuse FAR's far.sdd: that archive carries converted Supreme Commander
-- unit content, and rendering it would benchmark unit drawing as well as
-- terrain. recoil-metal draws terrain only, so the comparison has to be
-- terrain-only on both sides or it is not a comparison.
return {
	name = "recoil-metal terrain benchmark",
	shortName = "rmbench",
	game = "rmbench",
	shortGame = "rmbench",
	version = "1",
	mutator = "official",
	description = "Loads a map, times frames, writes them to the log, quits.",
	modtype = 1,
}
