#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#include <obs-module.h>
#include <util/platform.h>
#include <../UI/obs-frontend-api/obs-frontend-api.h>
#else
#include <obs-module.h>
#include <../UI/obs-frontend-api/obs-frontend-api.h>
#include <pthread.h>
#include <util/platform.h>
#include <util/threading.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#define HAS_SHMEM // only for Windows for now
#define HAS_PUT // only for Windows for now
#else
#define Sleep(ms) os_sleep_ms(ms)
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("screenshot-filter-jpeg", "en-US")

#define do_log(level, format, ...) \
	blog(level, "[screenshot-filter-jpeg] " format, ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

static void capture_key_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *key, bool pressed);

struct screenshot_filter_data;
static bool write_image(struct screenshot_filter_data *filter, uint8_t *image_data_ptr,
			int image_data_linesize, uint32_t width, uint32_t height);
static bool write_data(const char *destination, uint8_t *data, size_t len,
		       const char *content_type, uint32_t width, uint32_t height,
		       int destination_type);

#ifdef HAS_PUT
static bool put_data(char *url, uint8_t *buf, size_t len, char *content_type,
		     int width, int height);
#endif

#define SETTING_DESTINATION_TYPE "destination_type"

#define SETTING_DESTINATION_FOLDER "destination_folder"
#define SETTING_DESTINATION_PATH "destinaton_path"
#define SETTING_DESTINATION_URL "destination_url"
#define SETTING_DESTINATION_SHMEM "destination_shmem"

#define SETTING_DESTINATION_PATH_ID 0
#ifdef HAS_PUT
#define SETTING_DESTINATION_URL_ID 1
#endif
#ifdef HAS_SHMEM
#define SETTING_DESTINATION_SHMEM_ID 2
#endif
#define SETTING_DESTINATION_FOLDER_ID 3

#define SETTING_TIMER "timer"
#define SETTING_INTERVAL "interval"
#define SETTING_RAW "raw"

struct screenshot_filter_data {
	obs_source_t *context;

#ifdef _WIN32
	HANDLE image_writer_thread;
#else
	pthread_t image_writer_thread;
#endif

	int destination_type;
	char *destination;
	bool timer;
	float interval;
	bool raw;
	int timer_shown;
	uint64_t at_shown_next_ns;
	int timer_previewed;
	uint64_t at_previewed_next_ns;
	int timer_activated;
	uint64_t at_activated_next_ns;
	bool is_preview, was_preview;
	obs_hotkey_id capture_hotkey_id;

	char *text_src_name;
	int text_src_last_sec;

	float since_last;
	bool capture;

	uint32_t width;
	uint32_t height;
	gs_texrender_t *texrender;
	gs_stagesurf_t *staging_texture;
	uint32_t resize_w, resize_h;

	uint8_t *data;
	uint32_t linesize;
	bool ready;

#ifdef HAS_SHMEM
	uint32_t index;
	char shmem_name[256];
	uint32_t shmem_size;
	HANDLE shmem;
#endif

#ifdef _WIN32
	HANDLE mutex;
#else
	pthread_mutex_t mutex;
#define WaitForSingleObject(mutex, type) pthread_mutex_lock(&mutex)
#define ReleaseMutex(mutex) pthread_mutex_unlock(&mutex)
#endif
	bool exit;
	bool exited;
};

#ifdef _WIN32
static DWORD CALLBACK write_images_thread(struct screenshot_filter_data *filter_)
#else
static void *write_images_thread(void *filter_)
#endif
{
	struct screenshot_filter_data *filter = filter_;
	while (!filter->exit) {
		WaitForSingleObject(filter->mutex, INFINITE);
		// copy all props & data inside the mutex, then do the processing/write/put outside of the mutex
		uint8_t *data = NULL;
		char *destination = filter->destination;
		int destination_type = filter->destination_type;
		uint32_t width = filter->width;
		uint32_t height = filter->height;
		uint32_t linesize = filter->linesize;
		bool raw = filter->raw;
		if (filter->ready) {
			data = bzalloc(linesize * height);
			memcpy(data, filter->data, linesize * height);
			filter->ready = false;
		}
		ReleaseMutex(filter->mutex);

		if (data && width > 10 && height > 10) {
			blog(LOG_INFO, "write_images_thread: got data width=%d height=%d", width, height);
#ifdef HAS_SHMEM
			if (destination_type == SETTING_DESTINATION_SHMEM_ID) {
				if (filter->shmem) {
					uint32_t *buf =
						(uint32_t *)MapViewOfFile(
							filter->shmem,
							FILE_MAP_ALL_ACCESS, 0,
							0, filter->shmem_size);

					if (buf) {
						buf[0] = width;
						buf[1] = height;
						buf[2] = linesize;
						buf[3] = filter->index ++;
						memcpy(&buf[4], data,
						       linesize * height);
					}

					UnmapViewOfFile(buf);
				}
			} else
#endif
			if (raw)
				write_data(destination, data, linesize * height,
					   "image/rgba32", width, height,
					   destination_type);
			else
				write_image(filter, data, linesize, width, height);
			bfree(data);
		}
		Sleep(200);
	}
	filter->exited = true;

	return 0;
}

static const char *screenshot_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Screenshot Filter JPEG";
}

static bool is_dest_modified(obs_properties_t *props, obs_property_t *unused,
			     obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_FOLDER),
				 type == SETTING_DESTINATION_FOLDER_ID);
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_PATH),
				 type == SETTING_DESTINATION_PATH_ID);
#ifdef HAS_PUT
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_URL),
				 type == SETTING_DESTINATION_URL_ID);
#endif
#ifdef HAS_SHMEM
	obs_property_set_visible(obs_properties_get(props,
						    SETTING_DESTINATION_SHMEM),
				 type == SETTING_DESTINATION_SHMEM_ID);

	obs_property_set_visible(obs_properties_get(props, SETTING_RAW),
				 type != SETTING_DESTINATION_SHMEM_ID);

	obs_property_set_visible(obs_properties_get(props, SETTING_TIMER),
				 type != SETTING_DESTINATION_SHMEM_ID);
#endif

	bool is_timer_enable = obs_data_get_bool(settings, SETTING_TIMER);
	obs_property_set_visible(obs_properties_get(props, SETTING_INTERVAL),
				 is_timer_enable
#ifdef HAS_SHMEM
				 || type == SETTING_DESTINATION_SHMEM_ID
#endif
			);

	return true;
}

static bool is_timer_enable_modified(obs_properties_t *props,
				     obs_property_t *unused,
				     obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);

#ifdef HAS_SHMEM
	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
#endif
	bool is_timer_enable = obs_data_get_bool(settings, SETTING_TIMER);
	obs_property_set_visible(obs_properties_get(props, SETTING_INTERVAL),
				 is_timer_enable
#ifdef HAS_SHMEM
				 || type == SETTING_DESTINATION_SHMEM_ID
#endif
			);

	return true;
}

static bool at_shown_modified(obs_properties_t *props, obs_property_t *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);
	bool en = settings ? obs_data_get_bool(settings, "at_shown") : false;
	obs_property_set_visible(obs_properties_get(props, "timer_shown"), en);
	return true;
}

static bool at_previewed_modified(obs_properties_t *props, obs_property_t *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);
	bool en = settings ? obs_data_get_bool(settings, "at_previewed") : false;
	obs_property_set_visible(obs_properties_get(props, "timer_previewed"), en);
	return true;
}

static bool at_activated_modified(obs_properties_t *props, obs_property_t *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);
	bool en = settings ? obs_data_get_bool(settings, "at_activated") : false;
	obs_property_set_visible(obs_properties_get(props, "timer_activated"), en);
	return true;
}

static bool resize_modified(obs_properties_t *props, obs_property_t *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);
	bool en = settings ? obs_data_get_bool(settings, "resize") : false;
	obs_property_set_visible(obs_properties_get(props, "resize_w"), en);
	obs_property_set_visible(obs_properties_get(props, "resize_h"), en);
	return true;
}

static bool is_text_source_id(const char *srcId)
{
	if (!strcmp(srcId, "text_gdiplus")) return true;
	if (!strcmp(srcId, "text_gdiplus_v2")) return true;
	if (!strcmp(srcId, "text_ft2_source")) return true;
	if (!strcmp(srcId, "text_ft2_source_v2")) return true;
	if (!strcmp(srcId, "obs_text_pthread_source")) return true;
	return false;
}

static bool add_text_source_to_property(void *data, obs_source_t *source)
{
	obs_property_t *p = data;
	const char *srcId = obs_source_get_id(source);
	if (!is_text_source_id(srcId))
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(p, name, name);
	return true;
}

static obs_properties_t *screenshot_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(
		props, SETTING_DESTINATION_TYPE, "Destination Type",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Output to folder",
				  SETTING_DESTINATION_FOLDER_ID);
	obs_property_list_add_int(p, "Output to file",
				  SETTING_DESTINATION_PATH_ID);
#ifdef HAS_PUT
	obs_property_list_add_int(p, "Output to URL",
				  SETTING_DESTINATION_URL_ID);
#endif
#ifdef HAS_SHMEM
	obs_property_list_add_int(p, "Output to Named Shared Memory",
				  SETTING_DESTINATION_SHMEM_ID);
#endif

	obs_property_set_modified_callback(p, is_dest_modified);
	obs_properties_add_path(props, SETTING_DESTINATION_FOLDER,
				"Destination (folder)", OBS_PATH_DIRECTORY,
				"*.*", NULL);
	obs_properties_add_path(props, SETTING_DESTINATION_PATH, "Destination",
				OBS_PATH_FILE_SAVE, "*.*", NULL);
#ifdef HAS_PUT
	obs_properties_add_text(props, SETTING_DESTINATION_URL,
				"Destination (url)", OBS_TEXT_DEFAULT);
#endif
#ifdef HAS_SHMEM
	obs_properties_add_text(props, SETTING_DESTINATION_SHMEM,
				"Shared Memory Name", OBS_TEXT_DEFAULT);
#endif

	obs_property_t *p_enable_timer =
		obs_properties_add_bool(props, SETTING_TIMER, "Enable timer");
	obs_property_set_modified_callback(p_enable_timer,
					   is_timer_enable_modified);

	obs_properties_add_float(props, SETTING_INTERVAL,
					"Interval (seconds)", 0.25, 86400, 0.25);

	obs_properties_add_bool(props, SETTING_RAW, "Raw image");

	p = obs_properties_add_bool(props, "at_shown", "Screenshot at shown");
	obs_property_set_modified_callback(p, at_shown_modified);
	obs_properties_add_int(props, "timer_shown", "Timer after shown [s]", 0, 10, 1);

	p = obs_properties_add_bool(props, "at_previewed", "Screenshot at displayed on Preview");
	obs_property_set_modified_callback(p, at_previewed_modified);
	obs_properties_add_int(props, "timer_previewed", "Timer after displayed [s]", 0, 10, 1);

	p = obs_properties_add_bool(props, "at_activated", "Screenshot at displayed on Program");
	obs_property_set_modified_callback(p, at_activated_modified);
	obs_properties_add_int(props, "timer_activated", "Timer after displayed [s]", 0, 10, 1);

	p = obs_properties_add_list(props, "text_src_name", "Countdown text", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "(Disabled)", "");
	obs_enum_sources(add_text_source_to_property, p);

	p = obs_properties_add_bool(props, "resize", "Resize");
	obs_property_set_modified_callback(p, resize_modified);
	obs_properties_add_int(props, "resize_w", "Width", 16, 8192, 1);
	obs_properties_add_int(props, "resize_h", "Height", 16, 8192, 1);

	return props;
}

static void screenshot_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, SETTING_DESTINATION_TYPE,
				    SETTING_DESTINATION_FOLDER_ID);
	obs_data_set_default_bool(settings, SETTING_TIMER, false);
	obs_data_set_default_double(settings, SETTING_INTERVAL, 2.0f);
	obs_data_set_default_bool(settings, SETTING_RAW, false);
	obs_data_set_default_int(settings, "resize_w", 1600);
	obs_data_set_default_int(settings, "resize_h", 1200);
}

static void screenshot_filter_update(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	int type = (int)obs_data_get_int(settings, SETTING_DESTINATION_TYPE);
	const char *path = obs_data_get_string(settings, SETTING_DESTINATION_PATH);
#ifdef HAS_PUT
	const char *url = obs_data_get_string(settings, SETTING_DESTINATION_URL);
#endif
#ifdef HAS_SHMEM
	const char *shmem_name =
		obs_data_get_string(settings, SETTING_DESTINATION_SHMEM);
#endif
	const char *folder_path =
		obs_data_get_string(settings, SETTING_DESTINATION_FOLDER);
	bool is_timer_enabled = obs_data_get_bool(settings, SETTING_TIMER);

	WaitForSingleObject(filter->mutex, INFINITE);

	filter->destination_type = type;
	bfree(filter->destination);
	if (type == SETTING_DESTINATION_PATH_ID) {
		filter->destination = bstrdup(path);
#ifdef HAS_PUT
	} else if (type == SETTING_DESTINATION_URL_ID) {
		filter->destination = bstrdup(url);
#endif
#ifdef HAS_SHMEM
	} else if (type == SETTING_DESTINATION_SHMEM_ID) {
		filter->destination = bstrdup(shmem_name);
#endif
	} else if (type == SETTING_DESTINATION_FOLDER_ID) {
		filter->destination = bstrdup(folder_path);
	}
	else
		filter->destination = bstrdup("");
	info("Set destination=%s, %d", filter->destination,
	     filter->destination_type);

	filter->timer = is_timer_enabled
#ifdef HAS_SHMEM
		|| type == SETTING_DESTINATION_SHMEM_ID
#endif
		;
	filter->interval = (float)obs_data_get_double(settings, SETTING_INTERVAL);
	filter->raw = obs_data_get_bool(settings, SETTING_RAW);

	bool at_shown = obs_data_get_bool(settings, "at_shown");
	filter->timer_shown = at_shown ? (int)obs_data_get_int(settings, "timer_shown") : -1;

	bool at_previewed = obs_data_get_bool(settings, "at_previewed");
	filter->timer_previewed = at_previewed ? (int)obs_data_get_int(settings, "timer_previewed") : -1;

	bool at_activated = obs_data_get_bool(settings, "at_activated");
	filter->timer_activated = at_activated ? (int)obs_data_get_int(settings, "timer_activated") : -1;

	bfree(filter->text_src_name);
	filter->text_src_name = bstrdup(obs_data_get_string(settings, "text_src_name"));

	bool resize = obs_data_get_bool(settings, "resize");
	filter->resize_w = resize ? (uint32_t)obs_data_get_int(settings, "resize_w") : 0;
	filter->resize_h = resize ? (uint32_t)obs_data_get_int(settings, "resize_h") : 0;

	ReleaseMutex(filter->mutex);
}

static void check_notify_preview(struct screenshot_filter_data *);
static void on_preview_scene_changed(enum obs_frontend_event event, void *param);

static void screenshot_filter_shown(void *data)
{
	struct screenshot_filter_data *filter = data;

	if (filter->timer_shown < 0)
		return;

	uint64_t cur_ns = os_gettime_ns();

	WaitForSingleObject(filter->mutex, INFINITE);
	if (!filter->at_shown_next_ns) {
		filter->at_shown_next_ns = cur_ns + filter->timer_shown * 1000000000ULL;
		blog(LOG_INFO, "at_shown_next_ns=%lld", filter->at_shown_next_ns);
	}
	ReleaseMutex(filter->mutex);

	check_notify_preview(filter);
}

static void screenshot_filter_previewed(void *data)
{
	struct screenshot_filter_data *filter = data;

	if (filter->timer_previewed < 0)
		return;

	uint64_t cur_ns = os_gettime_ns();

	WaitForSingleObject(filter->mutex, INFINITE);
	if (!filter->at_previewed_next_ns) {
		filter->at_previewed_next_ns = cur_ns + filter->timer_previewed * 1000000000ULL;
		blog(LOG_INFO, "at_previewed_next_ns=%lld", filter->at_previewed_next_ns);
	}
	ReleaseMutex(filter->mutex);
}

struct preview_callback_s
{
	obs_source_t *src;
	bool is_preview;
};

static void preview_callback(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct preview_callback_s *cb_data = param;
	if (child == cb_data->src)
		cb_data->is_preview = true;
}

static void check_notify_preview(struct screenshot_filter_data *filter)
{
	if (obs_source_enabled(filter->context)) {
		obs_source_t* preview_soure = obs_frontend_get_current_preview_scene();
		if (preview_soure) {
			struct preview_callback_s cb_data = {
				.src = obs_filter_get_parent(filter->context),
				.is_preview = false
			};
			if (preview_soure==cb_data.src)
				cb_data.is_preview = true;
			else
				obs_source_enum_active_sources(preview_soure, preview_callback, &cb_data);
			obs_source_release(preview_soure);
			filter->is_preview = cb_data.is_preview;
		}
		else
			filter->is_preview = false;
	}
	else
		filter->is_preview = false;

	if (filter->is_preview && !filter->was_preview)
		screenshot_filter_previewed(filter);
	filter->was_preview = filter->is_preview;
}

static void on_preview_scene_changed(enum obs_frontend_event event, void *param)
{
	struct screenshot_filter_data *filter = param;
	switch (event) {
		case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		case OBS_FRONTEND_EVENT_SCENE_CHANGED:
			check_notify_preview(filter);
			break;
	}
}

static void screenshot_filter_activated(void *data)
{
	struct screenshot_filter_data *filter = data;

	if (filter->timer_activated < 0)
		return;

	uint64_t cur_ns = os_gettime_ns();

	WaitForSingleObject(filter->mutex, INFINITE);
	if (!filter->at_activated_next_ns) {
		filter->at_activated_next_ns = cur_ns + filter->timer_activated * 1000000000ULL;
		blog(LOG_INFO, "at_activated_next_ns=%lld", filter->at_activated_next_ns);
	}
	ReleaseMutex(filter->mutex);
}


static void *screenshot_filter_create(obs_data_t *settings,
				      obs_source_t *context)
{
	struct screenshot_filter_data *filter =
		bzalloc(sizeof(struct screenshot_filter_data));
	info("Created filter: %p", filter);

	filter->context = context;
	filter->timer_shown = -1;
	filter->timer_previewed = -1;
	filter->timer_activated = -1;

	obs_enter_graphics();
	filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

#ifdef _WIN32
	filter->image_writer_thread = CreateThread(NULL, 0, write_images_thread,
						   (LPVOID)filter, 0, NULL);
	if (!filter->image_writer_thread) {
		//error("image_writer_thread: Failed to create thread: %d", GetLastError());
		return NULL;
	}
#else
	if (pthread_create(&filter->image_writer_thread, NULL, write_images_thread, filter)) {
		return NULL;
	}
#endif
	info("Created image writer thread %p", filter);

	filter->interval = 2.0f;
#ifdef HAS_SHMEM
	filter->shmem_name[0] = '\0';
#endif

#ifdef _WIN32
	filter->mutex = CreateMutexA(NULL, FALSE, NULL);
#else
	pthread_mutex_init(&filter->mutex, NULL);
#endif

	obs_source_update(context, settings);

	obs_frontend_add_event_callback(on_preview_scene_changed, filter);

	return filter;
}

static void screenshot_filter_save(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	obs_data_array_t *hotkeys = obs_hotkey_save(filter->capture_hotkey_id);
	obs_data_set_array(settings, "capture_hotkey", hotkeys);
	obs_data_array_release(hotkeys);
}

static void make_hotkey(struct screenshot_filter_data *filter)
{
	const char *filter_name = obs_source_get_name(filter->context);

	obs_source_t *parent = obs_filter_get_parent(filter->context);
	const char *parent_name = obs_source_get_name(parent);

	char hotkey_name[256];
	char hotkey_description[512];
	snprintf(hotkey_name, 255, "Screenshot Filter JPEG.%s.%s", parent_name,
		 filter_name);
	snprintf(hotkey_description, 512, "%s: Take JPEG screenshot of \"%s\"",
		 filter_name, parent_name);

	filter->capture_hotkey_id = obs_hotkey_register_frontend(
		hotkey_name, hotkey_description, capture_key_callback,
		filter);

	info("Registered hotkey on %s: %s %s, key=%d", filter_name,
		hotkey_name, hotkey_description,
		(int)filter->capture_hotkey_id);
	
}

static void screenshot_filter_load(void *data, obs_data_t *settings)
{
	struct screenshot_filter_data *filter = data;

	info("Registering hotkey on filter load for filter %p", filter);
	make_hotkey(filter);

	obs_data_array_t *hotkeys =
		obs_data_get_array(settings, "capture_hotkey");
	if (filter->capture_hotkey_id && obs_data_array_count(hotkeys)) {
		info("Restoring hotkey settings for %d",
		     (int)filter->capture_hotkey_id);
		obs_hotkey_load(filter->capture_hotkey_id, hotkeys);
	}
	obs_data_array_release(hotkeys);
}

static void screenshot_filter_destroy(void *data)
{
	struct screenshot_filter_data *filter = data;
	filter->exit = true;

	obs_frontend_remove_event_callback(on_preview_scene_changed, filter);

	for (int _ = 0; _ < 500 && !filter->exited; ++_) {
		Sleep(10);
	}
	if (filter->exited) {
		info("Image writer thread stopped");
	} else {
		// This is not good... Hopefully the thread has crashed, otherwise it's going to be working with filter after it has been free'd
		warn("Image writer thread failed to stop");
	}

	WaitForSingleObject(filter->mutex, INFINITE);
	obs_enter_graphics();
	gs_texrender_destroy(filter->texrender);
	if (filter->staging_texture) {
		gs_stagesurface_destroy(filter->staging_texture);
	}
	obs_leave_graphics();
	if (filter->data) {
		bfree(filter->data);
	}

#ifdef HAS_SHMEM
	if (filter->shmem) {
		CloseHandle(filter->shmem);
	}
#endif

#ifdef _WIN32
	ReleaseMutex(filter->mutex);
	CloseHandle(filter->mutex);
#else
	pthread_mutex_destroy(&filter->mutex);
#endif

	bfree(filter->destination);
	bfree(filter->text_src_name);

	bfree(filter);
}

static void screenshot_filter_remove(void *data, obs_source_t *context)
{
	UNUSED_PARAMETER(context);
	struct screenshot_filter_data *filter = data;
	if (filter->capture_hotkey_id) {
		obs_hotkey_unregister(filter->capture_hotkey_id);
		filter->capture_hotkey_id = 0;
	}
}

static void screenshot_filter_tick(void *data, float t)
{
	struct screenshot_filter_data *filter = data;

	obs_source_t *target = obs_filter_get_target(filter->context);

	if (!target) {
		filter->width = 0;
		filter->height = 0;

		if (filter->staging_texture) {
			obs_enter_graphics();
			gs_stagesurface_destroy(filter->staging_texture);
			obs_leave_graphics();
			filter->staging_texture = NULL;
		}
		if (filter->data) {
			bfree(filter->data);
			filter->data = NULL;
		}
		filter->ready = false;

		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	WaitForSingleObject(filter->mutex, INFINITE);
	bool update = false;
	if (width != filter->width || height != filter->height) {
		update = true;

		filter->width = width;
		filter->height = height;

		obs_enter_graphics();
		if (filter->staging_texture) {
			gs_stagesurface_destroy(filter->staging_texture);
		}
		filter->staging_texture = gs_stagesurface_create(
			filter->width, filter->height, GS_RGBA);
		obs_leave_graphics();
		info("Created Staging texture %d by %d: %p", width, height,
		     filter->staging_texture);

		if (filter->data) {
			bfree(filter->data);
		}
		filter->data = bzalloc((width + 32) * height * 4);
		filter->ready = false;
		filter->capture = false;
		filter->since_last = 0.0f;
	}

#ifdef HAS_SHMEM
	if (filter->destination_type == SETTING_DESTINATION_SHMEM_ID &&
	    filter->destination) {
		if (update || strncmp(filter->destination, filter->shmem_name,
				      sizeof(filter->shmem_name))) {
			info("update shmem");
			if (filter->shmem) {
				info("Closing shmem \"%s\": %x",
				     filter->shmem_name, filter->shmem);
				CloseHandle(filter->shmem);
			}
			filter->shmem_size = 12 + (width + 32) * height * 4;
			wchar_t name[256];
			mbstowcs(name, filter->destination, sizeof(name));
			filter->shmem = CreateFileMapping(INVALID_HANDLE_VALUE,
							  NULL, PAGE_READWRITE,
							  0, filter->shmem_size,
							  name);
			strncpy(filter->shmem_name, filter->destination,
				sizeof(filter->shmem_name));
			info("Created shmem \"%s\": %x", filter->shmem_name,
			     filter->shmem);
		}
	}
#else
	UNUSED_PARAMETER(update);
#endif

	if (filter->timer) {
		filter->since_last += t;
		if (filter->since_last > filter->interval - 0.05) {
			filter->capture = true;
			filter->since_last = 0.0f;
			blog(LOG_INFO, "request capture for timer");
		}
	}

	int64_t next_ns = 0x8000000000000000LL;

	if (filter->at_shown_next_ns) {
		int64_t ns = (int64_t)(os_gettime_ns() - filter->at_shown_next_ns);
		if (ns >= 0) {
			filter->capture = true;
			filter->at_shown_next_ns = 0;
			blog(LOG_INFO, "request capture at shown");
			next_ns = 0;
		}
		else if (ns > next_ns)
			next_ns = ns;
	}

	if (filter->at_previewed_next_ns) {
		int64_t ns = (int64_t)(os_gettime_ns() - filter->at_previewed_next_ns);
		if (ns >= 0) {
			filter->capture = true;
			filter->at_previewed_next_ns = 0;
			blog(LOG_INFO, "request capture at preview");
			next_ns = 0;
		}
		else if (ns > next_ns)
			next_ns = ns;
	}

	if (filter->at_activated_next_ns) {
		int64_t ns = (int64_t)(os_gettime_ns() - filter->at_activated_next_ns);
		if (ns >= 0) {
			filter->capture = true;
			filter->at_activated_next_ns = 0;
			blog(LOG_INFO, "request capture at activated");
			next_ns = 0;
		}
		else if (ns > next_ns)
			next_ns = ns;
	}

	if (filter->text_src_name && *filter->text_src_name) {
		int sec =
			next_ns>=0 ? 0 :
			next_ns==0x8000000000000000LL ? 0 :
			(int)((-next_ns + 999999999) / 1000000000LL);
		if (filter->text_src_last_sec != sec) {
			const char sz[16] = {0};
			if (sec)
				snprintf(sz, sizeof(sz)-1, "%d", sec);
			obs_source_t *src = obs_get_source_by_name(filter->text_src_name);
			obs_data_t *settings = obs_source_get_settings(src);
			obs_data_set_string(settings, "text", sz);
			obs_source_update(src, settings);
			obs_data_release(settings);
			obs_source_release(src);
			filter->text_src_last_sec = sec;
		}
	}

	ReleaseMutex(filter->mutex);
}

static void screenshot_filter_render(void *data, gs_effect_t *effect)
{
	struct screenshot_filter_data *filter = data;
	UNUSED_PARAMETER(effect);

	if (!filter->capture_hotkey_id) {
		info("Registering hotkey on filter render for filter %p", filter);
		make_hotkey(filter);
	}

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!parent || !filter->width || !filter->height || !filter->capture) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texrender_reset(filter->texrender);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->texrender, filter->width,
			       filter->height)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)filter->width, 0.0f,
			 (float)filter->height, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async) {
			obs_source_default_render(target);
		} else {
			obs_source_video_render(target);
		}

		gs_texrender_end(filter->texrender);
	}

	gs_blend_state_pop();

	gs_effect_t *effect2 = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);

	if (tex) {
		gs_stage_texture(filter->staging_texture, tex);

		uint8_t *data;
		uint32_t linesize;
		WaitForSingleObject(filter->mutex, INFINITE);
		if (gs_stagesurface_map(filter->staging_texture, &data,
					&linesize)) {
			memcpy(filter->data, data, linesize * filter->height);
			filter->linesize = linesize;
			filter->ready = true;

			gs_stagesurface_unmap(filter->staging_texture);
		}
		filter->capture = false;
		ReleaseMutex(filter->mutex);

		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect2, "image");
		gs_effect_set_texture(image, tex);

		while (gs_effect_loop(effect2, "Draw"))
			gs_draw_sprite(tex, 0, filter->width, filter->height);
	}
}

// code adapted from https://github.com/obsproject/obs-studio/pull/1269 and https://stackoverflow.com/a/12563019
static bool write_image(struct screenshot_filter_data *filter, uint8_t *image_data_ptr,
			int image_data_linesize, uint32_t width, uint32_t height)
{
	bool success = false;

	int ret;
	AVFrame *frame;
	AVPacket pkt;

	if (image_data_ptr == NULL)
		goto err_no_image_data;

	AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (codec == NULL)
		goto err_png_codec_not_found;

	AVCodecContext *codec_context = avcodec_alloc_context3(codec);
	if (codec_context == NULL)
		goto err_png_encoder_context_alloc;

	uint32_t cwidth = filter->resize_w ? filter->resize_w : width;
	uint32_t cheight = filter->resize_h ? filter->resize_h : height;

	codec_context->bit_rate = 400000;
	codec_context->width = cwidth;
	codec_context->height = cheight;
	codec_context->time_base = (AVRational){1, 25};
	codec_context->pix_fmt = AV_PIX_FMT_YUVJ444P;
	codec_context->sample_aspect_ratio.num = 1;
	codec_context->sample_aspect_ratio.den = 1;

	if (avcodec_open2(codec_context, codec, NULL) != 0)
		goto err_png_encoder_open;

	frame = av_frame_alloc();
	if (frame == NULL)
		goto err_av_frame_alloc;

	frame->format = codec_context->pix_fmt;
	frame->width = cwidth;
	frame->height = cheight;

	ret = av_image_alloc(frame->data, frame->linesize, codec_context->width,
			     codec_context->height, codec_context->pix_fmt, 4);
	if (ret < 0)
		goto err_av_image_alloc;

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	// convert RGBA to YUVJ444P since jpeg format cannot accept RGB
	{
		struct SwsContext *sws_ctx = sws_getContext(
				width, height, AV_PIX_FMT_RGBA,
				cwidth, cheight, codec_context->pix_fmt,
				SWS_BILINEAR, NULL, NULL, NULL );
		if (!sws_ctx) {
			goto err_sws;
			info("sws_getContext failed");
		}
		sws_scale(sws_ctx,
				(const uint8_t * const*)&image_data_ptr, &image_data_linesize, 0, height,
				frame->data, frame->linesize );
		sws_freeContext(sws_ctx);
	}

	frame->pts = 1;

	int got_output = 0;
	ret = avcodec_encode_video2(codec_context, &pkt, frame, &got_output);
	if (ret == 0 && got_output) {
		success = write_data(filter->destination, pkt.data, pkt.size,
				     "image/jpeg", cwidth, cheight,
				     filter->destination_type);
		av_free_packet(&pkt);
	}

err_sws:
	av_freep(frame->data);

err_av_image_alloc:
	// Failed allocating image data buffer
	av_frame_free(&frame);
	frame = NULL;

err_av_frame_alloc:
	// Failed allocating frame
	avcodec_close(codec_context);

err_png_encoder_open:
	// Failed opening PNG encoder
	avcodec_free_context(&codec_context);
	codec_context = NULL;

err_png_encoder_context_alloc:
	// failed allocating PNG encoder context
	// no need to free AVCodec* codec
err_png_codec_not_found:
	// PNG encoder not found
err_no_image_data:
	// image_data_ptr == NULL

	return success;
}

static bool write_data(const char *destination, uint8_t *data, size_t len,
		       const char *content_type, uint32_t width, uint32_t height,
		       int destination_type)
{
	bool success = false;

	if (destination_type == SETTING_DESTINATION_PATH_ID) {
		FILE *of = fopen(destination, "wb");

		if (of != NULL) {
			//info("write %s (%d bytes)", destination, len);
			fwrite(data, 1, len, of);
			fclose(of);
			success = true;
		}
	}
#ifdef HAS_PUT
	if (destination_type == SETTING_DESTINATION_URL_ID) {
		if (strstr(destination, "http://") != NULL ||
		    strstr(destination, "https://") != NULL) {
			//info("PUT %s (%d bytes)", destination, len);
			success = put_data(destination, data, len, content_type,
					   width, height);
		}
	}
#else
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
#endif
	if (destination_type == SETTING_DESTINATION_FOLDER_ID) {
		FILE *of = fopen(destination, "rb");

		if (of != NULL) {
			fclose(of);
		}
#ifdef _WIN32
		else
#endif
		{
			time_t nowunixtime = time(NULL);
			struct tm *nowtime = localtime(&nowunixtime);
			char _file_destination[260];
			char file_destination[260];

			int dest_length = snprintf(
				_file_destination, 259,
				"%s/%d-%02d-%02d_%02d-%02d-%02d", destination,
				nowtime->tm_year + 1900, nowtime->tm_mon + 1,
				nowtime->tm_mday, nowtime->tm_hour,
				nowtime->tm_min, nowtime->tm_sec);

			int repeat_count = 0;
			while (true) {
				if (repeat_count > 5) {
					break;
				}
				dest_length = snprintf(file_destination, 259,
						       "%s", _file_destination);
				if (repeat_count > 0) {
					dest_length = snprintf(
						file_destination, 259,
						"%s_%d.raw", _file_destination,
						repeat_count);
				}
				repeat_count++;

				if (!strcmp(content_type, "image/jpeg")) {
					if (strlen(file_destination)+4 < sizeof(file_destination))
						strcat(file_destination, ".jpg");
				}

				if (dest_length <= 0) {
					break;
				}

				of = fopen(file_destination, "rb");
				if (of != NULL) {
					fclose(of);
					blog(LOG_ERROR, "write_data: file exists %s", file_destination);
					continue;
				}

				of = fopen(file_destination, "wb");
				if (of == NULL) {
					blog(LOG_ERROR, "write_data: cannot open %s", file_destination);
					continue;
				} else {
					fwrite(data, 1, len, of);
					fclose(of);
					success = true;
					break;
				}
			}
		}
	}

	return success;
}

#ifdef HAS_PUT
static bool put_data(char *url, uint8_t *buf, size_t len, char *content_type,
		     int width, int height)
{
	bool success = false;

	char *host_start = strstr(url, "://");
	if (host_start == NULL)
		return false;
	host_start += 2;

	char *host_end;
	char *port_start = strchr(host_start, ':');
	char *location_start = strchr(host_start, '/');
	if (!location_start)
		location_start = "";

	int port;
	if (port_start != NULL) {
		// have port specifier
		host_end = port_start;

		char port_str[16] = {0};
		strncat(port_str, port_start + 1,
			min(sizeof(port_str) - 1,
			    (location_start - host_end) - 1));
		if (strlen(port_str) == 0)
			return false;
		port = atoi(port_str);

	} else {
		char *https = strstr(url, "https");
		if (https == NULL || https > host_start) {
			port = 443;

			// unsupported
			warn("https unsupported");
			return false;
		} else {
			port = 80;
		}
		host_end = location_start;
	}

	char host[128] = {0};
	strncat(host, host_start, min(sizeof(host) - 1, host_end - host_start));
	char *location = location_start;

	HINTERNET hIntrn = InternetOpenA(
		"OBS Screenshot Plugin/1.2.1",
		INTERNET_OPEN_TYPE_PRECONFIG_WITH_NO_AUTOPROXY, NULL, NULL, 0);
	if (!hIntrn)
		goto err_internet_open;

	HINTERNET hConn = InternetConnectA(hIntrn, host, port, NULL, NULL,
					   INTERNET_SERVICE_HTTP, 0, NULL);
	if (!hConn)
		goto err_internet_connect;

	DWORD dwOpenRequestFlags = INTERNET_FLAG_KEEP_CONNECTION |
				   INTERNET_FLAG_NO_COOKIES |
				   INTERNET_FLAG_NO_CACHE_WRITE |
				   INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD;

	HINTERNET hReq = HttpOpenRequestA(hConn, "PUT", location, "HTTP/1.1",
					  NULL, NULL, dwOpenRequestFlags, NULL);
	if (!hReq)
		goto err_http_open_request;

	char header[1024] = {0};
	snprintf(header, sizeof(header), "%sContent-Type: %s\r\n", header,
		 content_type);
	snprintf(header, sizeof(header), "%sImage-Width: %d\r\n", header,
		 width);
	snprintf(header, sizeof(header), "%sImage-Height: %d\r\n", header,
		 height);

	if (HttpSendRequestA(hReq, header, strnlen(header, sizeof(header)), buf,
			     len)) {
		success = true;
		info("Uploaded file to %s:%d%s", host, port, location);
	} else {
		warn("Failed to upload file to http://%s:%d%s - %d", host, port,
		     location, GetLastError());
	}

	InternetCloseHandle(hReq);

err_http_open_request:
	InternetCloseHandle(hConn);

err_internet_connect:
	InternetCloseHandle(hIntrn);

err_internet_open:
	// nothing to close

	return success;
}
#endif

static void capture_key_callback(void *data, obs_hotkey_id id,
				 obs_hotkey_t *key, bool pressed)
{
	UNUSED_PARAMETER(key);
	struct screenshot_filter_data *filter = data;
	const char *filter_name = obs_source_get_name(filter->context);
	info("Got capture_key pressed for %s, id: %d, pressed: %d",
	     filter_name, (int)id, (int)pressed);

	if (id != filter->capture_hotkey_id || !pressed)
		return;

	info("Triggering capture");
	WaitForSingleObject(filter->mutex, INFINITE);
	filter->capture = true;
	ReleaseMutex(filter->mutex);
}

static
struct obs_source_info screenshot_filter = {
	.id = "screenshot_filter_jpeg",

	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = screenshot_filter_get_name,

	.get_properties = screenshot_filter_properties,
	.get_defaults = screenshot_filter_defaults,
	.update = screenshot_filter_update,

	.show = screenshot_filter_shown,
	.activate = screenshot_filter_activated,

	.create = screenshot_filter_create,
	.destroy = screenshot_filter_destroy,

	.save = screenshot_filter_save,
	.load = screenshot_filter_load,

	.video_tick = screenshot_filter_tick,
	.video_render = screenshot_filter_render,

	.filter_remove = screenshot_filter_remove};

bool obs_module_load(void)
{
	obs_register_source(&screenshot_filter);
	return true;
}
