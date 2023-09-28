#pragma once

#if defined(SST_ENABLE_HPX) 

#include <hpx/include/actions.hpp>
#include <hpx/include/components.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/serialization.hpp>

#include <utility>

namespace SST { namespace Comms { namespace Component {

    struct HPX_COMPONENT_EXPORT P2PCommunicator
      : hpx::components::component_base<P2PCommunicator>
    {
        void invoke(const std::string ibuf);
        HPX_DEFINE_COMPONENT_ACTION(CommsComponent, invoke)
    };

    hpx::mutex mut;
    std::string data;

} // namespace Component

HPX_REGISTER_ACTION_DECLARATION(
    Component::P2PCommunicator::invoke_action, comms_component_invoke_action)

struct P2PCommunicator
   : hpx::components::client_base<P2PCommunicator, Component::P2PCommunicator>
{
    using base_type = hpx::components::client_base<P2PCommunicator, Component::P2PCommunicator>;

    P2PCommunicator(hpx::future<hpx::id_type>&& f)
        : base_type(std::move(f))
    {
    }

    P2PCommunicator(hpx::id_type&& f)
        : base_type(std::move(f))
    {
    }

    void invoke(const hpx::id_type id, const std::string ibuf)
    {
            hpx::async<Component::P2PCommunicator::invoke_action>(id, ibuf)
                .get();
    }
};

} /* namespace Comms */ } // namespace SST

#endif
