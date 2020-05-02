#include "wayfire/output.hpp"
#include "wayfire/core.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/render-manager.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/util.hpp"
#include "../output/output-impl.hpp"
#include <xf86drmMode.h>
#include <sstream>
#include <cstring>
#include <unordered_set>

#include <wayfire/util/log.hpp>

extern "C"
{
#include <wlr/config.h>
#define static
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#if WLR_HAS_X11_BACKEND
    #include <wlr/backend/x11.h>
#endif
#include <wlr/backend/noop.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/render/wlr_renderer.h>
#undef static
}

static wl_output_transform get_transform_from_string(std::string transform)
{
    if (transform == "normal") return WL_OUTPUT_TRANSFORM_NORMAL;
    else if (transform == "90") return WL_OUTPUT_TRANSFORM_90;
    else if (transform == "180") return WL_OUTPUT_TRANSFORM_180;
    else if (transform == "270") return WL_OUTPUT_TRANSFORM_270;
    else if (transform == "flipped") return WL_OUTPUT_TRANSFORM_FLIPPED;
    else if (transform == "180_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_180;
    else if (transform == "90_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_90;
    else if (transform == "270_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_270;

    LOGE("Bad output transform in config: ", transform);
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

std::pair<wlr_output_mode, bool> parse_output_mode(std::string modeline)
{
    wlr_output_mode mode;
    /* If mode refresh is invalid, then it will be autodetected */
    mode.refresh = 0;
    int read = std::sscanf(modeline.c_str(), "%d x %d @ %d",
        &mode.width, &mode.height, &mode.refresh);

    if (mode.refresh < 1000)
        mode.refresh *= 1000;

    if (read < 2 || mode.width <= 0 || mode.height <= 0 || mode.refresh < 0)
        return {mode, false};

    return {mode, true};
}

std::pair<wf::point_t, bool> parse_output_layout(std::string layout)
{
    wf::point_t pos;
    int read;

    if (layout.find("@") != layout.npos)
        read = std::sscanf(layout.c_str(), "%d @ %d", &pos.x, &pos.y);
    else
        read = std::sscanf(layout.c_str(), "%d , %d", &pos.x, &pos.y);

    if (read < 2)
    {
        LOGE("Detected invalid layout in config: ", layout);
        return {{0, 0}, false};
    }

    return {pos, true};
}

wlr_output_mode *find_matching_mode(wlr_output *output,
    const wlr_output_mode& reference)
{
    wlr_output_mode *mode;
    wlr_output_mode *best = NULL;
    wl_list_for_each(mode, &output->modes, link)
    {
        if (mode->width == reference.width && mode->height == reference.height)
        {
            if (mode->refresh == reference.refresh)
                return mode;

            if (!best || best->refresh < mode->refresh)
                best = mode;
        }
    }

    return best;
}

// from rootston
static bool parse_modeline(const char *modeline, drmModeModeInfo &mode)
{
    char hsync[16];
    char vsync[16];
    float fclock;

    std::memset(&mode, 0, sizeof(mode));
    mode.type = DRM_MODE_TYPE_USERDEF;

    if (sscanf(modeline, "%f %hd %hd %hd %hd %hd %hd %hd %hd %15s %15s",
               &fclock, &mode.hdisplay, &mode.hsync_start, &mode.hsync_end,
               &mode.htotal, &mode.vdisplay, &mode.vsync_start, &mode.vsync_end,
               &mode.vtotal, hsync, vsync) != 11) {
        return false;
    }

    mode.clock = fclock * 1000;
    mode.vrefresh = mode.clock * 1000.0 * 1000.0 / mode.htotal / mode.vtotal;
    if (strcasecmp(hsync, "+hsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_PHSYNC;
    } else if (strcasecmp(hsync, "-hsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_NHSYNC;
    } else {
        return false;
    }

    if (strcasecmp(vsync, "+vsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_PVSYNC;
    } else if (strcasecmp(vsync, "-vsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_NVSYNC;
    } else {
        return false;
    }

    snprintf(mode.name, sizeof(mode.name), "%dx%d@%d",
             mode.hdisplay, mode.vdisplay, mode.vrefresh / 1000);

    return true;
}

namespace wf
{
    void transfer_views(wf::output_t *from, wf::output_t *to)
    {
        assert(from);

        LOGI("transfer views from ", from->handle->name, " -> ",
            to ? to->handle->name : "null");
        /* first move each desktop view(e.g windows) to another output */
        std::vector<wayfire_view> views;
        if (to)
        {
            /* If we aren't moving to another output, then there is no need to
             * enumerate views either */
            views = from->workspace->get_views_in_layer(
                wf::WM_LAYERS & (~wf::LAYER_UNMANAGED));
            std::reverse(views.begin(), views.end());
        }

        for (auto& view : views)
            from->workspace->remove_view(view);

        /* views would be empty if !to, but clang-analyzer detects null deref */
        if (to)
        {
            for (auto& view : views)
            {
                wf::get_core().move_view_to_output(view, to);
                to->workspace->move_to_workspace(view,
                    to->workspace->get_current_workspace());

                if (view->tiled_edges)
                    view->tile_request(view->tiled_edges);

                if (view->fullscreen)
                    view->fullscreen_request(to, true);

                if (!view->fullscreen && !view->tiled_edges && view->is_mapped())
                {
                    auto geometry = wf::clamp(view->get_wm_geometry(),
                        to->workspace->get_workarea());
                    view->set_geometry(geometry);
                }
            }
        }

        /* just remove all other views - backgrounds, panels, etc.
         * desktop views have been removed by the previous cycle */
        for (auto& view : from->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            if (view->is_mapped())
                view->close();

            view->set_output(nullptr);
        }
        /* A note: at this point, some views might already have been deleted */
    }

    constexpr wf::point_t output_state_t::default_position;
    bool output_state_t::operator == (const output_state_t& other) const
    {
        if (source == OUTPUT_IMAGE_SOURCE_NONE)
            return other.source == OUTPUT_IMAGE_SOURCE_NONE;

        if (source == OUTPUT_IMAGE_SOURCE_MIRROR)
        {
            return other.source == OUTPUT_IMAGE_SOURCE_MIRROR &&
                mirror_from == other.mirror_from;
        }

        bool eq = true;

        eq &= source == other.source;
        eq &= position == other.position;
        eq &= (mode.width == other.mode.width);
        eq &= (mode.height == other.mode.height);
        eq &= (mode.refresh == other.mode.refresh);
        eq &= (transform == other.transform);
        eq &= (scale == other.scale);

        return eq;
    }

    /** Represents a single output in the output layout */
    struct output_layout_output_t
    {
        wlr_output *handle;
        output_state_t current_state;

        std::unique_ptr<wf::output_impl_t> output;
        wl_listener_wrapper on_destroy, on_mode;
        std::shared_ptr<wf::config::option_base_t>
            mode_opt, position_opt, scale_opt, transform_opt;
        const std::string default_value = "default";


        void initialize_config_options()
        {
            std::string output_name = handle->name;
            auto& config = wf::get_core().config;
            if (!config.get_section(output_name))
            {
                config.merge_section(
                    std::make_shared<wf::config::section_t> (output_name));
            }

            auto section = config.get_section(output_name);
            auto add_if_missing = [&] (std::string name, std::string defval)
            {
                if (!section->get_option_or(name))
                {
                    section->register_new_option(std::make_shared<
                        wf::config::option_t<std::string>> (name, defval));
                }

                auto opt = section->get_option(name);
                opt->set_default_value_str(defval);
                return opt;
            };

            mode_opt = add_if_missing("mode", default_value);
            scale_opt = add_if_missing("scale", "1.0");
            position_opt = add_if_missing("layout", default_value);
            transform_opt = add_if_missing("transform", "normal");
        }

        output_layout_output_t(wlr_output *handle)
        {
            this->handle = handle;
            on_destroy.connect(&handle->events.destroy);
            initialize_config_options();

            bool is_nested_compositor = wlr_output_is_wl(handle);

#if WLR_HAS_X11_BACKEND
            is_nested_compositor |= wlr_output_is_x11(handle);
#endif
            if (is_nested_compositor)
            {
                /* Nested backends can be resized by the user. We need to handle
                 * these cases */
                on_mode.set_callback([=] (void *data) {
                    handle_mode_changed();
                });
                on_mode.connect(&handle->events.mode);
            }
        }

        /**
         * Update the current configuration based on the mode set by the
         * backend.
         */
        void handle_mode_changed()
        {
            auto& lmanager = wf::get_core().output_layout;
            auto config = lmanager->get_current_configuration();
            if (config.count(handle) &&
                config[handle].source == OUTPUT_IMAGE_SOURCE_SELF)
            {
                if (output && output->get_screen_size() != get_effective_size())
                {
                    /* mode changed. Apply new configuration. */
                    current_state.mode.width = handle->width;
                    current_state.mode.height = handle->height;
                    current_state.mode.refresh = handle->refresh;
                    this->output->set_effective_size(get_effective_size());
                    this->output->render->damage_whole();
                    emit_configuration_changed(wf::OUTPUT_MODE_CHANGE);
                }
            }
        }

        wlr_output_mode select_default_mode()
        {
            wlr_output_mode *mode;
            wl_list_for_each(mode, &handle->modes, link)
            {
                if (mode->preferred)
                    return *mode;
            }

            /* Couldn't find a preferred mode. Just return the last, which is
             * usually also the "largest" */
            wl_list_for_each_reverse(mode, &handle->modes, link)
                return *mode;

            /* Finally, if there isn't any mode (for ex. wayland backend),
             * try the wlr_output resolution, falling back to 1200x720
             * if width or height is <= 0 */
            wlr_output_mode default_mode;
            auto width = handle->width > 0 ? handle->width : 1200;
            auto height = handle->height > 0 ? handle->height : 720;
            auto refresh = handle->refresh > 0 ? handle->refresh : 60000;

            default_mode.width = width;
            default_mode.height = height;
            default_mode.refresh = refresh;

            return default_mode;
        }

        /* Returns true if mode setting for the given output can succeed */
        bool is_mode_supported(wlr_output_mode query)
        {
            /* DRM doesn't support setting a custom mode, so any supported mode
             * must be found in the mode list */
            if (wlr_output_is_drm(handle))
            {
                wlr_output_mode *mode;
                wl_list_for_each(mode, &handle->modes, link)
                {
                    if (mode->width == query.width && mode->height == query.height)
                        return true;
                }

                return false;
            }

            /* X11 and Wayland backends support setting custom modes */
            return true;
        }

        wlr_output_mode load_mode_from_config()
        {
            wlr_output_mode mode;
            mode.width = -1;

            auto set_mode = mode_opt->get_value_str();
            if (set_mode != "default" && set_mode != "auto")
            {
                auto value = parse_output_mode(set_mode);
                if (value.second)
                {
                    if (is_mode_supported(value.first)) {
                        mode = value.first;
                    } else {
                        LOGE("Output mode ", set_mode, " for output ",
                            handle->name, " is not supported, ",
                            " try adding a custom mode.");
                    }
                }
            }

            /* Nothing usable in config, try to pick a default mode */
            if (mode.width < 0)
                mode = select_default_mode();

            return mode;
        }

        output_state_t load_state_from_config()
        {
            output_state_t state;

            state.position = output_state_t::default_position;
            auto set_position = position_opt->get_value_str();
            if (set_position != default_value)
            {
                auto value = parse_output_layout(set_position);
                if (value.second)
                    state.position = value.first;
            }

            /* Make sure we can use custom modes that are
             * specified in the config */
            refresh_custom_modes();

            std::string set_mode = mode_opt->get_value_str();
            if (set_mode == "off")
            {
                state.source = OUTPUT_IMAGE_SOURCE_NONE;
                return state;
            }
            else if (set_mode.find("mirror") == 0)
            {
                state.source = OUTPUT_IMAGE_SOURCE_MIRROR;

                std::stringstream ss(set_mode);
                ss >> state.mirror_from; // skip the mirror word
                ss >> state.mirror_from;

                state.mode = select_default_mode();
            }
            else
            {
                state.source = OUTPUT_IMAGE_SOURCE_SELF;
                state.mode = load_mode_from_config();
            }

            auto set_scale = wf::option_type::from_string<double>(
                scale_opt->get_value_str());
            if (set_scale.value_or(-1) <= 0)
            {
                LOGE("Invalid scale for ", handle->name, " in config: ",
                    scale_opt->get_value_str());
                state.scale = 1;
            } else
            {
                state.scale = set_scale.value();
            }

            state.transform = get_transform_from_string(
                transform_opt->get_value_str());
            return state;
        }

        void ensure_wayfire_output(const wf::dimensions_t& effective_size)
        {
            if (this->output)
            {
                this->output->set_effective_size(effective_size);
                return;
            }

            this->output =
                std::make_unique<wf::output_impl_t> (handle, effective_size);
            auto wo = output.get();

            /* Focus the first output, but do not change the focus on subsequently
             * added outputs. We also change the focus if the noop output was
             * focused */
            wlr_output *focused = get_core().get_active_output() ?
                get_core().get_active_output()->handle : nullptr;
            if (!focused || wlr_output_is_noop(focused))
                get_core().focus_output(wo);

            /*
             * At this point, this->output is a valid output and is part of the
             * get_outputs() list.
             *
             * We have also have updated the focused output. So, at this point
             * all plugin-relevant structures have been updated.
             */
            this->output->start_plugins();

            output_added_signal data;
            data.output = wo;
            get_core().output_layout->emit_signal("output-added", &data);
        }

        void destroy_wayfire_output(bool shutdown)
        {
            if (!this->output)
                return;

            LOGE("disabling output: ", output->handle->name);

            auto wo = output.get();
            output_removed_signal data;
            data.output = wo;

            wo->emit_signal("pre-remove", &data);
            get_core().output_layout->emit_signal("output-pre-remove", &data);

            if (get_core().get_active_output() == wo && !shutdown)
            {
                get_core().focus_output(
                    get_core().output_layout->get_next_output(wo));
            } else if (shutdown) {
                get_core().focus_output(nullptr);
            }

            /* It doesn't make sense to transfer to another output if we're
             * going to shut down the compositor */
            transfer_views(wo,
                shutdown ? nullptr : get_core().get_active_output());
            get_core().output_layout->emit_signal("output-removed", &data);
            this->output = nullptr;
        }

        std::unordered_set<std::string> added_custom_modes;
        void add_custom_mode(std::string modeline)
        {
            if (added_custom_modes.count(modeline))
                return;

            added_custom_modes.insert(modeline);
            drmModeModeInfo *mode = new drmModeModeInfo;
            if (!parse_modeline(modeline.c_str(), *mode))
            {
                LOGE("invalid modeline ", modeline, " in config file");
                return;
            }

            LOGD("output ", handle->name, ": adding custom mode ", mode->name);
            if (wlr_output_is_drm(handle))
                wlr_drm_connector_add_mode(handle, mode);
        }

        void refresh_custom_modes()
        {
            static const std::string custom_mode_prefix = "custom_mode";
            auto section = get_core().config.get_section(handle->name);
            if (!section)
                return;

            for (auto& opt : section->get_registered_options())
            {
                if (custom_mode_prefix ==
                    opt->get_name().substr(0, custom_mode_prefix.length()))
                {
                    add_custom_mode(opt->get_value_str());
                }
            }
        }

        /** Check whether the given state can be applied */
        bool test_state(const output_state_t& state)
        {
            if (state.source == OUTPUT_IMAGE_SOURCE_NONE)
                return true;

            if (state.source == OUTPUT_IMAGE_SOURCE_MIRROR)
                return true;

            /* XXX: are there more things to check? */
            refresh_custom_modes();
            return is_mode_supported(state.mode);
        }

        /** Change the output mode */
        void apply_mode(const wlr_output_mode& mode)
        {
            if (handle->current_mode)
            {
                /* Do not modeset if nothing changed */
                if (handle->current_mode->width == mode.width &&
                    handle->current_mode->height == mode.height &&
                    handle->current_mode->refresh == mode.refresh)
                {
                    /* Commit the enabling of the output */
                    wlr_output_commit(handle);
                    return;
                }
            }

            refresh_custom_modes();
            auto built_in = find_matching_mode(handle, mode);
            if (built_in)
            {
                wlr_output_set_mode(handle, built_in);
            } else
            {
                LOGI("Couldn't find matching mode ",
                    mode.width, "x", mode.height, "@", mode.refresh / 1000.0,
                    " for output ", handle->name, ". Trying to use custom mode",
                    "(might not work)");

                wlr_output_set_custom_mode(handle, mode.width, mode.height,
                    mode.refresh);
            }

            wlr_output_commit(handle);
        }

        /* Mirroring implementation */
        wl_listener_wrapper on_mirrored_frame;
        wl_listener_wrapper on_frame;
        wlr_output *locked_cursors_on = NULL;

        /** Render the output using texture as source */
        void render_output(wlr_texture *texture)
        {
            auto renderer = get_core().renderer;
            wlr_output_attach_render(handle, NULL);
            wlr_renderer_begin(renderer, handle->width, handle->height);

            /* Project a box filling the whole screen */
            float projection[9], box[9];
            wlr_matrix_projection(projection, handle->width, handle->height,
                WL_OUTPUT_TRANSFORM_NORMAL);

            wlr_box geometry = {0, 0, handle->width, handle->height};
            wlr_matrix_project_box(box, &geometry, WL_OUTPUT_TRANSFORM_NORMAL,
                0.0, projection);

            wlr_render_texture_with_matrix(renderer, texture, box, 1.0);
            wlr_renderer_end(renderer);
            wlr_output_commit(handle);
        }

        /* Load output contents and render them */
        void handle_frame()
        {
            auto wo = get_core().output_layout->find_output(
                current_state.mirror_from);
            if (!wo)
            {
                LOGE("Cannot find mirrored output ", current_state.mirror_from);
                return;
            }

            wlr_dmabuf_attributes attributes;
            if (!wlr_output_export_dmabuf(wo->handle, &attributes))
            {
                LOGE("Failed reading mirrored output contents");
                return;
            }

            /* We export the output to mirror from to a dmabuf, then create
             * a texture from this and use it to render "our" output */
            auto texture = wlr_texture_from_dmabuf(
                get_core().renderer, &attributes);
            render_output(texture);

            wlr_texture_destroy(texture);
            wlr_dmabuf_attributes_finish(&attributes);
        }

        void set_enabled(bool enabled)
        {
            if (wlr_output_is_noop(handle))
                return;

            wlr_output_enable(handle, enabled);
            if (!enabled)
                wlr_output_commit(handle);
        }

        void setup_mirror()
        {
            /* Check if we can mirror */
            auto wo = get_core().output_layout->find_output(
                current_state.mirror_from);

            bool mirror_active = (wo != nullptr);
            if (wo)
            {
                auto config =
                    get_core().output_layout->get_current_configuration();
                auto& wo_state = config[wo->handle];

                if (wo_state.source & OUTPUT_IMAGE_SOURCE_NONE)
                    mirror_active = false;
            }

            if (!mirror_active)
            {
                /* If we mirror from a DPMS or an OFF output, we should turn
                 * off this output as well */
                set_enabled(false);
                LOGI(handle->name, ": Cannot mirror from output ",
                    current_state.mirror_from, ". Disabling output.");
                return;
            }

            /* Force software cursors on the mirrored from output.
             * This ensures that they will be copied when reading pixels
             * from the main plane */
            wlr_output_lock_software_cursors(wo->handle, true);
            locked_cursors_on = wo->handle;

            wlr_output_schedule_frame(handle);
            on_mirrored_frame.set_callback([=] (void*) {
                /* The mirrored output was repainted, schedule repaint
                 * for us as well */
                wlr_output_schedule_frame(handle);
            });
            on_mirrored_frame.connect(&wo->handle->events.precommit);

            on_frame.set_callback([=] (void*) { handle_frame(); });
            on_frame.connect(&handle->events.frame);
        }

        void teardown_mirror()
        {
            if (locked_cursors_on)
            {
                wlr_output_lock_software_cursors(locked_cursors_on, false);
                locked_cursors_on = NULL;
            }

            on_mirrored_frame.disconnect();
            on_frame.disconnect();
        }

        wf::dimensions_t get_effective_size()
        {
            wf::dimensions_t effective_size;
            wlr_output_effective_resolution(handle,
                &effective_size.width, &effective_size.height);
            return effective_size;
        }

        /**
         * Send the output-configuration-changed signal.
         */
        void emit_configuration_changed(uint32_t changed_fields)
        {
            if (!wlr_output_is_noop(handle) && changed_fields)
            {
                wf::output_configuration_changed_signal data{current_state};
                data.output = output.get();
                data.changed_fields = changed_fields;
                output->emit_signal("output-configuration-changed", &data);
            }
        }

        /** Apply the given state to the output, ignoring position.
         *
         * This won't have any effect if the output state can't be applied,
         * i.e if test_state(state) == false */
        void apply_state(const output_state_t& state, bool is_shutdown = false)
        {
            if (!test_state(state))
                return;

            uint32_t changed_fields = 0;
            if (this->current_state.source != state.source)
                changed_fields |= wf::OUTPUT_SOURCE_CHANGE;
            if (this->current_state.mode.width != state.mode.width ||
                this->current_state.mode.height != state.mode.height ||
                this->current_state.mode.refresh != state.mode.refresh)
            {
                changed_fields |= wf::OUTPUT_MODE_CHANGE;
            }
            if (this->current_state.scale != state.scale)
                changed_fields |= wf::OUTPUT_SCALE_CHANGE;
            if (this->current_state.transform != state.transform)
                changed_fields |= wf::OUTPUT_TRANSFORM_CHANGE;

            this->current_state = state;

            /* Even if output will remain mirrored, we can tear it down and set
             * up again, in case the output to mirror from changed */
            teardown_mirror();

            if (state.source == OUTPUT_IMAGE_SOURCE_NONE)
            {
                /* output is OFF */
                destroy_wayfire_output(is_shutdown);
                set_enabled(false);
                return;
            }

            set_enabled(!(state.source & OUTPUT_IMAGE_SOURCE_NONE));
            apply_mode(state.mode);
            if (state.source & OUTPUT_IMAGE_SOURCE_SELF)
            {
                if (handle->transform != state.transform)
                    wlr_output_set_transform(handle, state.transform);

                if (handle->scale != state.scale)
                    wlr_output_set_scale(handle, state.scale);

                wlr_output_commit(handle);

                ensure_wayfire_output(get_effective_size());
                output->render->damage_whole();
                emit_configuration_changed(changed_fields);
            }
            else /* state.source == OUTPUT_IMAGE_SOURCE_MIRROR */
            {
                destroy_wayfire_output(is_shutdown);
                setup_mirror();
            }
        }

    };

    class output_layout_t::impl
    {
        std::map<wlr_output*, std::unique_ptr<output_layout_output_t>> outputs;

        wlr_output_layout *output_layout;
        wlr_output_manager_v1 *output_manager;

        wl_listener_wrapper on_new_output;
        wl_listener_wrapper on_output_manager_test;
        wl_listener_wrapper on_output_manager_apply;
        wl_idle_call idle_init_noop;
        wl_idle_call idle_update_configuration;
        wl_timer timer_remove_noop;

        wlr_backend *noop_backend;
        /* Wayfire generally assumes that an enabled output is always available.
         * However, when switching connectors or something it might happen that
         * temporarily no output is available. For those cases, we create a
         * virtual output with the noop backend. */
        std::unique_ptr<output_layout_output_t> noop_output;

        bool shutdown_received = false;
        signal_callback_t on_config_reload, on_shutdown;

        public:
        impl(wlr_backend *backend)
        {
            on_new_output.set_callback([=] (void *data) {
                add_output((wlr_output*) data);
            });
            on_new_output.connect(&backend->events.new_output);

            output_layout = wlr_output_layout_create();

            on_config_reload = [=] (void*) { reconfigure_from_config(); };
            get_core().connect_signal("reload-config", &on_config_reload);
            on_shutdown = [=] (void*) {
                /* Disconnect timer, since otherwise it will be destroyed
                 * after the wayland display is. */
                this->timer_remove_noop.disconnect();
                shutdown_received = true;
            };
            get_core().connect_signal("shutdown", &on_shutdown);

            noop_backend = wlr_noop_backend_create(get_core().display);
            wlr_backend_start(noop_backend);
            /* The noop output will be typically destroyed on the first
             * plugged monitor, however we need to create it here so that we
             * support booting with 0 monitors */
            idle_init_noop.run_once([&] () {
                if (get_outputs().empty())
                    ensure_noop_output();
            });

            output_manager = wlr_output_manager_v1_create(get_core().display);
            on_output_manager_test.set_callback([=] (void *data) {
                apply_wlr_configuration((wlr_output_configuration_v1*) data, true);
            });
            on_output_manager_apply.set_callback([=] (void *data) {
                apply_wlr_configuration((wlr_output_configuration_v1*) data, false);
            });

            on_output_manager_test.connect(&output_manager->events.test);
            on_output_manager_apply.connect(&output_manager->events.apply);
        }

        ~impl()
        {
            if (noop_output)
                noop_output->destroy_wayfire_output(true);
            wlr_backend_destroy(noop_backend);

            get_core().disconnect_signal("reload-config", &on_config_reload);
        }

        output_configuration_t output_configuration_from_wlr_configuration(
            wlr_output_configuration_v1 *configuration)
        {
            output_configuration_t result;
            wlr_output_configuration_head_v1 *head;
            wl_list_for_each(head, &configuration->heads, link)
            {
                if (!this->outputs.count(head->state.output))
                {
                    LOGE("Output configuration request contains unknown",
                        " output, probably a compositor bug!");
                    continue;
                }

                auto& handle = head->state.output;
                auto& state = result[handle];

                if (!head->state.enabled)
                {
                    state.source = OUTPUT_IMAGE_SOURCE_NONE;
                    continue;
                }

                state.source = OUTPUT_IMAGE_SOURCE_SELF;
                state.mode = head->state.mode ? *head->state.mode :
                    this->outputs[handle]->current_state.mode;
                state.position = {head->state.x, head->state.y};
                state.scale = head->state.scale;
                state.transform = head->state.transform;
            }

            return result;
        }

        void apply_wlr_configuration(
            wlr_output_configuration_v1 *wlr_configuration, bool test_only)
        {
            auto configuration =
                output_configuration_from_wlr_configuration(wlr_configuration);

            if (apply_configuration(configuration, test_only)) {
                wlr_output_configuration_v1_send_succeeded(wlr_configuration);
            } else {
                wlr_output_configuration_v1_send_failed(wlr_configuration);
            }
        }

        void ensure_noop_output()
        {
            LOGI("new output: NOOP-1");

            if (!noop_output)
            {
                auto handle = wlr_noop_add_output(noop_backend);
                noop_output = std::make_unique<output_layout_output_t> (handle);
            }

            /* Make sure that the noop output is up and running even before the
             * next reconfiguration. This is needed because if we are removing
             * an output, we might get into a situation where the last physical
             * output has already been removed but we are yet to add the noop one */
            noop_output->apply_state(noop_output->load_state_from_config());
            wlr_output_layout_add_auto(output_layout, noop_output->handle);
            timer_remove_noop.disconnect();
        }

        void remove_noop_output()
        {
            if (!noop_output)
                return;

            if (noop_output->current_state.source == OUTPUT_IMAGE_SOURCE_NONE)
                return;

            LOGI("remove output: NOOP-1");

            output_state_t state;
            state.source = OUTPUT_IMAGE_SOURCE_NONE;
            noop_output->apply_state(state);
            wlr_output_layout_remove(output_layout, noop_output->handle);
        }

        void add_output(wlr_output *output)
        {
            LOGI("new output: ", output->name);

            auto lo = new output_layout_output_t(output);
            outputs[output] = std::unique_ptr<output_layout_output_t>(lo);
            lo->on_destroy.set_callback([output, this] (void*) {
                remove_output(output);
            });

            reconfigure_from_config();
        }

        void remove_output(wlr_output *to_remove)
        {
            auto active_outputs = get_outputs();
            LOGI("remove output: ", to_remove->name);

            /* Unset mode, plus destroy the wayfire output */
            auto configuration = get_current_configuration();
            configuration[to_remove].source = OUTPUT_IMAGE_SOURCE_NONE;
            apply_configuration(configuration);

            outputs.erase(to_remove);

            /* If no physical outputs, then at least the noop output */
            assert(get_outputs().size() || shutdown_received);
        }

        /* Get the current configuration of all outputs */
        output_configuration_t get_current_configuration()
        {
            output_configuration_t configuration;
            for (auto& entry : this->outputs)
                configuration[entry.first] = entry.second->current_state;

            return configuration;
        }

        output_configuration_t last_config_configuration;

        /** Load config from file, test and apply */
        void reconfigure_from_config()
        {
            /* Load from config file */
            output_configuration_t configuration;
            for (auto& entry : this->outputs)
                configuration[entry.first] = entry.second->load_state_from_config();

            if (configuration == get_current_configuration() ||
                configuration == last_config_configuration)
            {
                return;
            }

            if (test_configuration(configuration))
                apply_configuration(configuration);
        }

        /** Check whether the given configuration can be applied */
        bool test_configuration(const output_configuration_t& config)
        {
            if (config.size() != this->outputs.size())
                return false;

            bool ok = true;
            for (auto& entry : config)
            {
                if (this->outputs.count(entry.first) == 0)
                    return false;

                ok &= this->outputs[entry.first]->test_state(entry.second);
            }

            /* TODO: reject overlapping outputs */
            /* TODO: possibly reject disjoint outputs? Note: this doesn't apply
             * if an output was destroyed */
            /* TODO: reject no enabled output setups */
            /* TODO: reject bad mirror configurations */
            return ok;
        }

        /** Apply the given configuration. Config MUST be a valid configuration */
        void apply_configuration(const output_configuration_t& config)
        {
            /* The order in which we enable and disable outputs is important.
             * Firstly, on some systems where there aren't enough CRTCs, we can
             * only enable a subset of all outputs at once. This means we should
             * first try to disable as many outputs as possible, and only then
             * start enabling new ones.
             *
             * Secondly, we need to check when we need to enable noop output -
             * which is exactly when all currently enabled outputs are going to
             * be disabled */

            /* Number of outputs that were enabled and continue to be enabled */
            int count_remaining_enabled = 0;
            auto active_outputs = get_outputs();
            for (auto& wo : active_outputs)
            {
                auto it = config.find(wo->handle);
                if (it != config.end() &&
                    (it->second.source & OUTPUT_IMAGE_SOURCE_SELF))
                {
                    ++count_remaining_enabled;
                }
            }

            bool turning_off_all_active =
                !active_outputs.empty() && count_remaining_enabled == 0;
            bool is_noop_active = noop_output && noop_output->output;

            if (turning_off_all_active && !shutdown_received && !is_noop_active)
            {
                /* If we aren't shutting down, and we will turn off all the
                 * currently enabled outputs, we'll need the noop output, as a
                 * temporary output to store views in, until a real output is
                 * enabled again */
                ensure_noop_output();
            }

            /* First: disable all outputs that need disabling */
            for (auto& entry : config)
            {
                auto& handle = entry.first;
                auto& state = entry.second;
                auto& lo = this->outputs[handle];

                if (!(state.source & OUTPUT_IMAGE_SOURCE_SELF))
                {
                    /* First shut down the output, move its views, etc. while it
                     * is still in the output layout and its global is active.
                     *
                     * This is needed so that clients can receive
                     * wl_surface.leave events for the to be destroyed output */
                    lo->apply_state(state, shutdown_received);
                    wlr_output_layout_remove(output_layout, handle);
                }
            }

            /* Second: enable outputs */
            int count_enabled = 0;
            for (auto& entry : config)
            {
                auto& handle = entry.first;
                auto& state = entry.second;
                auto& lo = this->outputs[handle];

                if (state.source & OUTPUT_IMAGE_SOURCE_SELF)
                {
                    ++count_enabled;
                    if (entry.second.position != output_state_t::default_position) {
                        wlr_output_layout_add(output_layout, handle,
                            state.position.x, state.position.y);
                    } else {
                        wlr_output_layout_add_auto(output_layout, handle);
                    }

                    lo->apply_state(state, shutdown_received);
                }
            }

            /* Third: enable mirrored outputs */
            for (auto& entry : config)
            {
                auto& handle = entry.first;
                auto& state = entry.second;
                auto& lo = this->outputs[handle];

                if (state.source == OUTPUT_IMAGE_SOURCE_MIRROR)
                {
                    lo->apply_state(state, shutdown_received);
                    wlr_output_layout_remove(output_layout, handle);
                }
            }

            get_core().output_layout->emit_signal("configuration-changed", nullptr);

            if (count_enabled > 0)
            {
                /* Make sure to remove the noop output if it is no longer needed.
                 * NB: Libwayland has a bug when a global is created and
                 * immediately destroyed, as clients don't have enough time
                 * to bind it. That's why we don't destroy noop immediately,
                 * but only after a timeout */
                timer_remove_noop.set_timeout(1000, [=] () {
                    remove_noop_output();
                });
            }

            idle_update_configuration.run_once([=] () {
                send_wlr_configuration();
            });
        }

        void send_wlr_configuration()
        {
            auto wlr_configuration = wlr_output_configuration_v1_create();
            for (auto& output : outputs)
            {
                auto head = wlr_output_configuration_head_v1_create(
                    wlr_configuration, output.first);

                auto box = wlr_output_layout_get_box(output_layout, output.first);
                if (box)
                {
                    head->state.x = box->x;
                    head->state.y = box->y;
                }
            }

            wlr_output_manager_v1_set_configuration(output_manager,
                wlr_configuration);
        }

        /* Public API functions */
        wlr_output_layout *get_handle() { return output_layout; }
        size_t get_num_outputs() { return get_outputs().size(); }

        wf::output_t *find_output(wlr_output *output)
        {
            if (outputs.count(output))
                return outputs[output]->output.get();

            if (noop_output && noop_output->handle == output)
                return noop_output->output.get();

            return nullptr;
        }

        wf::output_t *find_output(std::string name)
        {
            for (auto& entry : outputs)
            {
                if (entry.first->name == name)
                    return entry.second->output.get();
            }

            if (noop_output && noop_output->handle->name == name)
                return noop_output->output.get();

            return nullptr;
        }

        std::vector<wf::output_t*> get_outputs()
        {
            std::vector<wf::output_t*> result;
            for (auto& entry : outputs)
            {
                if (entry.second->current_state.source & OUTPUT_IMAGE_SOURCE_SELF)
                    result.push_back(entry.second->output.get());
            }

            if (result.empty())
            {
                if (noop_output && noop_output->output)
                    result.push_back(noop_output->output.get());
            }

            return result;
        }

        wf::output_t *get_next_output(wf::output_t *output)
        {
            auto os = get_outputs();

            auto it = std::find(os.begin(), os.end(), output);
            if (it == os.end() || std::next(it) == os.end()) {
                return os[0];
            } else {
                return *(++it);
            }
        }

        wf::output_t *get_output_coords_at(const wf::pointf_t& origin, wf::pointf_t& closest)
        {
            wlr_output_layout_closest_point(output_layout, NULL,
                origin.x, origin.y, &closest.x, &closest.y);

            auto handle = wlr_output_layout_output_at(output_layout, closest.x, closest.y);
            assert(handle || shutdown_received);
            if (!handle)
                return nullptr;

            if (noop_output && handle == noop_output->handle) {
                return noop_output->output.get();
            } else {
                return outputs[handle]->output.get();
            }
        }

        wf::output_t *get_output_at(int x, int y)
        {
            wf::pointf_t dummy;
            return get_output_coords_at({1.0 * x, 1.0 * y}, dummy);
        }

        bool apply_configuration(const output_configuration_t& configuration,
            bool test_only)
        {
            bool ok = test_configuration(configuration);
            if (ok && !test_only)
                apply_configuration(configuration);

            return ok;
        }
    };

    /* Just pass to the PIMPL */
    output_layout_t::output_layout_t(wlr_backend *b) : pimpl(new impl(b)) {}
    output_layout_t::~output_layout_t() = default;
    wlr_output_layout *output_layout_t::get_handle()
    { return pimpl->get_handle(); }
    wf::output_t *output_layout_t::get_output_at(int x, int y)
    { return pimpl->get_output_at(x, y); }
    wf::output_t *output_layout_t::get_output_coords_at(wf::pointf_t origin, wf::pointf_t& closest)
    { return pimpl->get_output_coords_at(origin, closest); }
    size_t output_layout_t::get_num_outputs()
    { return pimpl->get_num_outputs(); }
    std::vector<wf::output_t*> output_layout_t::get_outputs()
    { return pimpl->get_outputs(); }
    wf::output_t *output_layout_t::get_next_output(wf::output_t *output)
    { return pimpl->get_next_output(output); }
    wf::output_t *output_layout_t::find_output(wlr_output *output)
    { return pimpl->find_output(output); }
    wf::output_t *output_layout_t::find_output(std::string name)
    { return pimpl->find_output(name); }
    output_configuration_t output_layout_t::get_current_configuration()
    { return pimpl->get_current_configuration(); }
    bool output_layout_t::apply_configuration(const output_configuration_t& configuration, bool test_only)
    { return pimpl->apply_configuration(configuration, test_only); }
}
