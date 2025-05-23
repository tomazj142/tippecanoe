#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cmath>
#include <sys/types.h>
#include <sys/wait.h>
#include <sqlite3.h>
#include <limits.h>
#include "main.hpp"
#include "mvt.hpp"
#include "mbtiles.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include "serial.hpp"
#include "errors.hpp"
#include "thread.hpp"

extern "C" {
#include "jsonpull/jsonpull.h"
}

#include "plugin.hpp"
#include "write_json.hpp"
#include "read_json.hpp"

struct writer_arg {
	int write_to;
	std::vector<mvt_layer> *layers;
	unsigned z;
	unsigned x;
	unsigned y;
	int extent;
};

void *run_writer(void *a) {
	writer_arg *wa = (writer_arg *) a;

	FILE *fp = fdopen(wa->write_to, "w");
	if (fp == NULL) {
		perror("fdopen (pipe writer)");
		exit(EXIT_OPEN);
	}

	json_writer state(fp);
	for (size_t i = 0; i < wa->layers->size(); i++) {
		layer_to_geojson((*(wa->layers))[i], wa->z, wa->x, wa->y, false, true, false, true, 0, 0, 0, true, state, 0, std::set<std::string>());
	}

	if (fclose(fp) != 0) {
		if (errno == EPIPE) {
			static bool warned = false;
			if (!warned) {
				fprintf(stderr, "Warning: broken pipe in postfilter\n");
				warned = true;
			}
		} else {
			perror("fclose output to filter");
			exit(EXIT_CLOSE);
		}
	}

	return NULL;
}

// Reads from the postfilter
std::vector<mvt_layer> parse_layers(int fd, int z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, int extent) {
	FILE *f = fdopen(fd, "r");
	if (f == NULL) {
		perror("fdopen filter output");
		exit(EXIT_OPEN);
	}

	std::vector<mvt_layer> out = parse_layers(f, z, x, y, extent, false);

	if (fclose(f) != 0) {
		perror("fclose postfilter output");
		exit(EXIT_CLOSE);
	}

	for (auto const &layer : out) {
		std::string layername = layer.name;

		std::map<std::string, layermap_entry> &layermap = (*layermaps)[tiling_seg];
		if (layermap.count(layername) == 0) {
			layermap_entry lme = layermap_entry(layermap.size());
			lme.minzoom = z;
			lme.maxzoom = z;

			layermap.insert(std::pair<std::string, layermap_entry>(layername, lme));

			if (lme.id >= (*layer_unmaps)[tiling_seg].size()) {
				(*layer_unmaps)[tiling_seg].resize(lme.id + 1);
				(*layer_unmaps)[tiling_seg][lme.id] = layername;
			}
		}

		auto ts = layermap.find(layername);
		if (ts == layermap.end()) {
			fprintf(stderr, "Internal error: layer %s not found\n", layername.c_str());
			exit(EXIT_IMPOSSIBLE);
		}
		if (z < ts->second.minzoom) {
			ts->second.minzoom = z;
		}
		if (z > ts->second.maxzoom) {
			ts->second.maxzoom = z;
		}

		for (auto const &feature : layer.features) {
			if (feature.type == mvt_point) {
				ts->second.points++;
			} else if (feature.type == mvt_linestring) {
				ts->second.lines++;
			} else if (feature.type == mvt_polygon) {
				ts->second.polygons++;
			}

			for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
				const std::string &key = layer.keys[feature.tags[i]];
				const mvt_value &val = layer.values[feature.tags[i + 1]];

				// Nulls can be excluded here because this is the postfilter
				// and it is nearly time to create the vector representation

				if (val.type != mvt_null) {
					add_to_tilestats(ts->second.tilestats, key, mvt_value_to_serial_val(val));
				}
			}
		}
	}

	return out;
}

// Reads from the prefilter
serial_feature parse_feature(json_pull *jp, int z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, bool postfilter, key_pool &key_pool) {
	serial_feature sf;

	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "Filter output:%d: %s: ", jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				} else {
					fprintf(stderr, "\n");
				}
				exit(EXIT_JSON);
			}

			json_free(jp->root);
			sf.t = -1;
			return sf;
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}
		if (strcmp(type->value.string.string, "Feature") != 0) {
			continue;
		}

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "Filter output:%d: filtered feature with no geometry: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "Filter output:%d: feature without properties hash: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		json_object *geometry_type = json_hash_get(geometry, "type");
		if (geometry_type == NULL) {
			fprintf(stderr, "Filter output:%d: null geometry (additional not reported): ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		if (geometry_type->type != JSON_STRING) {
			fprintf(stderr, "Filter output:%d: geometry type is not a string: ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		json_object *coordinates = json_hash_get(geometry, "coordinates");
		if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
			fprintf(stderr, "Filter output:%d: feature without coordinates array: ", jp->line);
			json_context(j);
			exit(EXIT_JSON);
		}

		int t;
		for (t = 0; t < GEOM_TYPES; t++) {
			if (strcmp(geometry_type->value.string.string, geometry_names[t]) == 0) {
				break;
			}
		}
		if (t >= GEOM_TYPES) {
			fprintf(stderr, "Filter output:%d: Can't handle geometry type %s: ", jp->line, geometry_type->value.string.string);
			json_context(j);
			exit(EXIT_JSON);
		}

		drawvec dv;
		parse_coordinates(t, coordinates, dv, VT_MOVETO, "Filter output", jp->line, j);
		if (mb_geometry[t] == VT_POLYGON) {
			dv = fix_polygon(dv, false, false);
		}

		// Scale and offset geometry from global to tile
		double scale = 1LL << geometry_scale;
		for (size_t i = 0; i < dv.size(); i++) {
			unsigned sx = 0, sy = 0;
			if (z != 0) {
				sx = x << (32 - z);
				sy = y << (32 - z);
			}
			dv[i].x = std::round(dv[i].x / scale) * scale - sx;
			dv[i].y = std::round(dv[i].y / scale) * scale - sy;
		}

		if (dv.size() > 0) {
			sf.t = mb_geometry[t];
			sf.segment = tiling_seg;
			sf.geometry = dv;
			sf.seq = 0;
			sf.index = 0;
			sf.bbox[0] = sf.bbox[1] = LLONG_MAX;
			sf.bbox[2] = sf.bbox[3] = LLONG_MIN;
			sf.extent = 0;
			sf.has_id = false;

			std::string layername = "unknown";
			json_object *tippecanoe = json_hash_get(j, "tippecanoe");
			if (tippecanoe != NULL) {
				json_object *layer = json_hash_get(tippecanoe, "layer");
				if (layer != NULL && layer->type == JSON_STRING) {
					layername = std::string(layer->value.string.string);
				}

				json_object *index = json_hash_get(tippecanoe, "index");
				if (index != NULL && index->type == JSON_NUMBER) {
					sf.index = index->value.number.number;
				}

				json_object *sequence = json_hash_get(tippecanoe, "sequence");
				if (sequence != NULL && sequence->type == JSON_NUMBER) {
					sf.seq = sequence->value.number.number;
				}

				json_object *extent = json_hash_get(tippecanoe, "extent");
				if (extent != NULL && extent->type == JSON_NUMBER) {
					sf.extent = extent->value.number.number;
				}

				json_object *dropped = json_hash_get(tippecanoe, "dropped");
				if (dropped != NULL && dropped->type == JSON_TRUE) {
					sf.dropped = FEATURE_DROPPED;  // dropped
				} else {
					sf.dropped = FEATURE_KEPT;  // kept
				}
			}

			for (size_t i = 0; i < dv.size(); i++) {
				if (dv[i].op == VT_MOVETO || dv[i].op == VT_LINETO) {
					if (dv[i].x < sf.bbox[0]) {
						sf.bbox[0] = dv[i].x;
					}
					if (dv[i].y < sf.bbox[1]) {
						sf.bbox[1] = dv[i].y;
					}
					if (dv[i].x > sf.bbox[2]) {
						sf.bbox[2] = dv[i].x;
					}
					if (dv[i].y > sf.bbox[3]) {
						sf.bbox[3] = dv[i].y;
					}
				}
			}

			json_object *id = json_hash_get(j, "id");
			if (id != NULL && id->type == JSON_NUMBER) {
				sf.id = id->value.number.number;
				if (id->value.number.large_unsigned > 0) {
					sf.id = id->value.number.large_unsigned;
				}
				sf.has_id = true;
			}

			std::map<std::string, layermap_entry> &layermap = (*layermaps)[tiling_seg];

			if (layermap.count(layername) == 0) {
				layermap_entry lme = layermap_entry(layermap.size());
				lme.minzoom = z;
				lme.maxzoom = z;

				layermap.insert(std::pair<std::string, layermap_entry>(layername, lme));

				if (lme.id >= (*layer_unmaps)[tiling_seg].size()) {
					(*layer_unmaps)[tiling_seg].resize(lme.id + 1);
					(*layer_unmaps)[tiling_seg][lme.id] = layername;
				}
			}

			auto ts = layermap.find(layername);
			if (ts == layermap.end()) {
				fprintf(stderr, "Internal error: layer %s not found\n", layername.c_str());
				exit(EXIT_IMPOSSIBLE);
			}
			sf.layer = ts->second.id;

			if (z < ts->second.minzoom) {
				ts->second.minzoom = z;
			}
			if (z > ts->second.maxzoom) {
				ts->second.maxzoom = z;
			}

			if (!postfilter) {
				if (sf.t == mvt_point) {
					ts->second.points++;
				} else if (sf.t == mvt_linestring) {
					ts->second.lines++;
				} else if (sf.t == mvt_polygon) {
					ts->second.polygons++;
				}
			}

			for (size_t i = 0; i < properties->value.object.length; i++) {
				serial_val v = stringify_value(properties->value.object.values[i], "Filter output", jp->line, j);

				// Nulls can be excluded here because the expression evaluation filter
				// would have already run before prefiltering

				if (v.type != mvt_null) {
					sf.full_keys.push_back(key_pool.pool(std::string(properties->value.object.keys[i]->value.string.string)));
					sf.full_values.push_back(v);

					if (!postfilter) {
						add_to_tilestats(ts->second.tilestats, std::string(properties->value.object.keys[i]->value.string.string), v);
					}
				}
			}

			json_free(j);
			return sf;
		}

		json_free(j);
	}
}

static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;

void setup_filter(const char *filter, int *write_to, int *read_from, pid_t *pid, unsigned z, unsigned x, unsigned y) {
	// This will create two pipes, a new thread, and a new process.
	//
	// The new process will read from one pipe and write to the other, and execute the filter.
	// The new thread will write the GeoJSON to the pipe that leads to the filter.
	// The original thread will read the GeoJSON from the filter and convert it back into vector tiles.

	if (pthread_mutex_lock(&pipe_lock) != 0) {
		perror("pthread_mutex_lock (pipe)");
		exit(EXIT_PTHREAD);
	}

	int pipe_orig[2], pipe_filtered[2];
	if (pipe(pipe_orig) < 0) {
		perror("pipe (original features)");
		exit(EXIT_OPEN);
	}
	if (pipe(pipe_filtered) < 0) {
		perror("pipe (filtered features)");
		exit(EXIT_OPEN);
	}

	std::string z_str = std::to_string(z);
	std::string x_str = std::to_string(x);
	std::string y_str = std::to_string(y);

	*pid = fork();
	if (*pid < 0) {
		perror("fork");
		exit(EXIT_PTHREAD);
	} else if (*pid == 0) {
		// child

		if (dup2(pipe_orig[0], 0) < 0) {
			perror("dup child stdin");
			exit(EXIT_OPEN);
		}
		if (dup2(pipe_filtered[1], 1) < 0) {
			perror("dup child stdout");
			exit(EXIT_OPEN);
		}
		if (close(pipe_orig[1]) != 0) {
			perror("close output to filter");
			exit(EXIT_CLOSE);
		}
		if (close(pipe_filtered[0]) != 0) {
			perror("close input from filter");
			exit(EXIT_CLOSE);
		}
		if (close(pipe_orig[0]) != 0) {
			perror("close dup input of filter");
			exit(EXIT_CLOSE);
		}
		if (close(pipe_filtered[1]) != 0) {
			perror("close dup output of filter");
			exit(EXIT_CLOSE);
		}

		// XXX close other fds?

		if (execlp("sh", "sh", "-c", filter, "sh", z_str.c_str(), x_str.c_str(), y_str.c_str(), NULL) != 0) {
			perror("exec");
			exit(EXIT_PTHREAD);
		}
	} else {
		// parent

		if (close(pipe_orig[0]) != 0) {
			perror("close filter-side reader");
			exit(EXIT_CLOSE);
		}
		if (close(pipe_filtered[1]) != 0) {
			perror("close filter-side writer");
			exit(EXIT_CLOSE);
		}
		if (fcntl(pipe_orig[1], F_SETFD, FD_CLOEXEC) != 0) {
			perror("cloxec output to filter");
			exit(EXIT_CLOSE);
		}
		if (fcntl(pipe_filtered[0], F_SETFD, FD_CLOEXEC) != 0) {
			perror("cloxec input from filter");
			exit(EXIT_CLOSE);
		}

		if (pthread_mutex_unlock(&pipe_lock) != 0) {
			perror("pthread_mutex_unlock (pipe_lock)");
			exit(EXIT_PTHREAD);
		}

		*write_to = pipe_orig[1];
		*read_from = pipe_filtered[0];
	}
}

std::vector<mvt_layer> filter_layers(const char *filter, std::vector<mvt_layer> &layers, unsigned z, unsigned x, unsigned y, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, int extent) {
	int write_to, read_from;
	pid_t pid;
	setup_filter(filter, &write_to, &read_from, &pid, z, x, y);

	writer_arg wa;
	wa.write_to = write_to;
	wa.layers = &layers;
	wa.z = z;
	wa.x = x;
	wa.y = y;
	wa.extent = extent;

	pthread_t writer;
	// this does need to be a real thread, so we can pipe both to and from it
	if (pthread_create(&writer, NULL, run_writer, &wa) != 0) {
		perror("pthread_create (filter writer)");
		exit(EXIT_PTHREAD);
	}

	std::vector<mvt_layer> nlayers = parse_layers(read_from, z, x, y, layermaps, tiling_seg, layer_unmaps, extent);

	while (1) {
		int stat_loc;
		if (waitpid(pid, &stat_loc, 0) < 0) {
			perror("waitpid for filter\n");
			exit(EXIT_PTHREAD);
		}
		if (WIFEXITED(stat_loc) || WIFSIGNALED(stat_loc)) {
			break;
		}
	}

	void *ret;
	if (pthread_join(writer, &ret) != 0) {
		perror("pthread_join filter writer");
		exit(EXIT_PTHREAD);
	}

	return nlayers;
}
