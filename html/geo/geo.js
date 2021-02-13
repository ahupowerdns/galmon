//
//
//

var fileWorld = 'world.geojson';
var fileAlmanac = '../almanac.json'
var fileObservers = '../observers.json'

var projectionChoices = [
	'Fahey',			// The prefered one
	'Aitoff',			// Interesting; but like Fahey
//	'Orthographic',			// 3d globe - not perfectly coded for now XXX
	'CylindricalStereographic',	// Beyond square
	'Equirectangular',		// The boring one
]

var projectionChoice = 0;

var constellation_state = {G: true, E: true, C: true, I: false, J: false, R: true};
var coverage_map_state = true;
var observer_map_state = true;
var display_all_state = false;
var globe_rotate = {lambda: 0.0, phi: 0.0};
var globe_rotate_center = {lambda: 0.0, phi: 0.0};

//
//
//

var svgWorld = d3.select('#svgworld');
var idWorld = document.getElementById('svgworld');

var svgGraticule = d3.select('#svggraticule');
var idGraticule = document.getElementById('svggraticule');

var svgObservers = d3.select('#svgobservers');
var idObservers = document.getElementById('svgobservers');

var svgAlmanac = d3.select('#svgalmanac');
var idAlmanac = document.getElementById('svgalmanac');

var geoPath;
var aProjection;

var speed = 1e-2;
var start = now();

function now()
{
	return Math.round(Date.now() / 1000);
}

function draw_world(data_world)
{
	svgWorld.html("");

	svgWorld.selectAll("path")
		.data(data_world.features)
		.enter()
			.append("path")
				.attr("class", "countries")
				.attr("d", geoPath);
}

function draw_graticule()
{
	var graticule = d3.geoGraticule();

	svgGraticule.html("");

	svgGraticule.selectAll("path")
		.data(graticule.lines())
		.enter()
			.append("path")
				.attr("class", "graticule line")
				.attr("id", function(d) {
					var c = d.coordinates;
					if (c[0][0] == c[1][0]) {
						return (c[0][0] < 0) ? -c[0][0] + "W" : +c[0][0] + "E";
					} else if (c[0][1] == c[1][1]) {
						return (c[0][1] < 0) ? -c[0][1] + "S" : c[0][1] + "N";
					}
				})
				.attr("d", geoPath);
	var draw_text = false;
	if (draw_text) {
		svgGraticule.selectAll('text')
			.data(graticule.lines())
			.enter()
				.append("text")
					.text(function(d) {
						var c = d.coordinates;
						if ((c[0][0] == c[1][0]) && (c[0][0] % 30 == 0)) {
							return (c[0][0]);
						} else if (c[0][1] == c[1][1]) {
							return (c[0][1]);
						}
					})
					.attr("class", "label")
					.attr("style", function(d) {
						var c = d.coordinates;
						return (c[0][1] == c[1][1]) ? "text-anchor: end" : "text-anchor: middle";
					})
					.attr("dx", function(d) {
						var c = d.coordinates;
						return (c[0][1] == c[1][1]) ? -10 : 0;
					})
					.attr("dy", function(d) {
						var c = d.coordinates;
						return (c[0][1] == c[1][1]) ? 4 : 10;
					})
					.attr('transform', function(d) {
						var c = d.coordinates;
						return ('translate(' + aProjection(c[0]) + ')')
					});
	}

	var draw_outline = true;
	if (draw_outline) {
		svgGraticule.append("path")
			.datum(graticule.outline)
			.attr("class", "graticule outline")
			.attr("d", geoPath);
	}
}

function get_almanac_valid(data_almanac)
{
	var a = [];
	Object.keys(data_almanac).forEach(function(e) {
		var o = data_almanac[e];
		o.sv = e;
		if (o["eph-latitude"] != null) {
			o.eph_latitude = o["eph-latitude"];	// json variables with dashes - bad bad
			o.eph_longitude = o["eph-longitude"];
			a.push(o);
		}
	});
	return a;
}

var Tooltip;

function create_tooltop()
{
	if (Tooltip) {
		return;
	}
	// create a tooltip
	Tooltip = d3.select("#combined")
		.append("span")
			.attr("class", "tooltip")
			.attr("id", "tooltip")
			.style("opacity", 0);
}

function to2(num)
{
	return parseFloat(num).toFixed(2);
}

function draw_radials(satellite, observer)
{
	// in order to draw the line between observer and satellite we need long/lat vs x/y
	var observer_center = aProjection.invert(geoPath.centroid(observer));			// find the center of the projected rectangle
	var satellite_center = aProjection.invert([satellite.attr("cx"), satellite.attr("cy")]);	// center of satellite
	svgObservers.append("path")
		.attr("class", "radials")
		.attr("d", function(r) { return geoPath({type: "LineString", coordinates: [observer_center, satellite_center]}); });
}

function draw_satellite_to_operator(d)
{
	// get list of observers for this satellite
	satellite = d3.select("#Satellite_" + d.name + "_bird");
	a = observers_list_almanac_raw(d);
	for (aa=0;aa<a.length;aa++) {
		observer_id = a[aa];
		observer = observer_shape[observer_id];
		if (!observer) {
			// should not happen
			continue;
		}
		draw_radials(satellite, observer);
	}
}

function draw_operator_to_satellite(d)
{
	observer_id = d.id;
	// get list of satellites for this observer
	observer = observer_shape[observer_id];
	if (!observer) {
		// should not happen
		return;
	}

	a = svs_list_observer_raw(d);
	for (aa=0;aa<a.length;aa++) {
		satellite_id = a[aa];
		// check the satellite is seen in almanac data - ie. double check.
		if (!data_almanac[satellite_id] || !data_almanac[satellite_id].observed) {
			// no line needed
			continue;
		}
		var s = $("[id^='Satellite_" + satellite_id + "_']");
		if (!s.is(":visible")) {
			continue;
		}
		satellite = d3.select("#Satellite_" + satellite_id + "_bird");
		draw_radials(satellite, observer);
	}
}

function color_of(r)
{
	switch (r.gnssid) {
		case 0: return "green";		// GPS
		case 1: return "gray";		// SBAS - not coded
		case 2: return "blue";		// Galileo
		case 3: return "red";		// BeiDou
		case 4: return "gray";		// IMES - not coded
		case 5: return "gray";		// QZSS - not coded
		case 6: return "yellow";	// GLONASS
	}
	return "magenta";			// - should not happen
}

var tooltop_deltax = 10; // was 30

function draw_almanac(data_almanac)
{
	var arr = get_almanac_valid(data_almanac);

	svgAlmanac.html("");

	// Three function that change the tooltip when user hover / move / leave a cell
	var mouseover = function(d) {
		var o = observers_list_almanac(d);
		s = d.name + " [" + to2(d.eph_longitude) + "," + to2(d.eph_latitude) + "] " + ((o == "") ? "" : " seen by " + o);
		Tooltip.html(s)
			.style("opacity", 1)
			.style("left", (d3.mouse(this)[0] + tooltop_deltax) + "px")
			.style("top", (d3.mouse(this)[1]) + "px");
		d3.selectAll(".radials").remove();
		draw_satellite_to_operator(d);
	}
	var mousemove = function(d) {
		var o = observers_list_almanac(d);
		s = d.name + " [" + to2(d.eph_longitude) + "," + to2(d.eph_latitude) + "] " + ((o == "") ? "" : " seen by " + o);
		Tooltip.html(s)
			.style("left", (d3.mouse(this)[0] + tooltop_deltax) + "px")
			.style("top", (d3.mouse(this)[1]) + "px");
	}
	var mouseleave = function(d) {
		Tooltip.html("")
			.style("opacity", 0);
		d3.selectAll(".radials").remove();
		display_all_refresh();
	}

	// text first as we want the satellite circles to be always above them!
	svgAlmanac.selectAll("text")
		.data(arr)
		.enter()
		.append("text")
			.text(r => r.sv)
			.attr("class", "labels")
			.attr("id", r => "Satellite_" + r.name + "_label")
			.attr("dx", r => (aProjection([r.eph_longitude, r.eph_latitude])[0] + 0))
			.attr("dy", r => (aProjection([r.eph_longitude, r.eph_latitude])[1] + 0))
			.attr("fill", r => ((r.observed) ? "black" : "#666666"))
			.attr("text-anchor", r => ((r.eph_longitude > 0) ? "start" : "end"))
			.attr("baseline-shift", r => ((r.eph_latitude > 0) ? "+30%" : "-90%"))
			.attr("fill-opacity", r => ((r.observed) ? "1.0" : ".5"))
			.attr("stroke-opacity", r => ((r.observed) ? "1.0" : ".5"))
			.attr("font-weight", r => ((r.observed) ? "bold" : null));

	svgAlmanac.selectAll("circle")
		.data(arr)
		.enter()
		.append("circle")
			.attr("class", "satellites")
			.attr("id", r => "Satellite_" + r.name + "_bird")
			.attr("r", 3)
			.attr("cx", r => aProjection([r.eph_longitude, r.eph_latitude])[0])
			.attr("cy", r => aProjection([r.eph_longitude, r.eph_latitude])[1])
			.attr("fill", r => color_of(r))
			.attr("fill-opacity", r => ((r.observed) ? "1.0" : ".5"))
			.attr("stroke-opacity", r => ((r.observed) ? "1.0" : ".5"))
			.on("mouseover", mouseover)
			.on("mousemove", mousemove)
			.on("mouseleave", mouseleave);

}

function age_in_seconds(t)
{
	return now() - t;
}

function a_to_s(a)
{
	var r = "";
	for (aa=0;aa<a.length;aa++) {
		r += a[aa] + " ";
	}
	if (r.length > 0) {
		r = r.slice(0, -1);
	}
	return r;
}

function svs_list_observer_raw(d)
{
	var r = []
	var svs = d.svs;
	for (s in svs) {
		// check the satellite is seen in almanac data - ie. double check.
		if (data_almanac[svs[s].name]) {
			r.push(svs[s].name);
		}
	}
	return r;
}

function observers_list_almanac_raw(d)
{
	var a = [];
	var s_id = d.name;

	for (oo=0;oo<data_observers.length;oo++) {
		o = data_observers[oo];
		o_id = o.id;
		for (s in o.svs) {
			if (s_id == o.svs[s].name) {
				a.push(o_id);
				break;
			}
		}
	}
	return a;
}

function observers_list_almanac(d)
{
	return a_to_s(observers_list_almanac_raw(d));
}

function svs_list_observer(d)
{
	return a_to_s(svs_list_observer_raw(d));
}

function observer_up(r)
{
	var considered_old = 15 * 60;	// 15 mins

	if (age_in_seconds(r["last-seen"]) > considered_old) {
		// data is old
		return false;
	}
	if (Object.keys(r.svs).length == 0) {
		// nothing visible
		return false;
	}
	return true;
}

var observer_shape = [];

function draw_observers(data_observers)
{
	// Three function that change the tooltip when user hover / move / leave a cell
	var mouseover = function(d) {
		var o = svs_list_observer(d);
		s = d.id + ": [" + to2(d.longitude) + "," + to2(d.latitude) + "]" + ((o == "") ? "" : " sees " + svs_list_observer(d));
		Tooltip.html(s)
			.style("opacity", 1)
			.style("left", (d3.mouse(this)[0] + tooltop_deltax) + "px")
			.style("top", (d3.mouse(this)[1]) + "px");
		d3.selectAll(".radials").remove();
		draw_operator_to_satellite(d);

		// redraw only the one coverage circle
		coverages = $("[id^='Coverage_']");
		coverages.hide();
		coverages = $("[id^='Coverage_" + d.id +  "_area']");
		coverages.show();
	}
	var mousemove = function(d) {
		var o = svs_list_observer(d);
		s = d.id + ": [" + to2(d.longitude) + "," + to2(d.latitude) + "]" + ((o == "") ? "" : " sees " + svs_list_observer(d));
		Tooltip.html(s)
			.style("left", (d3.mouse(this)[0] + tooltop_deltax) + "px")
			.style("top", (d3.mouse(this)[1]) + "px");
	}
	var mouseleave = function(d) {
		Tooltip.html("")
			.style("opacity", 0);
		d3.selectAll(".radials").remove();
		display_all_refresh();
		coverage_map_refresh();
	}

	// text first as we want the observer rectangle to be always above them!
	svgAlmanac.selectAll("olables")
		.data(data_observers)
		.enter()
		.append("text")
			.text(r => r.id)
			.attr("class", r => (observer_up(r) ? "observers" : "observers down"))
			.attr("id", r => "Observer_" + r.id + "_label")
			.attr("dx", r => (aProjection([r.longitude, r.latitude])[0]))
			.attr("dy", r => (aProjection([r.longitude, r.latitude])[1]))
			.attr("fill", r => (observer_up(r) ? "black" : "#666666"))
			.attr("text-anchor", r => ((r.longitude > 0) ? "start" : "end"))
			.attr("baseline-shift", r => ((r.latitude > 0) ? "+30%" : "-90%"))
			.attr("fill-opacity", r => (observer_up(r) ? "1.0" : ".5"))
			.attr("stroke-opacity", r => (observer_up(r) ? "1.0" : ".5"))
			.attr("font-weight", r => (observer_up(r) ? "bold" : null));

	// we draw a geo correct observer rectangle - mapped onto whatever globe we have projected
	var observer_degrees = 2.5;
	svgAlmanac.selectAll("div")
		.data(data_observers)
		.enter()
			.append("path")
			.attr("class", r => (observer_up(r) ? "observers" : "observers down"))
			.attr("id", r => ("Observer_" + r.id + "_location"))
			.attr("d", function(r) {
				var path = {
					type: "LineString",
					coordinates: [
						[r.longitude - observer_degrees/2, r.latitude - observer_degrees/2],
						[r.longitude - observer_degrees/2, r.latitude + observer_degrees/2],
						[r.longitude + observer_degrees/2, r.latitude + observer_degrees/2],
						[r.longitude + observer_degrees/2, r.latitude - observer_degrees/2],
					]
				};
				observer_shape[r.id] = path;	// save away for later
				return geoPath(path);
			})
			.on("mouseover", mouseover)
			.on("mousemove", mousemove)
			.on("mouseleave", mouseleave);

	// kick off the annimation - if needed
	annimate_down_observers();
}

var down_timer;

function annimate_down_observers()
{
	clearTimeout(down_timer);

	var down = document.getElementsByClassName('observers down');
	if (down.length > 0) {
		// if we have an observer that is down - lets annoutate it!
		for (var ii=0, ll=down.length; ii<ll; ii++){
			//if (down[ii].classList.contains('down')) {
			//	down[ii].classList.remove('down');
			//} else {
			//	down[ii].classList.add('down');
			//}
			down[ii].style.fill = (down[ii].style.fill == "black") ? "red" : "black";
			down[ii].style.stroke = (down[ii].style.stroke == "black") ? "red" : "black";
		}
	}
	var seconds = 2;
	down_timer = setTimeout(annimate_down_observers, seconds * 1000.0);
}

function draw_observers_coverage(data_observers)
{
	var radius = 65;			// XXX fix
	var geoCircle = d3.geoCircle();

	svgObservers.html("");

	// we draw a geo correct circle
	svgObservers.selectAll("div")
		.data(data_observers)
		.enter()
			.append("path")
			.attr("class", r => (observer_up(r) ? "coverage" : "coverage down"))
			.attr("id", r => ("Coverage_" + r.id + "_area"))
			.attr("d", function(r) { return geoPath(geoCircle.center([r.longitude, r.latitude]).radius(radius)());
			});
}

var data_almanac = null;
var data_observers = null;
var time_last_data_received = 0;

function do_update_almanac_observers(error, results)
{
	var updated_almanac = false;
	var updated_observers = false;

	if (results && results.length > 0 && results[0]) {
		data_almanac = results[0];
		updated_almanac = true;
		time_last_data_received = now();
	}
	if (results.length > 1 && results[1]) {
		data_observers = results[1];
		updated_observers = true;
		time_last_data_received = now();
	}

	// XXX cheat for now - we need until we fix d3/svg bug above
	updated_almanac = true;
	updated_observers = true;

	if (updated_almanac) {
		// We write into the svgalmanac area - so clean it and rewrite it
		if (draw_almanac) {
			draw_almanac(data_almanac);
		}
		// now hide/show the ones that should be seen
		constellation_refresh();
	}
	if (updated_observers) {
		if (data_observers) {
			draw_observers_coverage(data_observers)
			draw_observers(data_observers);
		}
		coverage_map_refresh();
		observer_map_refresh();
	}
	display_all_refresh();
}

var redisplay_timer = null;
var display_observers_count = 0;

function stop_redisplay_timer()
{
	clearTimeout(redisplay_timer);
	redisplay_timer = null;
}

function do_redisplay_timer()
{
	stop_redisplay_timer();

	if ((now() - time_last_data_received) >= 55) {
		// refresh data from afar
		if (display_observers_count == 0) {
			// observers does not need that much updating!
			d3.queue(1)
				.defer(d3.json, fileAlmanac + '?t=' + now())
				.defer(d3.json, fileObservers + '?t=' + now())
				.awaitAll(do_update_almanac_observers);
			display_observers_count = 10;
		} else {
			// just queue an update to almanac
			d3.queue(1)
				.defer(d3.json, fileAlmanac + '?t=' + now())
				.awaitAll(do_update_almanac_observers);
		}
		display_observers_count--;
	} else {
		// just use existing data
		do_update_almanac_observers(null, []);
	}

	var seconds = 60;
	redisplay_timer = setTimeout(do_redisplay_timer, seconds * 1000.0);
}

function set_projection(data_world)
{
	// var aProjection = d3.geoMercator().scale(100).translate([250, 250]);
	// all this complexity is so we can scale to full screen.
	// see: https://stackoverflow.com/questions/14492284/center-a-map-in-d3-given-a-geojson-object

	var center = [0, 0];		// This is very Euro-centric - but that's how these projections works.
	var scale = 191;		// No idea what this does

	var svgCombined = d3.select('#combined');
	var idCombined = document.getElementById("combined");

	svgWorld = d3.select("#svgworld");
	idWorld = document.getElementById("svgworld");

	svgGraticule = d3.select("#svggraticule");
	idGraticule = document.getElementById("svggraticule");

	var offset = [idCombined.clientWidth/2, idCombined.clientHeight/2];

	switch(projectionChoices[projectionChoice]) {
	default:
		// fall thru to Equirectangular
	case 'Equirectangular':
		aProjection = d3.geoEquirectangular()
				.scale(scale)
				.translate(offset);
		break;
	case 'Aitoff':
		aProjection = d3.geoAitoff()
				.scale(scale)
				.translate(offset);
		break;
	case 'CylindricalStereographic':
		aProjection = d3.geoCylindricalStereographic()
				.scale(scale)
				.translate(offset);
		break;
	case 'Fahey':
		aProjection = d3.geoFahey()
				.scale(scale)
				.translate(offset);
		break;
	case 'Orthographic':
		aProjection = d3.geoOrthographic()
				.scale(scale)
				.translate(offset);
		break;
	}

	geoPath = d3.geoPath()
			.projection(aProjection);

	// using the path determine the bounds of the current map and use
	// these to determine better values for the scale and translation
	var bounds = geoPath.bounds(data_world);
	var hscale = scale * (idCombined.clientWidth - 40) / (bounds[1][0] - bounds[0][0]);
	var vscale = scale * (idCombined.clientHeight - 40) / (bounds[1][1] - bounds[0][1]);
	scale = (hscale < vscale) ? hscale : vscale;
	var offset = [
			idCombined.clientWidth - (bounds[0][0] + bounds[1][0])/2,
			idCombined.clientHeight - (bounds[0][1] + bounds[1][1])/2
			];

	if (0) {
		// new projection
		switch(projectionChoices[projectionChoice]) {
		default:
			// fall thru to Equirectangular
		case 'Equirectangular':
			aProjection = d3.geoEquirectangular()
					.center(center)
					.scale(scale)
					.translate(offset);
			break;
		case 'Aitoff':
			aProjection = d3.geoAitoff()
					.center(center) .scale(scale)
					.translate(offset);
			break;
		case 'CylindricalStereographic':
			aProjection = d3.geoCylindricalStereographic()
					.center(center)
					.scale(scale)
					.translate(offset);
			break;
		case 'Fahey':
			aProjection = d3.geoFahey()
					.center(center)
					.scale(scale)
					.translate(offset);
			break;
		case 'Orthographic':
			aProjection = d3.geoOrthographic()
					.center(center)
					.scale(scale)
					.translate(offset);
			break;
		}
	}

	svgCombined.attr("width", idCombined.clientWidth);
	svgCombined.attr("height", idCombined.clientHeight);

	padding = 0 // 20 - see svg: padding in css

	svgWorld.attr("width", idCombined.clientWidth - padding * 2);
	svgWorld.attr("height", idCombined.clientHeight - padding * 2);

	svgObservers.attr("width", idCombined.clientWidth - padding * 2);
	svgObservers.attr("height", idCombined.clientHeight - padding * 2);

	svgGraticule.attr("width", idCombined.clientWidth - padding * 2);
	svgGraticule.attr("height", idCombined.clientHeight - padding * 2);

	svgAlmanac.attr("width", idCombined.clientWidth - padding * 2);
	svgAlmanac.attr("height", idCombined.clientHeight - padding * 2);

}

function do_draw_world(data_world)
{
	set_projection(data_world);

	aProjection.rotate([globe_rotate.lambda, globe_rotate.phi]);	// show globe where it belongs for current timezone

	create_tooltop();

	draw_world(data_world);
	draw_graticule();

	if (0) {
		// XXX recode - this is bad.
		if (projectionChoice == 'Orthographic') {
			// rotating globe is done with a constant timer
			d3.timer(function() {
				var lambda = speed * (Date.now() - start);	// λ
				var phi = -15;					// φ
				aProjection.rotate([lambda + 180, -phi]);

				draw_world(data_world);
				draw_graticule();
				do_update_almanac_observers(null, [])
			});
		}
		// XXX this will work one day ...
		// var node = {id: "rotate_west"};	// The "id" is the only part used.
		// rotate_globe(node)
	}
}

var data_world = null;

function read_world()
{
	if (data_world == null) {
		d3.json(fileWorld, function(result) {
			data_world = result;
			do_draw_world(data_world);
			// after the world is read in and displayed - then start the timers!
			// we don't redraw the world!
			do_redisplay_timer();
		});
	} else {
		// after the world is read in and displayed - then start the timers!
		// we don't redraw the world!
		do_draw_world(data_world);
		do_redisplay_timer();
	}
}

function set_rotate_from_tz()
{
	var offset = new Date().getTimezoneOffset();	// in minutes from UTC

	globe_rotate_center.lambda = (offset/(60.0*24.0)) * 360.0;
	globe_rotate_center.phi = 0.0;

	globe_rotate.lambda = globe_rotate_center.lambda;
	globe_rotate.phi = globe_rotate_center.phi;
}

function load_settings()
{
	if(typeof(Storage) !== "undefined")
	{
		var storage_constellations = localStorage.getItem("geo-settings-constellations");
		if(storage_constellations != null)
		{
			try
			{
				constellation_state = JSON.parse(storage_constellations);
				/* Set Menubar checkboxes */
				for(const constellation in constellation_state)
				{
					if(document.getElementById('constellation-' + constellation))
					{
						document.getElementById('constellation-' + constellation).childNodes[1].checked = constellation_state[constellation];
					}
				}
			}
			catch(e)
			{
				console.log("Error parsing storage_constellations!",e);
			}
		}

		var storage_map = localStorage.getItem("geo-settings-map");
		if(storage_map != null)
		{
			try
			{
				var _map_settings = JSON.parse(storage_map);
				coverage_map_state = _map_settings["coverage"];
				observer_map_state = _map_settings["observer"];
				display_all_state = _map_settings["display_all"];
				/* Set Menubar checkboxes */
				document.getElementById("coverage_map").childNodes[1].checked = coverage_map_state;
				document.getElementById("observer_map").childNodes[1].checked = observer_map_state;
				document.getElementById("display_all").childNodes[1].checked = display_all_state;
			}
			catch(e)
			{
				console.log("Error parsing storage_map!",e);
			}
		}

		var storage_globe = localStorage.getItem("geo-settings-globe");
		if(storage_globe != null)
		{
			try
			{
				var _globe_settings = JSON.parse(storage_globe);
				globe_rotate = _globe_settings["rotate"];
				globe_rotate_center = _globe_settings["rotate_center"];
			}
			catch(e)
			{
				console.log("Error parsing storage_globe!",e);
			}
		}

		var storage_projection = localStorage.getItem("geo-settings-projection");
		if(storage_projection != null)
		{
			try
			{
				projectionChoice = JSON.parse(storage_projection);
			}
			catch(e)
			{
				console.log("Error parsing storage_projection!",e);
			}
		}

		/* Save defaults even if we didn't load anything */
		save_settings();
	}
}

function save_settings()
{
	if(typeof(Storage) !== "undefined")
	{
		localStorage.setItem("geo-settings-constellations", JSON.stringify(constellation_state));

		var _map_settings = {
			"coverage": coverage_map_state,
			"observer": observer_map_state,
			"display_all": display_all_state
		};
		localStorage.setItem("geo-settings-map", JSON.stringify(_map_settings));

		var _globe_settings = {
			"rotate": globe_rotate,
			"rotate_center": globe_rotate_center
		};
		localStorage.setItem("geo-settings-globe", JSON.stringify(_globe_settings));

		localStorage.setItem("geo-settings-projection", JSON.stringify(projectionChoice));
	}
}

function geo_start()
{
	set_rotate_from_tz();
	load_settings();
	read_world();
}

geo_start();

// d3.select("body").onresize = do_redisplay_timer;

function constellation_click(node)
{
	// Satellites are named LETTER+NUMBERS
	var c = node.innerText.trim();
	if (c == "GPS") { c = "G"; }		// G = GPS (American)
	else if (c == "Galileo") { c = "E"; }	// E = Europe
	else if (c == "BeiDou") { c = "C"; }	// C = China
	else if (c == "IMES") { c = "I"; }	// I = Indoor QZSS (doubtfully seen)
	else if (c == "QZSS") { c = "J"; }	// J = Japan
	else if (c == "GLONASS") { c = "R"; }	// R = Russia

	satellites = $("[id^='Satellite_" + c + "']");

	if (node.childNodes[1].checked) {
		satellites.show();
		constellation_state[c] = true;
	} else {
		satellites.hide();
		constellation_state[c] = false;
	}
	d3.selectAll(".radials").remove();
	display_all_refresh();
	save_settings();
}

function constellation_refresh()
{
	for (c in constellation_state) {
		satellites = $("[id^='Satellite_" + c + "']");
		if (constellation_state[c]) {
			satellites.show();
		} else {
			satellites.hide();
		}
	}
}

function coverage_map_click(node)
{
	if (node.childNodes[1].checked) {
		coverage_map_state = true;
	} else {
		coverage_map_state = false;
	}
	coverage_map_refresh();
	save_settings();
}

function coverage_map_refresh()
{
	coverages = $("[id^='Coverage_']");
	if (coverage_map_state) {
		coverages.show();
	} else {
		coverages.hide();
	}
}

function observer_map_click(node)
{
	if (node.childNodes[1].checked) {
		observer_map_state = true;
	} else {
		observer_map_state = false;
	}
	observer_map_refresh();
	save_settings();
}

function observer_map_refresh()
{
	observers = $("[id^='Observer_']");
	if (observer_map_state) {
		observers.show();
	} else {
		observers.hide();
	}
}

function display_all_click(node)
{
	if (node.childNodes[1].checked) {
		display_all_state = true;
	} else {
		display_all_state = false;
	}
	display_all_refresh();
	save_settings();
}

function display_all_refresh()
{
	if (display_all_state) {
		for (d in data_observers) {
			draw_operator_to_satellite(data_observers[d]);
		}
	} else {
		d3.selectAll(".radials").remove();
	}
}

function show_history_click(node)
{
	var c = node.innerText.trim();
	if (node.childNodes[1].checked) {
		// XXX
		console.log(c + " :checked");
	} else {
		// XXX
		console.log(c + " :not checked");
	}
}

function show_animate_click(node)
{
	var c = node.innerText.trim();
	if (node.childNodes[1].checked) {
		// XXX
		console.log(c + " :checked");
	} else {
		// XXX
		console.log(c + " :not checked");
	}
}

function update_projection_click(node)
{
	// cycle thru known globe projections
	projectionChoice++;
	if (projectionChoice >= projectionChoices.length)
		projectionChoice = 0;
	// now redraw everything!
	read_world();
	save_settings();
}

function rotate_globe(node)
{
	var t = node.id;
	if (t == "rotate_north") {
		globe_rotate.phi += 10;				// North
	} else if (t == "rotate_west") {
		globe_rotate.lambda -= 5;			// West
	} else if (t == "rotate_center") {
		globe_rotate.lambda = globe_rotate_center.lambda;
		globe_rotate.phi = globe_rotate_center.phi;	// Reset to center
	} else if (t == "rotate_east") {
		globe_rotate.lambda += 5;			// East
	} else if (t == "rotate_south") {
		globe_rotate.phi -= 10;				// South
        }

	read_world();
	save_settings();
}

// JQuery also has a startup 

$(document).ready(function () {
	$("[id^='constellation']").click(function() { constellation_click(this); });
	$('#coverage_map').click(function() { coverage_map_click(this); });
	$('#observer_map').click(function() { observer_map_click(this); });

	$('#display_all').click(function() { display_all_click(this); });
	$('#show_history').click(function() { show_history_click(this); });
	$('#show_animate').click(function() { show_animate_click(this); });
	$('#update_projection').click(function() { update_projection_click(this); });

	$("[id^='rotate_']").click(function() { rotate_globe(this); });
});

