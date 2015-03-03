#include "minisphere.h"
#include "api.h"
#include "person.h"
#include "tileset.h"

#include "map_engine.h"

struct map
{
	bool               is_toric;
	point3_t           origin;
	tileset_t*         tileset;
	int                num_layers;
	int                num_persons;
	int                num_triggers;
	int                num_zones;
	struct map_layer   *layers;
	struct map_person  *persons;
	struct map_trigger *triggers;
	lstring_t*         *scripts;
	struct map_zone    *zones;
};

struct map_layer
{
	int  width, height;
	int* tilemap;
};

struct map_person
{
	lstring_t* name;
	lstring_t* spriteset;
	int        x, y, z;
	lstring_t* create_script;
	lstring_t* destroy_script;
	lstring_t* command_script;
	lstring_t* talk_script;
	lstring_t* touch_script;
};

struct map_trigger
{
	lstring_t* script;
};

struct map_zone
{
	rect_t area;
	int    steps;
};

static bool change_map        (const char* filename, bool preserve_persons);
static void render_map_engine (void);
static void update_map_engine (void);

static duk_ret_t js_MapEngine             (duk_context* ctx);
static duk_ret_t js_AreZonesAt            (duk_context* ctx);
static duk_ret_t js_IsCameraAttached      (duk_context* ctx);
static duk_ret_t js_IsInputAttached       (duk_context* ctx);
static duk_ret_t js_IsTriggerAt           (duk_context* ctx);
static duk_ret_t js_GetCameraPerson       (duk_context* ctx);
static duk_ret_t js_GetCurrentMap         (duk_context* ctx);
static duk_ret_t js_GetInputPerson        (duk_context* ctx);
static duk_ret_t js_GetLayerHeight        (duk_context* ctx);
static duk_ret_t js_GetLayerWidth         (duk_context* ctx);
static duk_ret_t js_GetMapEngineFrameRate (duk_context* ctx);
static duk_ret_t js_GetTileHeight         (duk_context* ctx);
static duk_ret_t js_GetTileWidth          (duk_context* ctx);
static duk_ret_t js_SetMapEngineFrameRate (duk_context* ctx);
static duk_ret_t js_SetDefaultMapScript   (duk_context* ctx);
static duk_ret_t js_SetRenderScript       (duk_context* ctx);
static duk_ret_t js_SetUpdateScript       (duk_context* ctx);
static duk_ret_t js_IsMapEngineRunning    (duk_context* ctx);
static duk_ret_t js_AttachCamera          (duk_context* ctx);
static duk_ret_t js_AttachInput           (duk_context* ctx);
static duk_ret_t js_DetachInput           (duk_context* ctx);
static duk_ret_t js_ChangeMap             (duk_context* ctx);
static duk_ret_t js_DetachCamera          (duk_context* ctx);
static duk_ret_t js_DetachInput           (duk_context* ctx);
static duk_ret_t js_ExitMapEngine         (duk_context* ctx);
static duk_ret_t js_RenderMap             (duk_context* ctx);
static duk_ret_t js_SetDelayScript        (duk_context* ctx);
static duk_ret_t js_UpdateMapEngine       (duk_context* ctx);

static person_t*    s_camera_person = NULL;
static int          s_cam_x         = 0;
static int          s_cam_y         = 0;
static int          s_delay_frames  = -1;
static bool         s_exiting       = false;
static int          s_framerate     = 0;
static unsigned int s_frames        = 0;
static person_t*    s_input_person  = NULL;
static map_t*       s_map           = NULL;
static char*        s_map_filename  = NULL;

#pragma pack(push, 1)
struct rmp_header
{
	char    signature[4];
	int16_t version;
	uint8_t type;
	int8_t  num_layers;
	uint8_t reserved_1;
	int16_t num_entities;
	int16_t start_x;
	int16_t start_y;
	int8_t  start_layer;
	int8_t  start_direction;
	int16_t num_strings;
	int16_t num_zones;
	uint8_t toric_map;
	uint8_t reserved[234];
};

struct rmp_entity_header
{
	uint16_t x;
	uint16_t y;
	uint16_t z;
	uint16_t type;
	uint8_t  reserved[8];
};

struct rmp_layer_header
{
	int16_t  width;
	int16_t  height;
	uint16_t flags;
	float    parallax_x;
	float    parallax_y;
	float    scrolling_x;
	float    scrolling_y;
	int32_t  num_segments;
	uint8_t  reflective;
	uint8_t  reserved[3];
};

struct rmp_zone_header
{
	uint16_t x1;
	uint16_t y1;
	uint16_t x2;
	uint16_t y2;
	uint16_t z;
	uint16_t num_steps;
	uint8_t  reserved[4];
};
#pragma pack(pop)

enum mapscript
{
	MAP_SCRIPT_ON_ENTER,
	MAP_SCRIPT_ON_LEAVE,
	MAP_SCRIPT_ON_LEAVE_NORTH,
	MAP_SCRIPT_ON_LEAVE_EAST,
	MAP_SCRIPT_ON_LEAVE_SOUTH,
	MAP_SCRIPT_ON_LEAVE_WEST
};

bool g_map_running = false;

map_t*
load_map(const char* path)
{
	// strings: 0 - tileset filename
	//          1 - music filename
	//          2 - script filename (obsolete, not used)
	//          3 - entry script
	//          4 - exit script
	//          5 - exit north script
	//          6 - exit east script
	//          7 - exit south script
	//          8 - exit west script
	
	uint16_t                 count;
	struct rmp_entity_header entity;
	bool                     failed = false;
	ALLEGRO_FILE*            file;
	struct rmp_layer_header  layer_info;
	map_t*                   map;
	int                      num_tiles;
	struct map_person*       person;
	struct rmp_header        rmp;
	int16_t*                 tile_data = NULL;
	char*                    tile_path;
	tileset_t*               tileset;
	struct map_trigger*      trigger;
	struct rmp_zone_header   zone;
	lstring_t*               *strings;

	int i, j;
	
	if ((map = calloc(1, sizeof(map_t))) == NULL) goto on_error;
	if ((file = al_fopen(path, "rb")) == NULL) goto on_error;
	if (al_fread(file, &rmp, sizeof(struct rmp_header)) != sizeof(struct rmp_header))
		goto on_error;
	if (memcmp(rmp.signature, ".rmp", 4) != 0) goto on_error;
	switch (rmp.version) {
	case 1:
		if ((strings = calloc(rmp.num_strings, sizeof(lstring_t*))) == NULL)
			goto on_error;
		for (i = 0; i < rmp.num_strings; ++i)
			failed = ((strings[i] = al_fread_lstring(file)) == NULL) || failed;
		if (failed) goto on_error;
		if ((map->layers = calloc(rmp.num_layers, sizeof(struct map_layer))) == NULL) goto on_error;
		for (i = 0; i < rmp.num_layers; ++i) {
			if (al_fread(file, &layer_info, sizeof(struct rmp_layer_header)) != sizeof(struct rmp_layer_header))
				goto on_error;
			map->layers[i].width = layer_info.width;
			map->layers[i].height = layer_info.height;
			if ((map->layers[i].tilemap = malloc(layer_info.width * layer_info.height * sizeof(int))) == NULL)
				goto on_error;
			free_lstring(al_fread_lstring(file));  // <-- layer name, not used by minisphere
			num_tiles = layer_info.width * layer_info.height;
			if ((tile_data = malloc(num_tiles * 2)) == NULL) goto on_error;
			if (al_fread(file, tile_data, num_tiles * 2) != num_tiles * 2) goto on_error;
			for (j = 0; j < num_tiles; ++j) map->layers[i].tilemap[j] = tile_data[j];
			free(tile_data); tile_data = NULL;
		}
		map->persons = NULL; map->num_persons = 0;
		map->triggers = NULL; map->num_triggers = 0;
		for (i = 0; i < rmp.num_entities; ++i) {
			al_fread(file, &entity, sizeof(struct rmp_entity_header));
			switch (entity.type) {
			case 1:  // person
				++map->num_persons;
				map->persons = realloc(map->persons, map->num_persons * sizeof(struct map_person));
				person = &map->persons[map->num_persons - 1];
				memset(person, 0, sizeof(struct map_person));
				if ((person->name = al_fread_lstring(file)) == NULL) goto on_error;
				if ((person->spriteset = al_fread_lstring(file)) == NULL) goto on_error;
				person->x = entity.x; person->y = entity.y; person->z = entity.z;
				if (al_fread(file, &count, 2) != 2 || count < 5) goto on_error;
				person->create_script = al_fread_lstring(file);
				person->destroy_script = al_fread_lstring(file);
				person->touch_script = al_fread_lstring(file);
				person->talk_script = al_fread_lstring(file);
				person->command_script = al_fread_lstring(file);
				for (j = 5; j < count; ++j) {
					free_lstring(al_fread_lstring(file));
				}
				al_fseek(file, 16, ALLEGRO_SEEK_CUR);
				break;
			case 2:  // trigger
				++map->num_triggers;
				map->triggers = realloc(map->triggers, map->num_triggers * sizeof(struct map_trigger));
				trigger = &map->triggers[map->num_triggers - 1];
				memset(trigger, 0, sizeof(struct map_trigger));
				trigger->script = al_fread_lstring(file);
				break;
			default:
				goto on_error;
			}
		}
		for (i = 0; i < rmp.num_zones; ++i) {
			al_fread(file, &zone, sizeof(struct rmp_zone_header));
			free_lstring(al_fread_lstring(file));
		}
		tile_path = get_asset_path(strings[0]->cstr, "maps", false);
		tileset = strcmp(strings[0]->cstr, "") == 0 ? load_tileset_f(file) : load_tileset(tile_path);
		free(tile_path);
		if (tileset == NULL) goto on_error;
		map->is_toric = rmp.toric_map;
		map->origin.x = rmp.start_x;
		map->origin.y = rmp.start_y;
		map->origin.z = rmp.start_layer;
		map->tileset = tileset;
		map->num_layers = rmp.num_layers;
		map->scripts = strings;
		break;
	default:
		goto on_error;
	}
	al_fclose(file);
	return map;

on_error:
	if (file != NULL) al_fclose(file);
	free(tile_data);
	if (strings != NULL) {
		for (i = 0; i < rmp.num_strings; ++i) free_lstring(strings[i]);
		free(strings);
	}
	if (map != NULL) {
		if (map->layers != NULL) {
			for (i = 0; i < rmp.num_layers; ++i) free(map->layers[i].tilemap);
			free(map->layers);
		}
		if (map->persons != NULL) {
			for (i = 0; i < map->num_persons; ++i) {
				free_lstring(map->persons[i].name);
				free_lstring(map->persons[i].spriteset);
			}
			free(map->persons);
		}
		free(map->triggers);
		free(map);
	}
	return NULL;
}

void
free_map(map_t* map)
{
	int i;
	
	if (map != NULL) {
		for (i = 0; i < 9; ++i) free_lstring(map->scripts[i]);
		for (i = 0; i < map->num_persons; ++i) {
			free_lstring(map->persons[i].name);
			free_lstring(map->persons[i].spriteset);
		}
		for (i = 0; i < map->num_triggers; ++i) {
			free_lstring(map->triggers[i].script);
		}
		free(map->triggers);
		free(map->persons);
		free(map->scripts);
		free(map->layers);
		free_tileset(map->tileset);
		free(map);
	}
}

void
init_map_engine_api(duk_context* ctx)
{
	register_api_func(ctx, NULL, "MapEngine", js_MapEngine);
	register_api_func(ctx, NULL, "AreZonesAt", js_AreZonesAt);
	register_api_func(ctx, NULL, "IsCameraAttached", js_IsCameraAttached);
	register_api_func(ctx, NULL, "IsInputAttached", js_IsInputAttached);
	register_api_func(ctx, NULL, "IsTriggerAt", js_IsTriggerAt);
	register_api_func(ctx, NULL, "GetCameraPerson", js_GetCameraPerson);
	register_api_func(ctx, NULL, "GetCurrentMap", js_GetCurrentMap);
	register_api_func(ctx, NULL, "GetInputPerson", js_GetInputPerson);
	register_api_func(ctx, NULL, "GetLayerHeight", js_GetLayerHeight);
	register_api_func(ctx, NULL, "GetLayerWidth", js_GetLayerWidth);
	register_api_func(ctx, NULL, "GetMapEngineFrameRate", js_GetMapEngineFrameRate);
	register_api_func(ctx, NULL, "GetTileHeight", js_GetTileHeight);
	register_api_func(ctx, NULL, "GetTileWidth", js_GetTileWidth);
	register_api_func(ctx, NULL, "SetMapEngineFrameRate", js_SetMapEngineFrameRate);
	register_api_func(ctx, NULL, "SetDefaultMapScript", js_SetDefaultMapScript);
	register_api_func(ctx, NULL, "SetRenderScript", js_SetRenderScript);
	register_api_func(ctx, NULL, "SetUpdateScript", js_SetUpdateScript);
	register_api_func(ctx, NULL, "IsMapEngineRunning", js_IsMapEngineRunning);
	register_api_func(ctx, NULL, "AttachCamera", js_AttachCamera);
	register_api_func(ctx, NULL, "AttachInput", js_AttachInput);
	register_api_func(ctx, NULL, "ChangeMap", js_ChangeMap);
	register_api_func(ctx, NULL, "DetachCamera", js_DetachCamera);
	register_api_func(ctx, NULL, "DetachInput", js_DetachInput);
	register_api_func(ctx, NULL, "ExitMapEngine", js_ExitMapEngine);
	register_api_func(ctx, NULL, "RenderMap", js_RenderMap);
	register_api_func(ctx, NULL, "SetDelayScript", js_SetDelayScript);
	register_api_func(ctx, NULL, "UpdateMapEngine", js_UpdateMapEngine);

	// Map script types
	register_api_const(ctx, "SCRIPT_ON_ENTER_MAP", MAP_SCRIPT_ON_ENTER);
	register_api_const(ctx, "SCRIPT_ON_LEAVE_MAP", MAP_SCRIPT_ON_LEAVE);
	register_api_const(ctx, "SCRIPT_ON_LEAVE_MAP_NORTH", MAP_SCRIPT_ON_LEAVE_NORTH);
	register_api_const(ctx, "SCRIPT_ON_LEAVE_MAP_EAST", MAP_SCRIPT_ON_LEAVE_EAST);
	register_api_const(ctx, "SCRIPT_ON_LEAVE_MAP_SOUTH", MAP_SCRIPT_ON_LEAVE_SOUTH);
	register_api_const(ctx, "SCRIPT_ON_LEAVE_MAP_WEST", MAP_SCRIPT_ON_LEAVE_WEST);

	// initialize subcomponent APIs (persons, etc.)
	init_person_api();
}

point3_t
get_map_origin(void)
{
	return s_map->origin;
}

static bool
change_map(const char* filename, bool preserve_persons)
{
	map_t*             map;
	char*              path;
	person_t*          person;
	struct map_person* person_info;

	int i;

	path = get_asset_path(filename, "maps", false);
	map = load_map(path);
	free(path);
	if (map != NULL) {
		free_map(s_map); free(s_map_filename);
		s_map = map; s_map_filename = strdup(filename);
		reset_persons(s_map, preserve_persons);

		// populate persons
		for (i = 0; i < s_map->num_persons; ++i) {
			person_info = &s_map->persons[i];
			person = create_person(person_info->name->cstr, person_info->spriteset->cstr, false);
			set_person_xyz(person, person_info->x, person_info->y, person_info->z);
			set_person_script(person, PERSON_SCRIPT_ON_CREATE, person_info->create_script);
			set_person_script(person, PERSON_SCRIPT_ON_DESTROY, person_info->destroy_script);
			set_person_script(person, PERSON_SCRIPT_ON_ACT_TOUCH, person_info->touch_script);
			set_person_script(person, PERSON_SCRIPT_ON_ACT_TALK, person_info->talk_script);
			set_person_script(person, PERSON_SCRIPT_GENERATOR, person_info->command_script);
			call_person_script(person, PERSON_SCRIPT_ON_CREATE);
		}
		
		// run default map entry script
		duk_push_global_stash(g_duktape);
		duk_get_prop_string(g_duktape, -1, "map_def_enter_script");
		if (duk_is_callable(g_duktape, -1)) {
			duk_call(g_duktape, 0); duk_pop(g_duktape);
		}
		duk_pop(g_duktape);
		
		// run map entry script
		duk_compile_lstring(g_duktape, 0x0, s_map->scripts[3]->cstr, s_map->scripts[3]->length);
		duk_call(g_duktape, 0); duk_pop(g_duktape);
		
		s_frames = 0;
		return true;
	}
	else {
		return false;
	}
}

static void
render_map_engine(void)
{
	int               cell_x, cell_y;
	int               first_cell_x, first_cell_y;
	struct map_layer* layer;
	int               map_w, map_h;
	int               tile_w, tile_h;
	int               off_x, off_y;
	int               tile_index;
	
	int x, y, z;
	
	get_tile_size(s_map->tileset, &tile_w, &tile_h);
	map_w = s_map->layers[0].width * tile_w;
	map_h = s_map->layers[0].height * tile_h;
	off_x = s_cam_x - g_res_x / 2;
	off_y = s_cam_y - g_res_y / 2;
	if (!s_map->is_toric) {
		off_x = fmin(fmax(off_x, 0), map_w - g_res_x);
		off_y = fmin(fmax(off_y, 0), map_h - g_res_y);
	}
	al_hold_bitmap_drawing(true);
	for (z = 0; z < s_map->num_layers; ++z) {
		layer = &s_map->layers[z];
		first_cell_x = floorf(off_x / (float)tile_w);
		first_cell_y = floorf(off_y / (float)tile_h);
		for (y = 0; y < g_res_y / tile_h + 2; ++y) for (x = 0; x < g_res_x / tile_w + 2; ++x) {
			cell_x = ((x + first_cell_x) % layer->width + layer->width) % layer->width;
			cell_y = ((y + first_cell_y) % layer->height + layer->height) % layer->height;
			tile_index = layer->tilemap[cell_x + cell_y * layer->width];
			draw_tile(s_map->tileset, x * tile_w - (off_x % tile_w + tile_w) % tile_w, y * tile_h - (off_y % tile_h + tile_h) % tile_h, tile_index);
		}
	}
	al_hold_bitmap_drawing(false);
	render_persons(off_x, off_y);
	duk_push_global_stash(g_duktape);
	duk_get_prop_string(g_duktape, -1, "render_script");
	if (duk_is_callable(g_duktape, -1)) duk_call(g_duktape, 0);
	duk_pop_2(g_duktape);
}

static void
update_map_engine(void)
{
	ALLEGRO_KEYBOARD_STATE kb_state;
	int                    map_w, map_h;
	int                    tile_w, tile_h;
	float                  x, y;
	
	++s_frames;
	update_persons();
	
	// check for player input
	if (s_input_person != NULL) {
		al_get_keyboard_state(&kb_state);
		if (al_key_down(&kb_state, ALLEGRO_KEY_UP)) {
			command_person(s_input_person, COMMAND_FACE_NORTH);
			command_person(s_input_person, COMMAND_MOVE_NORTH);
		}
		else if (al_key_down(&kb_state, ALLEGRO_KEY_RIGHT)) {
			command_person(s_input_person, COMMAND_FACE_EAST);
			command_person(s_input_person, COMMAND_MOVE_EAST);
		}
		else if (al_key_down(&kb_state, ALLEGRO_KEY_DOWN)) {
			command_person(s_input_person, COMMAND_FACE_SOUTH);
			command_person(s_input_person, COMMAND_MOVE_SOUTH);
		}
		else if (al_key_down(&kb_state, ALLEGRO_KEY_LEFT)) {
			command_person(s_input_person, COMMAND_FACE_WEST);
			command_person(s_input_person, COMMAND_MOVE_WEST);
		}
	}
	
	// update camera
	if (s_camera_person != NULL) {
		get_tile_size(s_map->tileset, &tile_w, &tile_h);
		map_w = s_map->layers[0].width * tile_w;
		map_h = s_map->layers[0].height * tile_h;
		get_person_xy(s_camera_person, &x, &y, map_w, map_h, false);
		s_cam_x = x;
		s_cam_y = y;
	}

	// run update script
	duk_push_global_stash(g_duktape);
	duk_get_prop_string(g_duktape, -1, "update_script");
	if (duk_is_callable(g_duktape, -1)) duk_call(g_duktape, 0);
	duk_pop_2(g_duktape);

	// run delay script, if applicable
	if (s_delay_frames >= 0 && --s_delay_frames == -1) {
		duk_push_global_stash(g_duktape);
		duk_get_prop_string(g_duktape, -1, "map_delay_script");
		duk_call(g_duktape, 0);
		duk_pop_2(g_duktape);
	}
}

static duk_ret_t
js_MapEngine(duk_context* ctx)
{
	const char* filename = duk_require_string(ctx, 0);
	int framerate = duk_require_int(ctx, 1);
	
	g_map_running = true;
	s_exiting = false;
	al_clear_to_color(al_map_rgba(0, 0, 0, 255));
	s_framerate = framerate;
	if (!change_map(filename, true)) duk_error(ctx, DUK_ERR_ERROR, "MapEngine(): Failed to load map file '%s' into map engine", filename);
	while (!s_exiting) {
		if (!begin_frame(s_framerate)) duk_error(ctx, DUK_ERR_ERROR, "!exit");
		update_map_engine();
		if (!g_skip_frame) render_map_engine();
	}
	g_map_running = false;
	return 0;
}

static duk_ret_t
js_AreZonesAt(duk_context* ctx)
{
	int x = duk_require_int(ctx, 0);
	int y = duk_require_int(ctx, 1);
	int z = duk_require_int(ctx, 2);

	if (z < 0 || z >= s_map->num_layers)
		duk_error(ctx, DUK_ERR_RANGE_ERROR, "AreZonesAt(): Invalid layer index; valid range is 0-%i, caller passed %i", s_map->num_layers - 1, z);
	// TODO: test for zones at specified location
	duk_push_false(ctx);
	return 1;
}

static duk_ret_t
js_IsCameraAttached(duk_context* ctx)
{
	duk_push_boolean(ctx, s_camera_person != NULL);
	return 1;
}

static duk_ret_t
js_IsInputAttached(duk_context* ctx)
{
	duk_push_boolean(ctx, s_input_person != NULL);
	return 1;
}

static duk_ret_t
js_IsTriggerAt(duk_context* ctx)
{
	int x = duk_require_int(ctx, 0);
	int y = duk_require_int(ctx, 1);
	int z = duk_require_int(ctx, 2);
	
	if (z < 0 || z >= s_map->num_layers)
		duk_error(ctx, DUK_ERR_RANGE_ERROR, "IsTriggerAt(): Invalid layer index; valid range is 0-%i, caller passed %i", s_map->num_layers - 1, z);
	duk_push_false(ctx);
	// TODO: test for triggers at specified location
	return 1;
}

static duk_ret_t
js_GetCameraPerson(duk_context* ctx)
{
	if (s_camera_person == NULL)
		duk_error(ctx, DUK_ERR_ERROR, "GetCameraPerson(): Invalid operation, camera not attached");
	duk_push_string(ctx, get_person_name(s_camera_person));
	return 1;
}

static duk_ret_t
js_GetCurrentMap(duk_context* ctx)
{
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "GetCurrentMap(): Operation requires the map engine to be running");
	duk_push_string(ctx, s_map_filename);
	return 1;
}

static duk_ret_t
js_GetInputPerson(duk_context* ctx)
{
	if (s_input_person == NULL)
		duk_error(ctx, DUK_ERR_ERROR, "GetInputPerson(): Invalid operation, input not attached");
	duk_push_string(ctx, get_person_name(s_input_person));
	return 1;
}

static duk_ret_t
js_GetLayerHeight(duk_context* ctx)
{
	int z = duk_require_int(ctx, 0);

	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "GetLayerHeight(): Map engine must be running");
	if (z < 0 || z > s_map->num_layers)
		duk_error(ctx, DUK_ERR_ERROR, "GetLayerHeight(): Invalid layer index; valid range is 0-%i, called passed %i", s_map->num_layers, z);
	duk_push_int(ctx, s_map->layers[z].height);
	return 1;
}

static duk_ret_t
js_GetLayerWidth(duk_context* ctx)
{
	int z = duk_require_int(ctx, 0);
	
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "GetLayerWidth(): Map engine must be running");
	if (z < 0 || z > s_map->num_layers)
		duk_error(ctx, DUK_ERR_ERROR, "GetLayerWidth(): Invalid layer index; valid range is 0-%i, called passed %i", s_map->num_layers, z);
	duk_push_int(ctx, s_map->layers[z].width);
	return 1;
}

static duk_ret_t
js_GetMapEngineFrameRate(duk_context* ctx)
{
	duk_push_int(ctx, s_framerate);
	return 1;
}

static duk_ret_t
js_GetTileHeight(duk_context* ctx)
{
	int w, h;

	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "GetTileHeight(): Map engine must be running");
	get_tile_size(s_map->tileset, &w, &h);
	duk_push_int(ctx, h);
	return 1;
}

static duk_ret_t
js_GetTileWidth(duk_context* ctx)
{
	int w, h;
	
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "GetTileWidth(): Map engine must be running");
	get_tile_size(s_map->tileset, &w, &h);
	duk_push_int(ctx, w);
	return 1;
}

static duk_ret_t
js_SetMapEngineFrameRate(duk_context* ctx)
{
	s_framerate = duk_to_int(ctx, 0);
	return 0;
}

static duk_ret_t
js_SetDefaultMapScript(duk_context* ctx)
{
	const char* script;
	size_t      script_size;
	const char* script_name;
	int         script_type;

	script_type = duk_require_int(ctx, 0);
	script = duk_require_lstring(ctx, 1, &script_size);
	script_name = (script_type == MAP_SCRIPT_ON_ENTER) ? "map_def_enter_script"
		: (script_type == MAP_SCRIPT_ON_LEAVE) ? "map_def_leave_script"
		: (script_type == MAP_SCRIPT_ON_LEAVE_NORTH) ? "map_def_leave_north_script"
		: (script_type == MAP_SCRIPT_ON_LEAVE_EAST) ? "map_def_leave_east_script"
		: (script_type == MAP_SCRIPT_ON_LEAVE_SOUTH) ? "map_def_leave_south_script"
		: (script_type == MAP_SCRIPT_ON_LEAVE_WEST) ? "map_def_leave_west_script"
		: NULL;
	if (script_name == NULL)
		duk_error(ctx, DUK_ERR_ERROR, "SetDefaultMapScript(): Invalid map script constant");
	duk_push_global_stash(ctx);
	duk_push_string(ctx, "[def-mapscript]");
	duk_compile_lstring_filename(ctx, DUK_COMPILE_EVAL, script, script_size);
	duk_put_prop_string(ctx, -2, script_name);
	duk_pop(ctx);
	return 0;
}

static duk_ret_t
js_SetDelayScript(duk_context* ctx)
{
	int frames = duk_require_int(ctx, 0);
	size_t script_size;
	const char* script = duk_require_lstring(ctx, 1, &script_size);

	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "SetDelayScript(): Map engine is not running");
	if (frames < 0)
		duk_error(ctx, DUK_ERR_RANGE_ERROR, "SetDelayScript(): Number of delay frames cannot be negative");
	duk_push_global_stash(ctx);
	duk_push_string(ctx, "[delayscript]");
	duk_compile_lstring_filename(ctx, DUK_COMPILE_EVAL, script, script_size);
	duk_put_prop_string(ctx, -2, "map_delay_script");
	duk_pop(ctx);
	s_delay_frames = frames;
	return 0;
}

static duk_ret_t
js_SetRenderScript(duk_context* ctx)
{
	const char* script;
	size_t      script_size;

	script = duk_require_lstring(ctx, 0, &script_size);
	duk_push_global_stash(ctx);
	duk_push_string(ctx, "[renderscript]");
	duk_compile_lstring_filename(ctx, DUK_COMPILE_EVAL, script, script_size);
	duk_put_prop_string(ctx, -2, "render_script");
	duk_pop(ctx);
	return 0;
}

static duk_ret_t
js_SetUpdateScript(duk_context* ctx)
{
	const char* script;
	size_t      script_size;

	script = duk_require_lstring(ctx, 0, &script_size);
	duk_push_global_stash(ctx);
	duk_push_string(ctx, "[updatescript]");
	duk_compile_lstring_filename(ctx, DUK_COMPILE_EVAL, script, script_size);
	duk_put_prop_string(ctx, -2, "update_script");
	duk_pop(ctx);
	return 0;
}

static duk_ret_t
js_IsMapEngineRunning(duk_context* ctx)
{
	duk_push_boolean(ctx, g_map_running);
	return 1;
}

static duk_ret_t
js_AttachCamera(duk_context* ctx)
{
	const char* name;
	person_t*   person;

	name = duk_to_string(ctx, 0);
	if ((person = find_person(name)) == NULL)
		duk_error(ctx, DUK_ERR_REFERENCE_ERROR, "AttachCamera(): Person '%s' doesn't exist", name);
	s_camera_person = person;
	return 0;
}

static duk_ret_t
js_AttachInput(duk_context* ctx)
{
	const char* name;
	person_t*   person;

	name = duk_to_string(ctx, 0);
	if ((person = find_person(name)) == NULL)
		duk_error(ctx, DUK_ERR_REFERENCE_ERROR, "AttachInput(): Person '%s' doesn't exist", name);
	s_input_person = person;
	return 0;
}

static duk_ret_t
js_ChangeMap(duk_context* ctx)
{
	const char* filename = duk_require_string(ctx, 0);
	
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "ChangeMap(): Map engine is not running");
	if (!change_map(filename, false))
		duk_error(ctx, DUK_ERR_ERROR, "ChangeMap(): Failed to load map file '%s' into map engine", filename);
	return 0;
}

static duk_ret_t
js_DetachCamera(duk_context* ctx)
{
	s_camera_person = NULL;
	return 0;
}

static duk_ret_t
js_DetachInput(duk_context* ctx)
{
	s_input_person = NULL;
	return 0;
}

static duk_ret_t
js_ExitMapEngine(duk_context* ctx)
{
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "ExitMapEngine(): Map engine is not running");
	s_exiting = true;
	return 0;
}

static duk_ret_t
js_RenderMap(duk_context* ctx)
{
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "RenderMap(): Operation requires the map engine to be running");
	render_map_engine();
	return 0;
}

static duk_ret_t
js_UpdateMapEngine(duk_context* ctx)
{
	if (!g_map_running)
		duk_error(ctx, DUK_ERR_ERROR, "UpdateMapEngine(): Operation requires the map engine to be running");
	update_map_engine();
	return 0;
}
