#include <wayfire/view.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workspace-manager.hpp>

class wayfire_wm_actions_t : public wf::plugin_interface_t
{
    nonstd::observer_ptr<wf::sublayer_t> always_above;
    wf::activator_callback on_toggle_above =
        [=](wf::activator_source_t, uint32_t) -> bool
    {
        if (!output->can_activate_plugin(this->grab_interface))
            return false;

        auto view = output->get_active_view();
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
            return false;

        auto always_on_top_views =
            output->workspace->get_views_in_sublayer(always_above);
        auto it = std::find(
            always_on_top_views.begin(), always_on_top_views.end(), view);

        if (it != always_on_top_views.end()) {
            output->workspace->add_view(view,
                (wf::layer_t)output->workspace->get_view_layer(view));
        } else {
            output->workspace->add_view_to_sublayer(view, always_above);
        }

        return true;
    };

    wf::option_wrapper_t<wf::activatorbinding_t> toggle_above{"wm-actions/toggle_always_on_top"};

  public:
    void init() override
    {
        always_above = output->workspace->create_sublayer(
            wf::LAYER_WORKSPACE, wf::SUBLAYER_DOCKED_ABOVE);
        output->add_activator(toggle_above, &on_toggle_above);
    }

    void fini() override
    {
        output->workspace->destroy_sublayer(always_above);
        output->rem_binding(&on_toggle_above);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_wm_actions_t);
