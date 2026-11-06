/* Race page: live standings, lap tables with history, spoken callouts.
 *
 * The scoring rules are the Chorus32 race page's (Chorus32-LapTimerBreakingCarbon):
 * the hole shot, nothing scores after a pilot takes the flag but laps are still timed,
 * position by laps then time on track, a table per round that survives a reload.
 * Here the laps come from the base / server as indexed `lap` events (index 0 is the
 * hole shot) and the page does everything else, as it did on the Chorus32 timer. */

header('race'); connect();

var NUM_PILOTS = 8;
var DEFAULT_TARGET = 20;      // a practice round is the common case: a number nobody hits by accident

var race_num = null;          // null until the first message
var race_state = "idle";      // idle | staging | racing | stopped
var race_start_t = null, race_go_t = null, race_stop_t = null;
var count_first = 0;          // whether lap 0 (the hole shot) counts as a lap
var target_laps = DEFAULT_TARGET;
var leader = null;
var announced_last_lap = false;

var pilots = [];
for (var i = 0; i < NUM_PILOTS; ++i) {
	pilots[i] = {
		node: i, active: false, name: "Pilot " + (i + 1),
		laps: [],            // every lap time in ms, index 0 is the hole shot
		finished_at: null,   // finishing position once they complete target_laps
		finish_index: null,  // index of the lap that took the flag; later ones are extra
		last_is_best: false,
		freq: 0, enter: 0, exit: 0,
	};
}
var name_edited_at = [0, 0, 0, 0, 0, 0, 0, 0];   // ignore the server's copy of a name for a moment after typing


function channel_label(pilot) {
	var i = RACEBAND.indexOf(pilot.freq);
	if (!pilot.freq) return "off";
	return (i >= 0 ? "R" + (i + 1) + " · " : "") + pilot.freq;
}

/* ---------------- race maths (from the Chorus32 page) ---------------- */

// Laps that count towards the result. Crossing the gate after taking the flag is
// normal -- a pilot with pack left keeps flying. Those laps are recorded and shown
// but must not reach any scored figure.
function scoring_laps(pilot) {
	if (pilot.finish_index == null) return pilot.laps;
	return pilot.laps.slice(0, pilot.finish_index + 1);
}

function extra_laps(pilot) {
	if (pilot.finish_index == null) return [];
	return pilot.laps.slice(pilot.finish_index + 1).filter(function (lap) { return lap !== undefined; });
}

function lap_number(lap_index) { return count_first ? lap_index + 1 : lap_index; }

// Is anybody still racing? Only pilots who have actually crossed the gate count: a
// pilot can be enabled with nobody on it.
function everyone_home() {
	return pilots.filter(function (p) { return p.active && p.laps.length > 0; })
		.every(function (p) { return p.finished_at != null; });
}

// Lap 0 is the hole shot: not a lap of the course, so unless the timer counts it
// it stays out of totals, bests and the lap count.
function counted_laps(pilot) {
	var scored = scoring_laps(pilot);
	var laps = count_first ? scored : scored.slice(1);
	return laps.filter(function (lap) { return lap !== undefined; });
}

function lap_count(pilot) { return counted_laps(pilot).length; }

function total_time(pilot) {
	return counted_laps(pilot).reduce(function (sum, lap) { return sum + lap; }, 0);
}

// Time on track since the start, hole shot included. This is what decides position
// (total_time drops the hole shot, which is right for lap statistics and wrong for
// placing).
function elapsed_time(pilot) {
	return scoring_laps(pilot).reduce(function (sum, lap) { return sum + (lap === undefined ? 0 : lap); }, 0);
}

function best_lap(pilot) {
	var laps = counted_laps(pilot);
	return laps.length ? Math.min.apply(null, laps) : null;
}

function best_consecutive(pilot) {
	var laps = counted_laps(pilot);
	if (laps.length < 2) return null;
	var best = Infinity;
	for (var i = 0; i + 1 < laps.length; ++i) best = Math.min(best, laps[i] + laps[i + 1]);
	return best;
}

// Most laps first, and among pilots on the same lap the one who got there soonest.
// Anyone home is classified ahead of anyone still out, in finishing order.
function standings() {
	var running = pilots.filter(function (p) { return p.active; });
	running.sort(function (a, b) {
		if (a.finished_at != null || b.finished_at != null) {
			if (a.finished_at == null) return 1;
			if (b.finished_at == null) return -1;
			return a.finished_at - b.finished_at;
		}
		var laps_a = lap_count(a), laps_b = lap_count(b);
		if (laps_a != laps_b) return laps_b - laps_a;
		return elapsed_time(a) - elapsed_time(b);
	});
	return running;
}

function format_time(ms) {
	if (ms == null) return "—";
	return (ms / 1000).toFixed(2);
}

function gap_text(pilot, lead) {
	if (lead == null || lap_count(lead) == 0) return "";
	if (lap_count(pilot) == 0) return "no laps";
	if (pilot === lead) return "leader";
	var lap_diff = lap_count(lead) - lap_count(pilot);
	if (lap_diff > 0) return "+" + lap_diff + (lap_diff == 1 ? " lap" : " laps");
	return "+" + ((elapsed_time(pilot) - elapsed_time(lead)) / 1000).toFixed(2) + "s";
}

function escape_html(text) {
	return String(text).replace(/[&<>"']/g, function (ch) {
		return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch];
	});
}

/* ---------------- roster ---------------- */

var live_levels = null, live_levels_at = 0;

function build_roster() {
	var html = "";
	for (var node = 0; node < NUM_PILOTS; ++node) {
		html +=
			'<div class="rx-row" id="roster_row_' + node + '" style="--c:var(--p' + (node + 1) + ')">' +
				'<input type="checkbox" id="roster_active_' + node + '" title="in this race">' +
				'<input type="text" id="roster_name_' + node + '" value="" maxlength="12" autocomplete="off">' +
				'<select id="roster_freq_' + node + '" class="rx-chan"></select>' +
				'<div class="rx-tune">' +
					'<label for="roster_enter_' + node + '">Enter</label><input type="number" step="0.25" id="roster_enter_' + node + '">' +
					'<label for="roster_exit_' + node + '">Exit</label><input type="number" step="0.25" id="roster_exit_' + node + '">' +
					'<span class="meter"><span class="meter-level" id="roster_level_' + node + '"></span>' +
					'<span class="meter-mark" id="roster_mark_' + node + '"></span><span class="meter-mark exit" id="roster_xmark_' + node + '"></span></span>' +
				'</div>' +
			'</div>';
	}
	document.getElementById("roster").innerHTML = html;
	for (node = 0; node < NUM_PILOTS; ++node) wire_roster(node);
}

var name_timers = {};
function send_name(node) {
	clearTimeout(name_timers[node]);
	name_timers[node] = setTimeout(function () {
		post('/api/pilot', { n: node + 1, name: pilots[node].name, push: false });
	}, 400);
}

function set_name(node, value, source) {
	pilots[node].name = value;
	name_edited_at[node] = Date.now();
	send_name(node);
	// Keep the other copy of the name (roster or standings row) in step.
	["roster_name_", "name_"].forEach(function (prefix) {
		var el = document.getElementById(prefix + node);
		if (el != null && el !== source && el.value != value) el.value = value;
	});
}

function wire_roster(node) {
	var pilot = pilots[node];

	document.getElementById("roster_active_" + node).addEventListener("change", function () {
		post('/api/pilot', { n: node + 1, enabled: this.checked });
	});

	document.getElementById("roster_name_" + node).addEventListener("input", function () {
		set_name(node, this.value, this);
		render();
		render_lap_tables();
	});

	document.getElementById("roster_freq_" + node).addEventListener("change", function () {
		post('/api/pilot', { n: node + 1, freq: parseInt(this.value, 10) });
	});

	function send_thresholds() {
		var enter = fromDb(parseFloat(document.getElementById("roster_enter_" + node).value));
		var exit = fromDb(parseFloat(document.getElementById("roster_exit_" + node).value));
		// Never send NaN or a crossed pair: an empty or invalid field puts the known values back.
		if (isNaN(enter) || isNaN(exit) || exit < 1 || exit >= enter || enter > 254) {
			document.getElementById("roster_enter_" + node).value = dbShow(pilot.enter);
			document.getElementById("roster_exit_" + node).value = dbShow(pilot.exit);
			return;
		}
		post('/api/pilot', { n: node + 1, enter: enter, exit: exit });
	}
	document.getElementById("roster_enter_" + node).addEventListener("change", send_thresholds);
	document.getElementById("roster_exit_" + node).addEventListener("change", send_thresholds);
}

function field_busy(el) { return el != null && document.activeElement === el; }

function render_roster() {
	var included = 0;
	for (var node = 0; node < NUM_PILOTS; ++node) {
		var pilot = pilots[node];
		var row = document.getElementById("roster_row_" + node);
		if (row == null) continue;
		row.className = "rx-row" + (pilot.active ? "" : " off");
		if (pilot.active) included++;

		var active = document.getElementById("roster_active_" + node);
		if (!field_busy(active)) active.checked = pilot.active;

		var name = document.getElementById("roster_name_" + node);
		if (!field_busy(name) && Date.now() - name_edited_at[node] > 1500 && name.value != pilot.name) name.value = pilot.name;

		var sel = document.getElementById("roster_freq_" + node);
		if (!field_busy(sel)) {
			var opts = RACEBAND.map(function (f, i) { return [f, "R" + (i + 1) + " · " + f]; });
			if (pilot.freq && RACEBAND.indexOf(pilot.freq) < 0) opts.push([pilot.freq, String(pilot.freq)]);
			var want = opts.map(function (o) { return o[0] + o[1]; }).join("|");
			if (sel.dataset.sig !== want) {
				sel.dataset.sig = want;
				sel.innerHTML = opts.map(function (o) { return '<option value="' + o[0] + '">' + o[1] + "</option>"; }).join("");
			}
			sel.value = pilot.freq;
		}

		var en = document.getElementById("roster_enter_" + node), ex = document.getElementById("roster_exit_" + node);
		if (!field_busy(en)) en.value = dbShow(pilot.enter);
		if (!field_busy(ex)) ex.value = dbShow(pilot.exit);

		// The live level needs the gate on USB (the LEVELS stream); over LoRa there is none.
		var level = document.getElementById("roster_level_" + node);
		var live = live_levels && Date.now() - live_levels_at < 2500;
		level.parentNode.style.display = live ? "" : "none";
		if (live) {
			level.style.width = Math.min(100, (live_levels[node] / 255) * 100) + "%";
			document.getElementById("roster_mark_" + node).style.left = Math.min(100, pilot.enter / 255 * 100) + "%";
			document.getElementById("roster_xmark_" + node).style.left = Math.min(100, pilot.exit / 255 * 100) + "%";
		}
	}
	document.getElementById("roster_count").innerText = included + (included == 1 ? " pilot" : " pilots");
}

/* ---------------- standings ---------------- */

var container = document.getElementById("standings");

function render() {
	var order = standings();
	document.getElementById("standings_empty").style.display = order.length ? "none" : "";
	var lead = order.length ? order[0] : null;

	// Rebuild only when the set or order of pilots changes; otherwise patch the rows so
	// that editing a name doesn't lose focus every time a lap arrives.
	var signature = order.map(function (p) { return p.node; }).join(",");
	if (container.dataset.signature !== signature) {
		container.dataset.signature = signature;
		container.innerHTML = order.map(function (pilot) {
			return '<div class="pilot" id="pilot_' + pilot.node + '" style="--c:var(--p' + (pilot.node + 1) + ')">' +
				'<div class="pos"></div>' +
				'<input class="name" id="name_' + pilot.node + '" value="" maxlength="12" autocomplete="off">' +
				'<div class="meta"></div>' +
				'<div class="last"><span class="last-label">Last lap</span><span class="value"></span><span class="gap"></span></div>' +
				'</div>';
		}).join("");
		order.forEach(function (pilot) {
			var input = document.getElementById("name_" + pilot.node);
			input.value = pilot.name;
			input.addEventListener("input", function () { set_name(pilot.node, this.value, this); render_lap_tables(); });
		});
	}

	order.forEach(function (pilot, index) {
		var row = document.getElementById("pilot_" + pilot.node);
		if (row == null) return;
		row.className = "pilot" + (index == 0 && lap_count(pilot) > 0 ? " leader" : "") + (pilot.finished_at != null ? " finished" : "");
		row.querySelector(".pos").innerText = lap_count(pilot) > 0 || pilot.finished_at != null ? index + 1 : "–";
		var name = document.getElementById("name_" + pilot.node);
		if (!field_busy(name) && name.value != pilot.name) name.value = pilot.name;

		// The headline figure is the last crossing, whichever side of the flag it fell.
		var laps = counted_laps(pilot), extras = extra_laps(pilot);
		var last = extras.length ? extras[extras.length - 1] : (laps.length ? laps[laps.length - 1] : null);
		var value = row.querySelector(".value");
		value.innerText = format_time(last);
		value.parentNode.className = "last" + (pilot.last_is_best ? " pb" : "") + (extras.length ? " beyond" : "");
		row.querySelector(".gap").innerText = pilot.finished_at != null ? "finished" : gap_text(pilot, lead);

		row.querySelector(".meta").innerHTML =
			"<span>" + channel_label(pilot) + "</span>" +
			"<span>Lap " + lap_count(pilot) + "/" + target_laps + "</span>" +
			(extras.length ? '<span class="extra">+' + extras.length + " after flag</span>" : "") +
			"<span>Best " + format_time(best_lap(pilot)) + "</span>" +
			"<span>2&nbsp;consec " + format_time(best_consecutive(pilot)) + "</span>";
	});
}

/* ---------------- lap tables and history ---------------- */

// Finished rounds are kept so a night's racing stays on screen, newest first, and survive
// a reload. They live in this browser (a phone at a track locks, sleeps and reconnects).
var MAX_HISTORY_ROUNDS = 8;
// Named race_history, not history: a top-level `var history` cannot shadow window.history.
var race_history = load_history();

function load_history() {
	try {
		var raw = localStorage.getItem("bc_history");
		var parsed = raw ? JSON.parse(raw) : [];
		if (!Array.isArray(parsed)) return [];
		return parsed.sort(function (a, b) { return (b.at || 0) - (a.at || 0); });
	} catch (e) { return []; }
}

function save_history() {
	try { localStorage.setItem("bc_history", JSON.stringify(race_history)); } catch (e) { /* quota or private mode */ }
}

function current_rows() {
	return pilots.filter(function (p) { return p.active && p.laps.length > 0; }).map(function (p) {
		return { node: p.node, name: p.name, laps: p.laps.slice(), finish_index: p.finish_index };
	});
}

// Snapshot whatever is on screen before a new round replaces it.
function archive_current_race() {
	var rows = current_rows();
	if (rows.length == 0) return;
	race_history.unshift({ race_num: race_num, at: Date.now(), target_laps: target_laps, count_first: count_first, rows: rows });
	race_history = race_history.slice(0, MAX_HISTORY_ROUNDS);
	save_history();
}

function counted_from(row, counts_first) {
	var laps = row.finish_index == null ? row.laps : row.laps.slice(0, row.finish_index + 1);
	var list = counts_first ? laps : laps.slice(1);
	return list.filter(function (lap) { return lap !== undefined; });
}

function round_html(round, is_live) {
	var max_laps = 0;
	round.rows.forEach(function (row) { max_laps = Math.max(max_laps, row.laps.length); });
	if (max_laps == 0) return "";
	var any_extra = false;

	var fastest = Infinity;
	round.rows.forEach(function (row) {
		counted_from(row, round.count_first).forEach(function (lap) { if (lap < fastest) fastest = lap; });
	});

	var header = '<th class="col-pilot" style="border-left-color:transparent">Pilot</th>';
	for (var i = 0; i < max_laps; ++i) {
		var label = (!round.count_first && i == 0) ? "HS" : (round.count_first ? i + 1 : i);
		header += "<th>" + label + "</th>";
	}
	header += '<th class="col-sep">Best</th><th>2&nbsp;cons</th><th>Total</th>';

	var body = round.rows.map(function (row) {
		var counted = counted_from(row, round.count_first);
		var best = counted.length ? Math.min.apply(null, counted) : null;
		var consec = null;
		for (var i = 0; i + 1 < counted.length; ++i) {
			var pair = counted[i] + counted[i + 1];
			if (consec == null || pair < consec) consec = pair;
		}
		var total = counted.reduce(function (sum, lap) { return sum + lap; }, 0);

		var cells = '<td class="col-pilot" style="--c:var(--p' + (row.node + 1) + ')">' + escape_html(row.name) + "</td>";
		for (i = 0; i < max_laps; ++i) {
			var lap = row.laps[i];
			if (lap === undefined) { cells += '<td class="cell-empty">&mdash;</td>'; continue; }
			var is_holeshot = !round.count_first && i == 0;
			// Flown after this pilot took the flag: timed but not scored. Marked, not hidden.
			var is_extra = row.finish_index != null && i > row.finish_index;
			if (is_extra) any_extra = true;
			var cls = is_extra ? "cell-extra" : (is_holeshot ? "cell-holeshot" : (lap === fastest ? "cell-fastest" : (lap === best ? "cell-best" : "")));
			cells += '<td class="' + cls + '">' + (lap / 1000).toFixed(2) + "</td>";
		}
		cells += '<td class="col-sep">' + format_time(best) + "</td><td>" + format_time(consec) + "</td><td>" +
			(total ? (total / 1000).toFixed(2) : "—") + "</td>";
		return "<tr>" + cells + "</tr>";
	}).join("");

	return '<div class="round"><div class="round-title"><span>Round ' + round.race_num + "</span>" +
		(is_live ? '<span class="round-live">live</span>' : '<span class="round-when">' + when_text(round.at) + "</span>") + "</div>" +
		'<div class="laptable-wrap"><table class="laptable"><thead><tr>' + header + "</tr></thead><tbody>" + body + "</tbody></table></div>" +
		(any_extra ? '<p class="legend">Struck-through times were flown after that pilot took the flag: timed, but outside the result.</p>' : "") +
		"</div>";
}

// Time of day for a round flown today, date and time for anything older.
function when_text(at) {
	if (!at) return "earlier";
	var then = new Date(at);
	var pad = function (n) { return String(n).padStart(2, "0"); };
	var clock = pad(then.getHours()) + ":" + pad(then.getMinutes());
	if (then.toDateString() === new Date().toDateString()) return clock;
	return pad(then.getDate()) + "/" + pad(then.getMonth() + 1) + " " + clock;
}

function render_lap_tables() {
	var live = { race_num: race_num == null ? 0 : race_num, target_laps: target_laps, count_first: count_first, rows: current_rows() };
	var html = "";
	if (live.rows.length > 0) html += round_html(live, true);
	race_history.forEach(function (round) { html += round_html(round, false); });
	document.getElementById("lap_rounds").innerHTML = html;
	document.getElementById("lap_table_empty").style.display = html ? "none" : "";
	render_lap_stats(live);
}

function stat(label, value) {
	return '<div><div class="stat-label">' + label + '</div><div class="stat-value">' + value + "</div></div>";
}

function render_lap_stats(live) {
	var all = [];
	live.rows.forEach(function (row) { all = all.concat(counted_from(row, live.count_first)); });
	var fastest = all.length ? Math.min.apply(null, all) : null;
	var average = all.length ? all.reduce(function (a, b) { return a + b; }, 0) / all.length : null;
	document.getElementById("lap_stats").innerHTML =
		stat("Laps this round", all.length ? String(all.length) : "—") + stat("Fastest", format_time(fastest)) +
		stat("Average", format_time(average)) + stat("Rounds kept", String(race_history.length + (live.rows.length ? 1 : 0)));
}

document.getElementById("clear_history").onclick = function () {
	race_history = [];
	save_history();
	render_lap_tables();
};

/* ---------------- lap handling ---------------- */

// quiet: replaying laps the page missed (a reload, a dropped connection): no beeps, no voice.
function on_lap(node, lap_index, lap_ms, quiet) {
	var pilot = pilots[node];
	if (pilot.laps[lap_index] !== undefined) return;   // already have it
	pilot.laps[lap_index] = lap_ms;

	// Beep on every crossing, the hole shot included: someone went through the gate.
	if (!quiet) Beep.lap(node);

	// Past the flag: timed and shown, but out of the result.
	if (pilot.finish_index != null && lap_index > pilot.finish_index) {
		pilot.last_is_best = false;
		// While anyone is still out the callouts belong to them; once everyone is home
		// the voice is free, so their times get read again.
		if (!quiet && everyone_home()) Announcer.lap(pilot.name, lap_number(lap_index), lap_ms / 1000);
		return;
	}

	var laps = counted_laps(pilot);
	var is_counted = count_first || lap_index > 0;
	if (!is_counted) return;

	var best = best_lap(pilot);
	pilot.last_is_best = laps.length > 1 && lap_ms === best;
	if (!quiet) Announcer.lap(pilot.name, lap_count(pilot), lap_ms / 1000);

	// Lead changes, announced once per change and only once someone has completed a lap.
	var order = standings();
	if (order.length && lap_count(order[0]) > 0 && order[0] !== leader) {
		if (leader != null && !quiet) Announcer.lead_change(order[0].name);
		leader = order[0];
	}

	// Final lap: said once, when the first pilot starts their last one.
	if (!announced_last_lap && lap_count(pilot) == target_laps - 1 && target_laps > 1) {
		announced_last_lap = true;
		if (!quiet) Announcer.last_lap(pilot.name);
	}

	if (pilot.finished_at == null && lap_count(pilot) >= target_laps) {
		// Rank among those already home by time on track, not by message arrival order.
		var mine = elapsed_time(pilot);
		var ahead = pilots.filter(function (p) { return p.finished_at != null && elapsed_time(p) < mine; }).length;
		pilot.finished_at = ahead + 1;
		pilot.finish_index = lap_index;   // everything after this index is a lap flown past the flag
		if (!quiet) {
			Beep.finish();
			Announcer.finish(pilot.name, pilot.finished_at);
		}
	}
}

function clear_round() {
	pilots.forEach(function (p) { p.laps = []; p.finished_at = null; p.finish_index = null; p.last_is_best = false; });
	leader = null;
	announced_last_lap = false;
	container.dataset.signature = "";   // force a rebuild
}

/* ---------------- server state ---------------- */

function apply_cfg() {
	if (!App.cfg) return;
	App.cfg.pilots.forEach(function (p) {
		var q = pilots[p.n - 1];
		q.active = p.enabled; q.freq = p.freq; q.enter = p.enter; q.exit = p.exit;
		if (Date.now() - name_edited_at[p.n - 1] > 1500) q.name = p.name;
	});
	var s = App.cfg.settings;
	count_first = s.holeshot ? 0 : 1;
	var min_field = document.getElementById("min_lap"), cd = document.getElementById("countdown"), cf = document.getElementById("count_first");
	var tl = document.getElementById("target_laps");
	if (!field_busy(tl)) { target_laps = s.laps; tl.value = s.laps; }
	if (!field_busy(min_field)) min_field.value = Math.round(s.min_lap_ms / 1000);
	if (!field_busy(cd)) cd.value = s.countdown;
	if (!field_busy(cf)) cf.checked = !!count_first;
	render_roster(); render(); render_lap_tables();
}

function set_state_ui() {
	var bar = document.getElementById("racebar"), button = document.getElementById("race_button"), status = document.getElementById("race_status");
	var live = race_state == "racing" || race_state == "staging";
	bar.classList.toggle("racing", race_state == "racing");
	status.innerText = { idle: "Idle", staging: "Get ready…", racing: "Racing", stopped: "Stopped" }[race_state] || race_state;
	button.innerText = live ? "Stop" : "Start";
	button.className = live ? "stop big-btn" : "primary big-btn";
	document.getElementById("roster_card").open = !live;
}

function apply_race(r, quiet) {
	var was = race_state;
	if (race_num !== null && r.num !== race_num) {
		// A new round: keep the one that just ran, then clear the table.
		archive_current_race();
		clear_round();
	}
	race_num = r.num;
	document.getElementById("race_num").innerText = race_num;
	race_state = r.state; race_start_t = r.start_t; race_go_t = r.go_t; race_stop_t = r.stop_t;

	// Fewer laps on the server than here means the round was reset: drop ours.
	var local = 0, remote = 0;
	pilots.forEach(function (p) { local += p.laps.length; });
	for (var n = 1; n <= NUM_PILOTS; ++n) remote += (r.laps[n] || []).length;
	if (remote < local) clear_round();
	for (n = 1; n <= NUM_PILOTS; ++n) {
		(r.laps[n] || []).forEach(function (ms, idx) { on_lap(n - 1, idx, ms, true); });
	}

	if (!quiet && race_state == "racing" && was != "racing") {
		Beep.start();
		Announcer.go();
	}
	set_state_ui();
	render(); render_lap_tables(); render_roster();
}

onMsg(function (m) {
	if (m.type === 'snapshot') {
		apply_cfg();
		if (m.race) apply_race(m.race, true);
	} else if (m.type === 'cfg') {
		apply_cfg();
	} else if (m.type === 'race') {
		apply_race(m.race, false);
	} else if (m.type === 'lap') {
		on_lap(m.n - 1, m.i, m.ms, false);
		render(); render_lap_tables();
	} else if (m.type === 'levels') {
		live_levels = m.d.l; live_levels_at = Date.now();
	}
});

/* ---------------- race control and options ---------------- */

document.getElementById("race_button").onclick = function () {
	post(race_state == "racing" || race_state == "staging" ? '/api/race/stop' : '/api/race/start');
};

function setting(id, key, cast) {
	var el = document.getElementById(id);
	el.addEventListener("change", function () {
		var v = cast(el.type === "checkbox" ? el.checked : el.value);
		if (v === null) { apply_cfg(); return; }   // invalid: put the known value back
		var body = {}; body[key] = v;
		post('/api/settings', body);
	});
}
setting("min_lap", "min_lap_ms", function (v) { v = parseInt(v, 10); return isNaN(v) || v < 1 || v > 120 ? null : v * 1000; });
setting("countdown", "countdown", function (v) { v = parseInt(v, 10); return isNaN(v) || v < 0 || v > 30 ? null : v; });
setting("count_first", "holeshot", function (v) { return !v; });

// The lap target is kept with the other race options in the gate's flash, like the names, so every
// phone and the Mac agree.
document.getElementById("target_laps").addEventListener("input", function () {
	target_laps = Math.max(1, parseInt(this.value, 10) || 1);
	render();
});
document.getElementById("target_laps").addEventListener("change", function () {
	var v = Math.max(1, Math.min(99, parseInt(this.value, 10) || 1));
	this.value = v; target_laps = v;
	post('/api/settings', { laps: v });
});

setInterval(function () {
	var t = 0, s = race_state;
	if (s == "staging" && race_go_t != null) t = serverNow() - race_go_t;                    // negative: counting down
	else if (s == "racing" && race_start_t != null) t = serverNow() - race_start_t;
	else if (s == "stopped" && race_start_t != null && race_stop_t != null) t = race_stop_t - race_start_t;
	var neg = t < 0, e = Math.floor(Math.abs(t));
	document.getElementById("race_clock").innerText =
		(neg ? "−" : "") + String(Math.floor(e / 60)).padStart(2, "0") + ":" + String(e % 60).padStart(2, "0");
}, 200);
setInterval(render_roster, 300);

/* ---------------- sound and screen options (from the Chorus32 page) ---------------- */

var voice_select = document.getElementById("voice_select");
var lang_select = document.getElementById("lang_select");
var voice_hint = document.getElementById("voice_hint");

try {
	var stored_lang = localStorage.getItem("bc_lang");
	if (stored_lang) lang_select.value = stored_lang;
} catch (e) { /* ignore */ }
Announcer.set_language(lang_select.value);

var voice_match_found = true;

// The browser can only speak with voices the device already has, so say so plainly
// when the chosen language has none rather than failing silently.
function on_voices_ready(found_match) { voice_match_found = found_match; update_voice_hint(); }

function update_voice_hint() {
	if (!Announcer.is_speaker()) {
		voice_hint.style.color = "var(--warn)";
		voice_hint.innerText = "Callouts are muted here: the race page is open somewhere else and that copy is doing the announcing. Reload this page to take it back.";
		return;
	}
	voice_hint.style.color = "";
	if (voice_match_found) { voice_hint.innerText = ""; return; }
	voice_hint.innerText = lang_select.value == "ar"
		? "No Arabic voice is installed on this device, so callouts will use whatever voice is selected. Add one under Settings › Accessibility › Spoken Content › Voices on iOS, or in the Google text-to-speech settings on Android."
		: "No voice for this language is installed on this device.";
}
setInterval(update_voice_hint, 2000);

Announcer.populate_voices(voice_select, on_voices_ready);

voice_select.addEventListener("change", function () {
	Announcer.set_voice(this.value);
	try { localStorage.setItem("bc_voice", this.value); } catch (e) { /* ignore */ }
});

// Auditioning the voice needs a real tap anyway, which doubles as the user gesture speech needs.
document.getElementById("voice_test").addEventListener("click", function () {
	var was_enabled = opt_voice.checked;
	if (!was_enabled) Announcer.set_enabled(true);
	Announcer.preview(pilots[0].name);
	if (!was_enabled) setTimeout(function () { Announcer.set_enabled(false); }, 12000);
});

lang_select.addEventListener("change", function () {
	Announcer.set_language(this.value);
	try {
		localStorage.setItem("bc_lang", this.value);
		localStorage.removeItem("bc_voice");   // the stored voice belongs to the previous language
	} catch (e) { /* ignore */ }
	Announcer.populate_voices(voice_select, on_voices_ready);
});

var opt_beep = document.getElementById("opt_beep");
try { if (localStorage.getItem("bc_beep") == "0") opt_beep.checked = false; } catch (e) { /* ignore */ }
Beep.set_enabled(opt_beep.checked);
opt_beep.addEventListener("change", function () {
	Beep.set_enabled(this.checked);
	try { localStorage.setItem("bc_beep", this.checked ? "1" : "0"); } catch (e) { /* ignore */ }
	if (this.checked) Beep.lap(0);   // immediate confirmation that sound works (the audio context starts on a gesture)
});

var opt_voice = document.getElementById("opt_voice");
try { if (localStorage.getItem("bc_callouts") == "0") opt_voice.checked = false; } catch (e) { /* ignore */ }
Announcer.set_enabled(opt_voice.checked);
opt_voice.addEventListener("change", function () {
	Announcer.set_enabled(this.checked);
	try { localStorage.setItem("bc_callouts", this.checked ? "1" : "0"); } catch (e) { /* ignore */ }
});

var opt_awake = document.getElementById("opt_awake");
var awake_state = document.getElementById("awake_state");
try { if (localStorage.getItem("keep_awake") == "0") opt_awake.checked = false; } catch (e) { /* ignore */ }

function refresh_awake() {
	try { localStorage.setItem("keep_awake", opt_awake.checked ? "1" : "0"); } catch (e) { /* ignore */ }
	if (opt_awake.checked) NoSleep.enable(); else NoSleep.disable();
	report_awake();
}

// Report what is actually holding the screen on, not what was asked for.
function report_awake() {
	if (!opt_awake.checked) { awake_state.innerText = ""; awake_state.style.color = ""; return; }
	var state = NoSleep.status();
	if (state.ok) {
		awake_state.style.color = "";
		awake_state.innerText = state.method == "wakelock" ? "Screen is held on by the system wake lock." : "Screen is held on by background video playback.";
	} else {
		awake_state.style.color = "var(--warn)";
		awake_state.innerText = "Screen lock is not active yet — tap anywhere on the page." + (state.detail ? " (" + state.detail + ")" : "");
	}
}
setInterval(report_awake, 3000);
opt_awake.addEventListener("change", refresh_awake);

// The wake lock, speech and the audio context all refuse to start until the page has been
// interacted with, so arm them together on the first tap.
function arm_on_gesture() {
	if (opt_beep.checked && !Beep.is_ready()) Beep.arm();
	if (opt_awake.checked && !NoSleep.is_enabled()) {
		NoSleep.enable();
		setTimeout(report_awake, 300);
	}
}
document.addEventListener("click", arm_on_gesture);
document.addEventListener("touchend", arm_on_gesture);

refresh_awake();
build_roster();
render_roster();
render();
render_lap_tables();
set_state_ui();
